// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// @file   CATrackerSpec.cxx
/// @author Matthias Richter
/// @since  2018-04-18
/// @brief  Processor spec for running TPC CA tracking

#include "CATrackerSpec.h"
#include "Headers/DataHeader.h"
#include "Framework/WorkflowSpec.h" // o2::framework::mergeInputs
#include "Framework/DataRefUtils.h"
#include "Framework/DataSpecUtils.h"
#include "Framework/ControlService.h"
#include "DataFormatsTPC/TPCSectorHeader.h"
#include "DataFormatsTPC/ClusterGroupAttribute.h"
#include "DataFormatsTPC/ClusterNative.h"
#include "DataFormatsTPC/ClusterNativeHelper.h"
#include "DataFormatsTPC/Helpers.h"
#include "TPCReconstruction/TPCCATracking.h"
#include "TPCBase/Sector.h"
#include "SimulationDataFormat/MCTruthContainer.h"
#include "SimulationDataFormat/MCCompLabel.h"
#include "Algorithm/Parser.h"
#include <FairMQLogger.h>
#include <memory> // for make_shared
#include <vector>
#include <iomanip>
#include <stdexcept>

using namespace o2::framework;
using namespace o2::header;

namespace o2
{
namespace TPC
{

using MCLabelContainer = o2::dataformats::MCTruthContainer<o2::MCCompLabel>;

DataProcessorSpec getCATrackerSpec(bool processMC, std::vector<int> const& inputIds)
{
  constexpr static size_t NSectors = o2::TPC::Sector::MAXSECTOR;
  using ClusterGroupParser = o2::algorithm::ForwardParser<o2::TPC::ClusterGroupHeader>;
  struct ProcessAttributes {
    // the input comes in individual calls and we need to buffer until
    // data set is complete, have to think about a DPL feature to take
    // ownership of an input
    std::array<std::vector<char>, NSectors> bufferedInputs;
    std::array<std::vector<MCLabelContainer>, NSectors> mcInputs;
    std::bitset<NSectors> validInputs = 0;
    std::bitset<NSectors> validMcInputs = 0;
    std::unique_ptr<ClusterGroupParser> parser;
    std::unique_ptr<o2::TPC::TPCCATracking> tracker;
    int verbosity = 1;
    std::vector<int> inputIds;
    bool readyToQuit = false;
  };

  auto initFunction = [processMC, inputIds](InitContext& ic) {
    auto options = ic.options().get<std::string>("tracker-options");

    auto processAttributes = std::make_shared<ProcessAttributes>();
    {
      processAttributes->inputIds = inputIds;
      auto& parser = processAttributes->parser;
      auto& tracker = processAttributes->tracker;
      parser = std::make_unique<ClusterGroupParser>();
      tracker = std::make_unique<o2::TPC::TPCCATracking>();
      if (tracker->initialize(options.c_str()) != 0) {
        throw std::invalid_argument("TPCCATracking initialization failed");
      }
      processAttributes->validInputs.reset();
      processAttributes->validMcInputs.reset();
    }

    auto processingFct = [processAttributes, processMC](ProcessingContext& pc) {
      if (processAttributes->readyToQuit) {
        return;
      }
      auto& parser = processAttributes->parser;
      auto& tracker = processAttributes->tracker;
      uint64_t activeSectors = 0;
      auto& verbosity = processAttributes->verbosity;

      // FIXME cleanup almost duplicated code
      auto& validMcInputs = processAttributes->validMcInputs;
      auto& mcInputs = processAttributes->mcInputs;
      if (processMC) {
        // we can later extend this to multiple inputs
        for (auto const& inputId : processAttributes->inputIds) {
          std::string inputLabel = "mclblin" + std::to_string(inputId);
          auto ref = pc.inputs().get(inputLabel);
          auto const* sectorHeader = DataRefUtils::getHeader<o2::TPC::TPCSectorHeader*>(ref);
          if (sectorHeader == nullptr) {
            // FIXME: think about error policy
            LOG(ERROR) << "sector header missing on header stack";
            return;
          }
          const int& sector = sectorHeader->sector;
          if (sector < 0) {
            continue;
          }
          if (validMcInputs.test(sector)) {
            // have already data for this sector, this should not happen in the current
            // sequential implementation, for parallel path merged at the tracker stage
            // multiple buffers need to be handled
            throw std::runtime_error("can only have one data set per sector");
          }
          mcInputs[sector] = std::move(pc.inputs().get<std::vector<MCLabelContainer>>(inputLabel.c_str()));
          validMcInputs.set(sector);
          activeSectors |= sectorHeader->activeSectors;
          if (verbosity > 1) {
            LOG(INFO) << "received " << *(ref.spec) << " MC label containers"
                      << " for sector " << sector                                      //
                      << std::endl                                                     //
                      << "  mc input status:   " << validMcInputs                      //
                      << std::endl                                                     //
                      << "  active sectors: " << std::bitset<NSectors>(activeSectors); //
          }
        }
      }

      auto& validInputs = processAttributes->validInputs;
      int operation = 0;
      std::map<int, DataRef> datarefs;
      for (auto const& inputId : processAttributes->inputIds) {
        std::string inputLabel = "input" + std::to_string(inputId);
        auto ref = pc.inputs().get(inputLabel);
        auto const* sectorHeader = DataRefUtils::getHeader<o2::TPC::TPCSectorHeader*>(ref);
        if (sectorHeader == nullptr) {
          // FIXME: think about error policy
          LOG(ERROR) << "sector header missing on header stack";
          return;
        }
        const int& sector = sectorHeader->sector;
        // check the current operation, this is used to either signal eod or noop
        // FIXME: the noop is not needed any more once the lane configuration with one
        // channel per sector is used
        if (sector < 0) {
          if (operation < 0 && operation != sector) {
            // we expect the same operation on all inputs
            LOG(ERROR) << "inconsistent lane operation, got " << sector << ", expecting " << operation;
          } else if (operation == 0) {
            // store the operation
            operation = sector;
          }
          continue;
        }
        if (validInputs.test(sector)) {
          // have already data for this sector, this should not happen in the current
          // sequential implementation, for parallel path merged at the tracker stage
          // multiple buffers need to be handled
          throw std::runtime_error("can only have one data set per sector");
        }
        activeSectors |= sectorHeader->activeSectors;
        validInputs.set(sector);
        datarefs[sector] = ref;
      }

      if (operation == -1) {
        // EOD is transmitted in the sectorHeader with sector number equal to -1
        o2::TPC::TPCSectorHeader sh{ -1 };
        sh.activeSectors = activeSectors;
        pc.outputs().snapshot(OutputRef{ "output", 0, { sh } }, -1);
        if (processMC) {
          pc.outputs().snapshot(OutputRef{ "mclblout", 0, { sh } }, -1);
        }
        pc.services().get<ControlService>().readyToQuit(false);
        processAttributes->readyToQuit = true;
        return;
      }
      auto printInputLog = [&verbosity, &validInputs, &activeSectors](auto& r, const char* comment, auto& s) {
        if (verbosity > 1) {
          LOG(INFO) << comment << " " << *(r.spec) << ", size " << DataRefUtils::getPayloadSize(r) //
                    << " for sector " << s                                                         //
                    << std::endl                                                                   //
                    << "  input status:   " << validInputs                                         //
                    << std::endl                                                                   //
                    << "  active sectors: " << std::bitset<NSectors>(activeSectors);               //
        }
      };
      auto& bufferedInputs = processAttributes->bufferedInputs;
      if (activeSectors == 0 || (activeSectors & validInputs.to_ulong()) != activeSectors ||
          (processMC && (activeSectors & validMcInputs.to_ulong()) != activeSectors)) {
        // not all sectors available, we have to buffer the inputs
        for (auto const& refentry : datarefs) {
          auto& sector = refentry.first;
          auto& ref = refentry.second;
          auto payploadSize = DataRefUtils::getPayloadSize(ref);
          bufferedInputs[sector].resize(payploadSize);
          std::copy(ref.payload, ref.payload + payploadSize, bufferedInputs[sector].begin());
          printInputLog(ref, "buffering", sector);
        }

        // not needed to send something, DPL will simply drop this timeslice, whenever the
        // data for all sectors is available, the output is sent in that time slice
        return;
      }
      assert(processMC == false || validMcInputs == validInputs);
      std::array<gsl::span<const char>, NSectors> inputs;
      auto inputStatus = validInputs;
      for (auto const& refentry : datarefs) {
        auto& sector = refentry.first;
        auto& ref = refentry.second;
        inputs[sector] = gsl::span(ref.payload, DataRefUtils::getPayloadSize(ref));
        inputStatus.reset(sector);
        printInputLog(ref, "received", sector);
      }
      if (inputStatus.any()) {
        // some of the inputs have been buffered
        for (size_t sector = 0; sector < inputStatus.size(); ++sector) {
          if (inputStatus.test(sector)) {
            inputs[sector] = gsl::span(&bufferedInputs[sector].front(), bufferedInputs[sector].size());
          }
        }
      }
      if (verbosity > 0) {
        if (inputStatus.any()) {
          LOG(INFO) << "using buffered data for " << inputStatus.count() << " sector(s)";
        }
        // make human readable information from the bitfield
        std::string bitInfo;
        auto nActiveBits = validInputs.count();
        if (((uint64_t)0x1 << nActiveBits) == validInputs.to_ulong() + 1) {
          // sectors 0 to some upper bound are active
          bitInfo = "0-" + std::to_string(nActiveBits - 1);
        } else {
          int rangeStart = -1;
          int rangeEnd = -1;
          for (size_t sector = 0; sector < validInputs.size(); sector++) {
            if (validInputs.test(sector)) {
              if (rangeStart < 0) {
                if (rangeEnd >= 0) {
                  bitInfo += ",";
                }
                bitInfo += std::to_string(sector);
                if (nActiveBits == 1) {
                  break;
                }
                rangeStart = sector;
              }
              rangeEnd = sector;
            } else {
              if (rangeStart >= 0 && rangeEnd > rangeStart) {
                bitInfo += "-" + std::to_string(rangeEnd);
              }
              rangeStart = -1;
            }
          }
          if (rangeStart >= 0 && rangeEnd > rangeStart) {
            bitInfo += "-" + std::to_string(rangeEnd);
          }
        }
        LOG(INFO) << "running tracking for sector(s) " << bitInfo;
      }
      ClusterNativeAccessFullTPC clusterIndex;
      memset(&clusterIndex, 0, sizeof(clusterIndex));
      ClusterNativeHelper::Reader::fillIndex(clusterIndex, inputs, mcInputs, [&validInputs](auto& index) { return validInputs.test(index); });

      std::vector<TrackTPC> tracks;
      MCLabelContainer tracksMCTruth;
      int retVal = tracker->runTracking(clusterIndex, &tracks, (processMC ? &tracksMCTruth : nullptr));
      if (retVal != 0) {
        // FIXME: error policy
        LOG(ERROR) << "tracker returned error code " << retVal;
      }
      LOG(INFO) << "found " << tracks.size() << " track(s)";
      pc.outputs().snapshot(OutputRef{ "output" }, tracks);
      if (processMC) {
        LOG(INFO) << "sending " << tracksMCTruth.getIndexedSize() << " track label(s)";
        pc.outputs().snapshot(OutputRef{ "mclblout" }, tracksMCTruth);
      }

      validInputs.reset();
      if (processMC) {
        validMcInputs.reset();
        for (auto& mcInput : mcInputs) {
          mcInput.clear();
        }
      }
    };

    return processingFct;
  };

  // FIXME: find out how to handle merge inputs in a simple and intuitive way
  // changing the binding name of the input in order to identify inputs by unique labels
  // in the processing. Think about how the processing can be made agnostic of input size,
  // e.g. by providing a span of inputs under a certain label
  auto createInputSpecs = [inputIds](bool makeMcInput) {
    Inputs inputs = { InputSpec{ "input", gDataOriginTPC, "CLUSTERNATIVE", 0, Lifetime::Timeframe } };
    if (makeMcInput) {
      inputs.emplace_back(InputSpec{ "mclblin", gDataOriginTPC, "CLNATIVEMCLBL", 0, Lifetime::Timeframe });
    }

    return std::move(mergeInputs(inputs, inputIds.size(),
                                 [inputIds](InputSpec& input, size_t index) {
                                   // using unique input names for the moment but want to find
                                   // an input-multiplicity-agnostic way of processing
                                   input.binding += std::to_string(inputIds[index]);
                                   DataSpecUtils::updateMatchingSubspec(input, inputIds[index]);
                                 }));
  };

  auto createOutputSpecs = [](bool makeMcOutput) {
    std::vector<OutputSpec> outputSpecs{
      OutputSpec{ { "output" }, gDataOriginTPC, "TRACKS", 0, Lifetime::Timeframe },
    };
    if (makeMcOutput) {
      OutputLabel label{ "mclblout" };
      constexpr o2::header::DataDescription datadesc("TRACKMCLBL");
      outputSpecs.emplace_back(label, gDataOriginTPC, datadesc, 0, Lifetime::Timeframe);
    }
    return std::move(outputSpecs);
  };

  return DataProcessorSpec{ "tpc-tracker", // process id
                            { createInputSpecs(processMC) },
                            { createOutputSpecs(processMC) },
                            AlgorithmSpec(initFunction),
                            Options{
                              { "tracker-options", VariantType::String, "", { "Option string passed to tracker" } },
                            } };
}

} // namespace TPC
} // namespace o2
