// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#ifndef ALICEO2_FIT_DIGITIZER_H
#define ALICEO2_FIT_DIGITIZER_H

#include "FITBase/Digit.h"
#include "T0Simulation/Detector.h"
#include "SimulationDataFormat/MCTruthContainer.h"
#include "SimulationDataFormat/MCCompLabel.h"
#include "FITBase/MCLabel.h"
#include "FITSimulation/DigitizationParameters.h"

namespace o2
{
namespace fit
{
class Digitizer
{
 public:
  Digitizer(const DigitizationParameters& params, Int_t mode = 0) : mMode(mode), parameters(params) { initParameters(); };
  ~Digitizer() = default;

  //void process(const std::vector<HitType>* hits, std::vector<Digit>* digits);
  void process(const std::vector<o2::fit::HitType>* hits, Digit* digit);
  void computeAverage(Digit& digit);

  void initParameters();
  // void printParameters();
  void setEventTime(double value) { mEventTime = value; }
  void setEventID(Int_t id) { mEventID = id; }
  void setSrcID(Int_t id) { mSrcID = id; }
  void setBC(Int_t bc) { mBC = bc; }
  void setOrbit(Int_t orbit) { mOrbit = orbit; }

  void setTriggers(Digit* digit);
  void smearCFDtime(Digit* digit);

  void init();
  void finish();

  void setMCLabels(o2::dataformats::MCTruthContainer<o2::fit::MCLabel>* mclb) { mMCLabels = mclb; }

 private:
  // digit info
  // parameters
  Int_t mMode;  //triggered or continuos
  Int_t mBC;    // BC
  Int_t mOrbit; //orbit
  Int_t mEventID;
  Int_t mSrcID;        // signal, background or QED
  Double_t mEventTime; // timestamp

  DigitizationParameters parameters;

  o2::dataformats::MCTruthContainer<o2::fit::MCLabel>* mMCLabels = nullptr;

  ClassDefNV(Digitizer, 1);
};
} // namespace fit
} // namespace o2

#endif
