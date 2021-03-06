// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file RawPixelReader.h
/// \brief Definition of the Alpide pixel reader for raw data processing
#ifndef ALICEO2_ITSMFT_RAWPIXELREADER_H_
#define ALICEO2_ITSMFT_RAWPIXELREADER_H_

#include "Headers/RAWDataHeader.h"
#include "CommonDataFormat/InteractionRecord.h"
#include "ITSMFTReconstruction/PixelReader.h"
#include "ITSMFTReconstruction/PixelData.h"
#include "ITSMFTReconstruction/ChipMappingITS.h" // this will become template parameter
#include "ITSMFTReconstruction/AlpideCoder.h"
#include "ITSMFTReconstruction/GBTWord.h"
#include "DetectorsBase/Triggers.h"
#include "ITSMFTReconstruction/PayLoadCont.h"
#include "ITSMFTReconstruction/PayLoadSG.h"
#include <TTree.h>
#include <TStopwatch.h>
#include <FairLogger.h>
#include <vector>
#include <limits>
#include <climits>
#include <memory>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <string_view>
#include <array>
#include <bitset>

#define _RAW_READER_ERROR_CHECKS_

namespace o2
{
namespace itsmft
{

constexpr int MaxLinksPerRU = 3;            // max numbet of GBT links per RU
constexpr int MaxCablesPerRU = 28;          // max numbet of cables RU can readout
constexpr int MaxChipsPerRU = 196;          // max number of chips the RU can readout
constexpr int MaxGBTPacketBytes = 8 * 1024; // Max size of GBT packet in bytes (8KB)
constexpr int NCRUPagesPerSuperpage = 256;  // Number of CRU pages per superpage

struct RUDecodingStat {

  // counters for format checks
  enum DecErrors : int {
    ErrPageCounterDiscontinuity, // RDH page counters for the same RU/trigger are not continuous
    ErrRDHvsGBTHPageCnt,         // RDH ang GBT header page counters are not consistent
    ErrMissingGBTHeader,         // GBT payload header was expected but not foun
    ErrMissingGBTTrailer,        // GBT payload trailer was expected but not found
    ErrNonZeroPageAfterStop,     // all lanes were stopped but the page counter in not 0
    ErrUnstoppedLanes,           // end of FEE data reached while not all lanes received stop
    ErrDataForStoppedLane,       // data was received for stopped lane
    ErrNoDataForActiveLane,      // no data was seen for lane (which was not in timeout)
    ErrIBChipLaneMismatch,       // chipID (on module) was different from the lane ID on the IB stave
    ErrCableDataHeadWrong,       // cable data does not start with chip header or empty chip
    NErrorsDefined
  };

  uint32_t lanesActive = 0;   // lanes declared by the payload header
  uint32_t lanesStop = 0;     // lanes received stop in the payload trailer
  uint32_t lanesTimeOut = 0;  // lanes received timeout
  uint32_t lanesWithData = 0; // lanes with data transmitted

  uint32_t nPackets = 0;                                                   // total number of packets
  std::array<int, NErrorsDefined> errorCounts = {};                        // error counters
  std::array<int, GBTDataTrailer::MaxStateCombinations> packetStates = {}; // packet status from the trailer

  //_____________________________________________________
  void clear()
  {
    nPackets = 0;
    errorCounts.fill(0);
    packetStates.fill(0);
    lanesActive = lanesStop = lanesTimeOut = lanesWithData = 0;
  }

  //_____________________________________________________
  void print(bool skipEmpty = true) const
  {
    int nErr = 0;
    for (int i = NErrorsDefined; i--;) {
      nErr += errorCounts[i];
    }
    printf("Decoding errors: %d\n", nErr);
    for (int i = 0; i < NErrorsDefined; i++) {
      if (!skipEmpty || errorCounts[i]) {
        printf("%-70s: %d\n", ErrNames[i].data(), errorCounts[i]);
      }
    }
    printf("Packet States Statistics (total packets: %d)\n", nPackets);
    for (int i = 0; i < GBTDataTrailer::MaxStateCombinations; i++) {
      if (packetStates[i]) {
        std::bitset<GBTDataTrailer::NStatesDefined> patt(i);
        printf("counts for triggers B[%s] : %d\n", patt.to_string().c_str(), packetStates[i]);
      }
    }
  }

  static constexpr std::array<std::string_view, NErrorsDefined> ErrNames = {
    "RDH page counters for the same RU/trigger are not continuous",      // ErrPageCounterDiscontinuity
    "RDH ang GBT header page counters are not consistent",               // ErrRDHvsGBTHPageCnt
    "GBT payload header was expected but not found",                     // ErrMissingGBTHeader
    "GBT payload trailer was expected but not found",                    // ErrMissingGBTTrailer
    "All lanes were stopped but the page counter in not 0",              // ErrNonZeroPageAfterStop
    "End of FEE data reached while not all lanes received stop",         // ErrUnstoppedLanes
    "Data was received for stopped lane",                                // ErrDataForStoppedLane
    "No data was seen for lane (which was not in timeout)",              // ErrNoDataForActiveLane
    "ChipID (on module) was different from the lane ID on the IB stave", // ErrIBChipLaneMismatch
    "Cable data does not start with chip header or empty chip"           // ErrCableDataHeadWrong
  };

  ClassDefNV(RUDecodingStat, 1);
};

constexpr std::array<std::string_view, RUDecodingStat::NErrorsDefined> RUDecodingStat::ErrNames;

struct RawDecodingStat {
  using ULL = unsigned long long;
  uint64_t nPagesProcessed = 0; // total number of pages processed
  uint64_t nRUsProcessed = 0;   // total number of RUs processed (1 RU may take a few pages)
  uint64_t nBytesProcessed = 0; // total number of bytes (rdh->memorySize) processed
  uint64_t nNonEmptyChips = 0;  // number of non-empty chips found
  uint64_t nHitsDecoded = 0;    // number of hits found

  RawDecodingStat() = default;

  void clear()
  {
    nPagesProcessed = 0;
    nRUsProcessed = 0;
    nBytesProcessed = 0;
    nNonEmptyChips = 0;
    nHitsDecoded = 0;
  }

  void print() const
  {
    printf("\nDecoding statistics\n");
    printf("%llu bytes for %llu RUs processed in %llu pages\n", (ULL)nBytesProcessed, (ULL)nRUsProcessed, (ULL)nPagesProcessed);
    printf("%llu hits found in %llu non-empty chips\n", (ULL)nHitsDecoded, (ULL)nNonEmptyChips);
  }

  ClassDefNV(RawDecodingStat, 1);
};

// support for the GBT single link data
struct RULink {
  PayLoadCont data;     // data buffer per link
  int lastPageSize = 0; // size of last added page = offset from the end to get to the RDH
  int nTriggers = 0;    // number of triggers loaded (the last one might be incomplete)
  uint32_t lanes = 0;   // lanes served by this link
};

struct RUDecodeData {
  std::array<PayLoadCont, MaxCablesPerRU> cableData;              // cable data in compressed ALPIDE format
  std::array<uint8_t, MaxCablesPerRU> cableHWID;                  // HW ID of cable whose data is in the corresponding slot of cableData
  std::array<o2::itsmft::ChipPixelData, MaxChipsPerRU> chipsData; // fully decoded data
  std::array<std::unique_ptr<RULink>, MaxLinksPerRU> links;       // data + counters for links of this RU
  RUDecodingStat statistics;                                      // decoding statistics

  int nCables = 0;         // total number of cables decoded for single trigger
  int nChipsFired = 0;     // number of chips with data or with errors
  int lastChipChecked = 0; // last chips checked among nChipsFired
  const RUInfo* ruInfo = nullptr;

  RUDecodeData() = default;
  //  RUDecodeData(const RUDecodeData& src) {}; // dummy?

  void clear()
  {
    clearTrigger();
    statistics.clear();
  }

  void clearTrigger()
  {
    for (int i = nCables; i--;) {
      cableData[i].clear();
    }
    nCables = 0;
  }
};

struct RULinks {
  std::array<PayLoadCont, MaxLinksPerRU> data;         // data buffer per link
  std::array<int, MaxLinksPerRU> lastPageSize = { 0 }; // size of last added page = offset from the end to get to the RDH
  std::array<int, MaxLinksPerRU> nTriggers = { 0 };    // number of triggers loaded (the last one might be incomplete)
  RULinks() = default;
};

/// Used both for encoding to and decoding from the alpide raw data format
/// Requires as a template parameter a helper class for detector-specific
/// mapping between the software global chip ID and HW module ID and chip ID
/// within the module, see for example ChipMappingITS class.
/// Similar helper class must be provided for the MFT

template <class Mapping = o2::itsmft::ChipMappingITS>
class RawPixelReader : public PixelReader
{
  using Coder = o2::itsmft::AlpideCoder;

 public:
  RawPixelReader()
  {
    mRUEntry.fill(-1); // no known links in the beginning
  }

  ~RawPixelReader() override
  {
    mSWIO.Stop();
    printf("RawPixelReader IO time: ");
    mSWIO.Print();
  }

  /// do we interpred GBT words as padded to 128 bits?
  bool isPadding128() const { return mPadding128; }

  /// do we treat CRU pages as having max size?
  bool isMaxPageImposed() const { return mImposeMaxPage; }

  /// assumed GBT word size (accounting for eventual padding)
  int getGBTWordSize() const { return mGBTWordSize; }

  /// impose padding model for GBT words
  void setPadding128(bool v)
  {
    mPadding128 = v;
    mGBTWordSize = mPadding128 ? o2::itsmft::GBTPaddedWordLength : o2::itsmft::GBTWordLength;
  }

  /// set min number of triggers to cache per frame
  void setMinTriggersToCache(int n) { mMinTriggersToCache = n > NCRUPagesPerSuperpage ? n : NCRUPagesPerSuperpage + 1; }

  int getMinTriggersToCache() const { return mMinTriggersToCache; }

  /// CRU pages are of max size of 8KB
  void imposeMaxPage(bool v) { mImposeMaxPage = v; }

  ///______________________________________________________________________
  ChipPixelData* getNextChipData(std::vector<ChipPixelData>& chipDataVec) override
  {
    // decode new RU if no cached non-empty chips

    if (mCurRUDecodeID >= 0) { // make sure current RU has fired chips to extract
      for (; mCurRUDecodeID < mNRUs; mCurRUDecodeID++) {
        auto& ru = mRUDecodeVec[mCurRUDecodeID];
        if (ru.lastChipChecked < ru.nChipsFired) {
          auto& chipData = ru.chipsData[ru.lastChipChecked++];
          int id = chipData.getChipID();
          chipDataVec[id].swap(chipData);
          return &chipDataVec[id];
        }
      }
      mCurRUDecodeID = 0; // no more decoded data if reached this place,
    }
    // will need to decode new trigger
    if (mMinTriggersCached < 2) { // last trigger might be incomplete, need to cache more data
      cacheLinksData(mRawBuffer);
    }
    if (mMinTriggersCached < 1 || !decodeNextTrigger()) {
      mCurRUDecodeID = -1;
      return nullptr; // nothing left
    }
    return getNextChipData(chipDataVec); // is it ok to use recursion here?
  }

  ///______________________________________________________________________
  void init() override{};

  ///______________________________________________________________________
  void clear()
  {
    mDecodingStat.clear();
    for (auto& rudec : mRUDecodeVec) {
      rudec.clear();
    }
    mNLinks = 0;
    mNRUs = 0;
    mIOFile.close();
    mRawBuffer.clear();
  }

  ///================================== Encoding methods ========================

  ///______________________________________________________________________
  int digits2raw(const std::vector<o2::itsmft::Digit>& digiVec, int from, int ndig, const o2::InteractionRecord& bcData,
                 uint8_t ruSWMin = 0, uint8_t ruSWMax = 0xff)
  {
    // Convert ndig digits belonging to the same trigger to raw data
    // The digits in the vector must be in increasing chipID order
    // Return the number of pages in the link with smallest amount of pages

    int nDigTot = digiVec.size();
    assert(from < nDigTot);
    int last = (from + ndig <= nDigTot) ? from + ndig : nDigTot;
    RUDecodeData* curRUDecode = nullptr;
    ChipPixelData* curChipData = nullptr;
    ChipInfo chInfo;
    UShort_t curChipID = 0xffff; // currently processed SW chip id
    mInteractionRecord = bcData;
    ruSWMax = (ruSWMax < uint8_t(MAP.getNRUs())) ? ruSWMax : MAP.getNRUs() - 1;

    if (mNRUs < int(ruSWMax) - ruSWMin) { // book containers if needed
      for (uint8_t ru = ruSWMin; ru <= ruSWMax; ru++) {
        auto& ruData = getCreateRUDecode(ru);
        int nLinks = 0;
        for (int il = 0; il < MaxLinksPerRU; il++) {
          nLinks += ruData.links[il] ? 1 : 0;
        }
        mNLinks += nLinks;
        if (!nLinks) {
          LOG(INFO) << "Imposing single link readout for RU " << int(ru);
          ruData.links[0] = std::make_unique<RULink>();
          ruData.links[0]->lanes = MAP.getCablesOnRUType(ruData.ruInfo->ruType);
          mNLinks++;
        }
      }
    }

    // place digits into corresponding chip buffers
    for (int id = from; id < last; id++) {
      const auto& dig = digiVec[id];
      if (curChipID != dig.getChipIndex()) {
        MAP.getChipInfoSW(dig.getChipIndex(), chInfo);
        if (chInfo.ru < ruSWMin || chInfo.ru > ruSWMax) { // ignore this chip?
          continue;
        }
        curChipID = dig.getChipIndex();
        mCurRUDecodeID = chInfo.ru;
        curRUDecode = &mRUDecodeVec[mCurRUDecodeID];
        curChipData = &curRUDecode->chipsData[curRUDecode->nChipsFired++];
        curChipData->setChipID(chInfo.chOnRU->id); // set ID within the RU
      }
      curChipData->getData().emplace_back(&dig); // add new digit to the container
    }
    // convert digits to alpide data in the per-cable buffers
    int minPages = 0xffffff;
    for (mCurRUDecodeID = ruSWMin; mCurRUDecodeID <= int(ruSWMax); mCurRUDecodeID++) {
      curRUDecode = &mRUDecodeVec[mCurRUDecodeID];
      uint16_t next2Proc = 0, nchTot = MAP.getNChipsOnRUType(curRUDecode->ruInfo->ruType);
      for (int ich = 0; ich < curRUDecode->nChipsFired; ich++) {
        auto& chipData = curRUDecode->chipsData[ich];
        convertEmptyChips(next2Proc, chipData.getChipID()); // if needed store EmptyChip flags
        next2Proc = chipData.getChipID() + 1;
        convertChip(chipData);
        chipData.clear();
      }
      convertEmptyChips(next2Proc, nchTot); // if needed store EmptyChip flags
      int minPageRU = fillRULinks();        // flush per-lane buffers to link buffers
      if (minPageRU < minPages) {
        minPages = minPageRU;
      }
    }

    return minPages;
  }

  //___________________________________________________________________________________
  void convertChip(o2::itsmft::ChipPixelData& chipData)
  {
    ///< convert digits of single chip to Alpide format.

    auto& ruData = mRUDecodeVec[mCurRUDecodeID]; // current RU container
    // fetch info of the chip with chipData->getChipID() ID within the RU
    const auto& chip = *MAP.getChipOnRUInfo(ruData.ruInfo->ruType, chipData.getChipID());
    ruData.cableHWID[chip.cableSW] = chip.cableHW;                    // register the cable HW ID

    auto& pixels = chipData.getData();
    std::sort(pixels.begin(), pixels.end(),
              [](auto lhs, auto rhs) {
                if (lhs.getRow() < rhs.getRow())
                  return true;
                if (lhs.getRow() > rhs.getRow())
                  return false;
                return lhs.getCol() < rhs.getCol();
              });
    ruData.cableData[chip.cableSW].ensureFreeCapacity(40 * (2 + pixels.size())); // make sure buffer has enough capacity
    mCoder.encodeChip(ruData.cableData[chip.cableSW], chipData, chip.chipOnModuleHW, mInteractionRecord.bc);
  }

  //______________________________________________________
  void convertEmptyChips(int fromChip, int uptoChip)
  {
    // add empty chip words to respective cable's buffers for all chips of the current RU container
    auto& ruData = mRUDecodeVec[mCurRUDecodeID];                     // current RU container
    for (int chipIDSW = fromChip; chipIDSW < uptoChip; chipIDSW++) { // flag chips w/o data
      const auto& chip = *MAP.getChipOnRUInfo(ruData.ruInfo->ruType, chipIDSW);
      ruData.cableHWID[chip.cableSW] = chip.cableHW; // register the cable HW ID
      ruData.cableData[chip.cableSW].ensureFreeCapacity(100);
      mCoder.addEmptyChip(ruData.cableData[chip.cableSW], chip.chipOnModuleHW, mInteractionRecord.bc);
    }
  }

  //___________________________________________________________________________________
  int fillRULinks()
  {
    // fill data of the RU to links buffer, return the number of pages in the link with smallest amount of pages
    constexpr uint8_t zero16[o2::itsmft::GBTPaddedWordLength] = { 0 }; // to speedup padding
    const int dummyNPages = 0xffffff;                                  // any large number
    int minPages = dummyNPages;
    auto& ruData = mRUDecodeVec[mCurRUDecodeID];
    ruData.nCables = ruData.ruInfo->nCables;
    o2::header::RAWDataHeader rdh;
    rdh.triggerOrbit = rdh.heartbeatOrbit = mInteractionRecord.orbit;
    rdh.triggerBC = rdh.heartbeatBC = mInteractionRecord.bc;
    rdh.triggerType = o2::trigger::PhT; // ??
    rdh.detectorField = MAP.getRUDetectorField();
    rdh.blockLength = 0xffff; // ITS keeps this dummy

    int maxGBTWordsPerPacket = (MaxGBTPacketBytes - rdh.headerSize) / o2::itsmft::GBTPaddedWordLength - 2;

    int nGBTW[MaxLinksPerRU] = { 0 };
    for (int il = 0; il < MaxLinksPerRU; il++) {
      auto link = ruData.links[il].get();
      if (!link) {
        continue;
      }
      int nGBTWordsNeeded = 0;
      for (int icab = ruData.nCables; icab--;) { // calculate number of GBT words per link
        if ((link->lanes & (0x1 << icab))) {
          int nb = ruData.cableData[icab].getSize();
          nGBTWordsNeeded += nb ? 1 + (nb - 1) / 9 : 0;
        }
      }
      // move data in padded GBT words from cable buffers to link buffers
      rdh.feeId = MAP.RUSW2FEEId(ruData.ruInfo->idSW, il); // write on link 0 always
      rdh.linkID = il;
      rdh.pageCnt = 0;
      rdh.stop = 0;
      rdh.memorySize = rdh.headerSize + (nGBTWordsNeeded + 2) * mGBTWordSize; // update remaining size
      if (rdh.memorySize > MaxGBTPacketBytes) {
        rdh.memorySize = MaxGBTPacketBytes;
      }
      rdh.offsetToNext = mImposeMaxPage ? MaxGBTPacketBytes : rdh.memorySize;

      link->data.ensureFreeCapacity(MaxGBTPacketBytes);
      link->data.addFast(reinterpret_cast<uint8_t*>(&rdh), rdh.headerSize); // write RDH for current packet
      link->nTriggers++;                                                    // acknowledge the page, note: here we count pages, not triggers
      o2::itsmft::GBTDataHeader gbtHeader(0, link->lanes);
      o2::itsmft::GBTDataTrailer gbtTrailer; // lanes will be set on closing the last page

      gbtHeader.setPacketID(rdh.pageCnt);
      link->data.addFast(gbtHeader.getW8(), mGBTWordSize); // write GBT header for current packet
      if (mVerbose) {
        LOG(INFO) << "Filling RU data";
        printRDH(rdh);
        gbtHeader.printX(mPadding128);
      }

      // now loop over the lanes served by this link, writing each time at most 9 bytes, untill all lanes are copied
      int nGBTWordsInPacket = 0;
      do {
        for (int icab = 0; icab < ruData.nCables; icab++) {
          if ((link->lanes & (0x1 << icab))) {
            auto& cableData = ruData.cableData[icab];
            int nb = cableData.getUnusedSize();
            if (!nb) {
              continue; // write 80b word only if there is something to write
            }
            if (nb > 9) {
              nb = 9;
            }
            int gbtWordStart = link->data.getSize();                                                              // beginning of the current GBT word in the link
            link->data.addFast(cableData.getPtr(), nb);                                                           // fill payload of cable
            link->data.addFast(zero16, mGBTWordSize - nb);                                                        // fill the rest of the GBT word by 0
            link->data[gbtWordStart + 9] = MAP.getGBTHeaderRUType(ruData.ruInfo->ruType, ruData.cableHWID[icab]); // set cable flag
            cableData.setPtr(cableData.getPtr() + nb);
            nGBTWordsNeeded--;
            if (mVerbose) {
              ((GBTData*)(&link->data[gbtWordStart]))->printX(mPadding128);
            }
            if (++nGBTWordsInPacket == maxGBTWordsPerPacket) { // check if new GBT packet must be created
              break;
            }
          } // storing data of single cable
        }   // loop over cables of this link

        if (nGBTWordsNeeded && nGBTWordsInPacket >= maxGBTWordsPerPacket) {
          // more data to write, write trailer and add new GBT packet
          link->data.add(gbtTrailer.getW8(), mGBTWordSize); // write empty GBT trailer for current packet
          if (mVerbose) {
            gbtTrailer.printX(mPadding128);
          }
          rdh.pageCnt++;                                     // flag new page
          rdh.stop = nGBTWordsNeeded < maxGBTWordsPerPacket; // flag if this is the last packet of multi-packet
          rdh.blockLength = 0xffff;                          // (nGBTWordsNeeded % maxGBTWordsPerPacket + 2) * mGBTWordSize; // record payload size
          // update remaining size, using padded GBT words (as CRU writes)
          rdh.memorySize = rdh.headerSize + (nGBTWordsNeeded + 2) * o2::itsmft::GBTPaddedWordLength;
          if (rdh.memorySize > MaxGBTPacketBytes) {
            rdh.memorySize = MaxGBTPacketBytes;
          }
          rdh.offsetToNext = mImposeMaxPage ? MaxGBTPacketBytes : rdh.memorySize;
          link->data.ensureFreeCapacity(MaxGBTPacketBytes);
          link->data.addFast(reinterpret_cast<uint8_t*>(&rdh), rdh.headerSize); // write RDH for current packet
          link->nTriggers++;                                                    // acknowledge the page, note: here we count pages, not triggers
          if (mVerbose) {
            printRDH(rdh);
          }
          gbtHeader.setPacketID(rdh.pageCnt);
          link->data.addFast(gbtHeader.getW8(), mGBTWordSize); // write GBT header for current packet
          if (mVerbose) {
            gbtHeader.printX(mPadding128);
          }
          nGBTWordsInPacket = 0; // reset counter of words in the packet
        }
      } while (nGBTWordsNeeded);

      gbtTrailer.setLanesStop(link->lanes);
      gbtTrailer.setPacketState(0x1 << GBTDataTrailer::PacketDone);
      link->data.addFast(gbtTrailer.getW8(), mGBTWordSize); // write GBT trailer for the last packet
      if (mVerbose) {
        gbtTrailer.printX(mPadding128);
      }
      // NOTE: here we don't pad the page to 8KB, will do this when flushing everything to the sink

      if (minPages > link->nTriggers) {
        minPages = link->nTriggers;
      }

    } // loop over links of RU
    ruData.clearTrigger();
    ruData.nChipsFired = 0;
    return minPages == dummyNPages ? 0 : minPages;
  }

  //___________________________________________________________________________________
  int flushSuperPages(int maxPages, PayLoadCont& sink)
  {
    // flush superpage (at most maxPages) of each link to the output,
    // return total number of pages flushed

    int totPages = 0;
    for (int ru = 0; ru < MAP.getNRUs(); ru++) {
      auto* ruData = getRUDecode(ru);
      if (!ruData) {
        continue;
      }
      for (int il = 0; il < MaxLinksPerRU; il++) {
        auto link = ruData->links[il].get();
        if (!link || link->data.isEmpty()) {
          continue;
        }
        int nPages = 0;
        sink.ensureFreeCapacity(maxPages * MaxGBTPacketBytes);
        const auto* ptrIni = link->data.getPtr();
        while (nPages < maxPages && !link->data.isEmpty()) {
          const auto ptr = link->data.getPtr();
          o2::header::RAWDataHeader* rdh = reinterpret_cast<o2::header::RAWDataHeader*>(ptr);
          sink.addFast(ptr, rdh->memorySize);                    // copy header + payload
          sink.fillFast(0, MaxGBTPacketBytes - rdh->memorySize); // complete with 0's till the end of the page
          link->data.setPtr(ptr + rdh->memorySize);
          link->nTriggers--; // here we count pages, not triggers
          nPages++;
        }
        totPages += nPages;
        link->data.moveUnusedToHead();
      } // loop over links
    }   // loop over RUs
    return totPages;
  }

  ///================================== Decoding methods ========================

  //_____________________________________________________________________________
  void printRDH(const o2::header::RAWDataHeader& h)
  {
    printf("RDH| Ver:%2u Hsz:%2u Blgt:%4u FEEId:0x%04x PBit:%u\n",
           uint32_t(h.version), uint32_t(h.headerSize), uint32_t(h.blockLength), uint32_t(h.feeId), uint32_t(h.priority));
    printf("RDH|[CRU: Offs:%5u Msz:%4u LnkId:0x%02x Packet:%3u CRUId:0x%04x]\n",
           uint32_t(h.offsetToNext), uint32_t(h.memorySize), uint32_t(h.linkID), uint32_t(h.packetCounter), uint32_t(h.cruID));
    printf("RDH| TrgOrb:%9u HBOrb:%9u TrgBC:%4u HBBC:%4u TrgType:%u\n",
           uint32_t(h.triggerOrbit), uint32_t(h.heartbeatOrbit), uint32_t(h.triggerBC), uint32_t(h.heartbeatBC),
           uint32_t(h.triggerType));
    printf("RDH| DetField:0x%05x Par:0x%04x Stop:0x%04x PageCnt:%5u\n", uint32_t(h.detectorField), uint32_t(h.par), uint32_t(h.stop), uint32_t(h.pageCnt));
  }

  //_____________________________________
  size_t cacheLinksData(PayLoadCont& buffer)
  {
    // distribute data from the single buffer among the links caches

    LOG(INFO) << "Cacheding links data, currently in cache: " << mMinTriggersCached << " triggers";
    auto nRead = loadInput(buffer);
    if (buffer.isEmpty()) {
      return nRead;
    }

    bool enoughTriggers[ChipMappingITS::getNRUs()][3] = { false }; // flag that enough triggeres were loaded for this link
    int nLEnoughTriggers = 0;                                      // number of links for we which enough number of triggers were loaded
    auto ptr = buffer.getPtr();
    o2::header::RAWDataHeader* rdh = reinterpret_cast<o2::header::RAWDataHeader*>(ptr);

    do {
      if (!isRDHHeuristic(rdh)) {   // does it look like RDH?
        if (!findNextRDH(buffer)) { // try to recover the pointer
          break;                    // no data to continue
        }
        ptr = buffer.getPtr();
      }

      int ruIDSW = getMapping().FEEId2RUSW(rdh->feeId);
      auto& ruDecode = getCreateRUDecode(ruIDSW);

      bool newTrigger = true; // check if we see new trigger
      auto link = ruDecode.links[rdh->linkID].get();
      if (link) {                                                                                                    // was there any data seen on this link before?
        const auto rdhPrev = reinterpret_cast<o2::header::RAWDataHeader*>(link->data.getEnd() - link->lastPageSize); // last stored RDH
        if (isSameRUandTrigger(rdhPrev, rdh)) {
          newTrigger = false;
        }
      } else { // a new link was added
        ruDecode.links[rdh->linkID] = std::make_unique<RULink>();
        link = ruDecode.links[rdh->linkID].get();
        mNLinks++;
      }
      // copy data to the buffer of the link and memorize its RDH pointer
      link->data.add(ptr, rdh->memorySize);
      link->lastPageSize = rdh->memorySize; // account new added size
      auto rdhC = reinterpret_cast<o2::header::RAWDataHeader*>(link->data.getEnd() - link->lastPageSize);
      rdhC->offsetToNext = rdh->memorySize; // since we skip 0-s, we have to modify the offset

      if (newTrigger) {
        link->nTriggers++; // acknowledge 1st trigger
        if (link->nTriggers >= mMinTriggersToCache && !enoughTriggers[ruIDSW][rdh->linkID]) {
          nLEnoughTriggers++;
          enoughTriggers[ruIDSW][rdh->linkID] = true;
        }
      }

      mDecodingStat.nBytesProcessed += rdh->memorySize;
      mDecodingStat.nPagesProcessed++;
      ptr += rdh->offsetToNext;
      buffer.setPtr(ptr);
      if (buffer.getUnusedSize() < MaxGBTPacketBytes) {
        nRead += loadInput(buffer); // update
        ptr = buffer.getPtr();      // pointer might have been changed
      }

      rdh = reinterpret_cast<o2::header::RAWDataHeader*>(ptr);

      if (mNLinks == nLEnoughTriggers) {
        break;
      }

    } while (!buffer.isEmpty());

    if (mNLinks == nLEnoughTriggers) {
      mMinTriggersCached = mMinTriggersToCache; // wanted number of triggers acquired
    } else {                                    // there were no enough triggers to fulfill mMinTriggersToCache requirement
      mMinTriggersCached = INT_MAX;
      for (int ir = 0; ir < mNRUs; ir++) {
        const auto& ruDecData = mRUDecodeVec[ir];
        for (auto& link : ruDecData.links) {
          if (link && link->nTriggers < mMinTriggersCached) {
            mMinTriggersCached = link->nTriggers;
          }
        }
      }
    }
    LOG(INFO) << "Cached at least " << mMinTriggersCached << " triggers on " << mNLinks << " links of " << mNRUs << " RUs";

    return nRead;
  }

  //_____________________________________
  int decodeNextTrigger()
  {
    // Decode next trigger from the cached links data and decrease cached triggers counter, return N links decoded
    if (mMinTriggersCached < 1) {
      return 0;
    }
    int nlinks = 0;
    for (int ir = mNRUs; ir--;) {
      auto& ruDecode = mRUDecodeVec[ir];
      if (!nlinks) {                        // on 1st occasion extract trigger data
        for (auto& link : ruDecode.links) { // loop over links to fill cable buffers
          if (link && !link->data.isEmpty()) {
            const auto rdh = reinterpret_cast<const o2::header::RAWDataHeader*>(link->data.getPtr());
            mInteractionRecord.bc = rdh->triggerBC;
            mInteractionRecord.orbit = rdh->triggerOrbit;
            mTrigger = rdh->triggerType;
            mInteractionRecordHB.bc = rdh->heartbeatBC;
            mInteractionRecordHB.orbit = rdh->heartbeatOrbit;
            break;
          }
        }
      }

      nlinks += decodeNextRUData(ruDecode);
      mDecodingStat.nRUsProcessed++;
    }
    mCurRUDecodeID = 0;
    mMinTriggersCached--;
    return nlinks;
  }

  //_____________________________________
  int decodeNextRUData(RUDecodeData& ruDecData)
  {
    // process data of single RU trigger from its links buffers
    int minTriggers = INT_MAX;
    int res = 0;
    ruDecData.clearTrigger();
    bool aborted = false;
    for (auto& link : ruDecData.links) { // loop over links to fill cable buffers
      if (link && !link->data.isEmpty()) {
        link->data.setPtr(decodeRUData(link->data.getPtr(), ruDecData, aborted));
        // we don't need to check the "abort" status since the checks for links data presence and synchronization
        // should have been done in advance
        if (--link->nTriggers < minTriggers) { // decrement counter of cached triggers
          minTriggers = link->nTriggers;
        }
        res++;
        if (link->data.isEmpty()) {
          link->data.clear();
        }
      }
    }
    if (ruDecData.nCables) {       // there are cables with data to decode
      decodeAlpideData(ruDecData); // decode Alpide data from the compressed RU Data
    }
    return res;
  }

  //_____________________________________
  bool findNextRDH(PayLoadCont& buffer)
  {
    // keep reading GRB words until RDH is found
    size_t nRead = 0;
    int scan = 0;
    bool goodRDH = false;
    auto ptr = buffer.getPtr();
    o2::header::RAWDataHeader* rdh = nullptr;
    do {
      if (buffer.isEmpty()) {
        auto nrl = loadInput(buffer);
        if (!nrl) {
          break;
        }
        nRead += nrl;
        ptr = buffer.getPtr();
      }
      scan++;
      ptr += o2::itsmft::GBTPaddedWordLength;
      buffer.setPtr(ptr);
      if (!buffer.isEmpty()) {
        rdh = reinterpret_cast<o2::header::RAWDataHeader*>(ptr);
      } else {
        break;
      }
    } while (!(goodRDH = isRDHHeuristic(rdh)));
    LOG(INFO) << "End of pointer recovery after skipping " << scan << " GBT words, RDH is"
              << (goodRDH ? "" : " not") << " found";
    return goodRDH;
  }

  //_____________________________________
  bool isRDHHeuristic(const o2::header::RAWDataHeader* rdh)
  {
    /// heuristic check if this is indeed an RDH
    return (!rdh || rdh->headerSize != sizeof(o2::header::RAWDataHeader) || rdh->zero0 != 0 || rdh->zero1 != 0 ||
            rdh->zero41 != 0 || rdh->zero42 != 0 || rdh->word5 != 0 || rdh->zero6 != 0)
             ? false
             : true;
  }

  //_____________________________________
  uint8_t* decodeRUData(uint8_t* raw, RUDecodeData& ruDecData, bool& aborted)
  {
    /// Decode raw data of single RU (possibly in a few GBT packets), collecting raw data
    /// for every cable in the corresponding slot of the provided ruDecData.
    /// No check is done if the necessary data are fully contained in the raw buffer.
    /// Return the pointer on the last raw data byte after decoding the RU
    /// In case of unrecoverable error set aborted to true

    aborted = false;

    // data must start by RDH
    auto rdh = reinterpret_cast<const o2::header::RAWDataHeader*>(raw);

#ifdef _RAW_READER_ERROR_CHECKS_
    if (!isRDHHeuristic(rdh)) {
      LOG(ERROR) << "Page does not start with RDH";
      for (int i = 0; i < 4; i++) {
        auto gbtD = reinterpret_cast<const o2::itsmft::GBTData*>(raw + i * 16);
        gbtD->printX(mPadding128);
      }
      raw += mGBTWordSize;
      aborted = true;
      return raw;
    }
#endif

    int ruIDSW = MAP.FEEId2RUSW(rdh->feeId);
    if (ruIDSW != ruDecData.ruInfo->idSW) {
      LOG(ERROR) << "RDG RU IDSW " << ruIDSW << " differs from expected " << ruDecData.ruInfo->idSW;
    }

    auto& ruStat = ruDecData.statistics;
    ruStat.nPackets++;

    ruDecData.nCables = ruDecData.ruInfo->nCables;
    while (1) {
      raw += rdh->headerSize;
      int nGBTWords = (rdh->memorySize - rdh->headerSize) / mGBTWordSize - 2; // number of GBT words excluding header/trailer
      auto gbtH = reinterpret_cast<const o2::itsmft::GBTDataHeader*>(raw);    // process GBT header

#ifdef _RAW_READER_ERROR_CHECKS_
      if (mVerbose) {
        printRDH(*rdh);
        gbtH->printX(mPadding128);
        LOG(INFO) << "Expect " << nGBTWords << " GBT words";
      }
      if (!gbtH->isDataHeader()) {
        gbtH->printX(mPadding128);
        LOG(ERROR) << "FEE#" << rdh->feeId << " GBT payload header was expected, abort page decoding";
        gbtH->printX(mPadding128);
        ruStat.errorCounts[RUDecodingStat::ErrMissingGBTHeader]++;
        aborted = true;
        return raw;
      }
      if (gbtH->getPacketID() != rdh->pageCnt) {
        LOG(ERROR) << "FEE#" << rdh->feeId << " Different GBT header " << gbtH->getPacketID()
                   << " and RDH page " << rdh->pageCnt << " counters";
        ruStat.errorCounts[RUDecodingStat::ErrRDHvsGBTHPageCnt]++;
      }

      if (ruStat.lanesActive == ruStat.lanesStop) { // all lanes received their stop, new page 0 expected
        if (rdh->pageCnt) {                         // flag lanes of this FEE
          LOG(ERROR) << "FEE#" << rdh->feeId << " Non-0 page counter (" << rdh->pageCnt << ") while all lanes were stopped";
          ruStat.errorCounts[RUDecodingStat::ErrNonZeroPageAfterStop]++;
        }
      }

      ruStat.lanesActive = gbtH->getLanes(); // TODO do we need to update this for every page?

      if (!rdh->pageCnt) { // reset flags
        ruStat.lanesStop = 0;
        ruStat.lanesWithData = 0;
      }

#endif
      raw += mGBTWordSize;
      for (int iw = 0; iw < nGBTWords; iw++, raw += mGBTWordSize) {
        auto gbtD = reinterpret_cast<const o2::itsmft::GBTData*>(raw);
        // TODO: need to clarify if the nGBTWords from the rdh->memorySize is reliable estimate of the real payload, at the moment this is not the case

        if (mVerbose) {
          printf("W%4d |", iw);
          gbtD->printX(mPadding128);
        }
        if (gbtD->isDataTrailer()) {
          nGBTWords = iw;
          break; // this means that the nGBTWords estimate was wrong
        }

        int cableHW = gbtD->getCableID();
        int cableSW = MAP.cableHW2SW(ruDecData.ruInfo->ruType, cableHW);
        ruDecData.cableData[cableSW].add(gbtD->getW8(), 9);
        ruDecData.cableHWID[cableSW] = cableHW;

#ifdef _RAW_READER_ERROR_CHECKS_
        ruStat.lanesWithData |= 0x1 << cableSW;    // flag that the data was seen on this lane
        if (ruStat.lanesStop & (0x1 << cableSW)) { // make sure stopped lanes do not transmit the data
          ruStat.errorCounts[RUDecodingStat::ErrDataForStoppedLane]++;
          LOG(ERROR) << "FEE#" << rdh->feeId << " Data received for stopped lane " << cableHW << " (sw:" << cableSW << ")";
        }
#endif

      } // we are at the trailer, packet is over, check if there are more for the same ru

      auto gbtT = reinterpret_cast<const o2::itsmft::GBTDataTrailer*>(raw); // process GBT trailer
#ifdef _RAW_READER_ERROR_CHECKS_

      if (mVerbose) {
        gbtT->printX(mPadding128);
      }

      if (!gbtT->isDataTrailer()) {
        gbtT->printX(mPadding128);
        LOG(ERROR) << "FEE#" << rdh->feeId << " GBT payload trailer was expected, abort page decoding NW" << nGBTWords;
        ruStat.errorCounts[RUDecodingStat::ErrMissingGBTTrailer]++;
        aborted = true;
        return raw;
      }

      ruStat.lanesTimeOut |= gbtT->getLanesTimeout(); // register timeouts
      ruStat.lanesStop |= gbtT->getLanesStop();       // register stops
#endif
      raw += mGBTWordSize;
      // we finished the GBT page, see if there is a continuation and if it belongs to the same multipacket

      if (!rdh->offsetToNext) { // RS TODO: what the last page in memory will contain as offsetToNext, is it 0?
        break;
      }

      raw = ((uint8_t*)rdh) + rdh->offsetToNext; // jump to the next packet:
      auto rdhN = reinterpret_cast<const o2::header::RAWDataHeader*>(raw);
      // check if data of given RU are over, i.e. we the page counter was wrapped to 0 (should be enough!) or other RU/trigger started
      if (!isSameRUandTrigger(rdh, rdhN)) {

#ifdef _RAW_READER_ERROR_CHECKS_
        // make sure all lane stops for finished page are received
        if (ruStat.lanesActive != ruStat.lanesStop) {
          if (rdh->triggerType != o2::trigger::SOT) { // only SOT trigger allows unstopped lanes?
            LOG(ERROR) << "FEE#" << rdh->feeId << " end of FEE data but not all lanes received stop";
            ruStat.errorCounts[RUDecodingStat::ErrUnstoppedLanes]++;
          }
        }

        // make sure all active lanes (except those in time-out) have sent some data
        if ((~ruStat.lanesWithData & ruStat.lanesActive) != ruStat.lanesTimeOut) {
          LOG(ERROR) << "FEE#" << rdh->feeId << " Lanes not in time-out but not sending data";
          ruStat.errorCounts[RUDecodingStat::ErrNoDataForActiveLane]++;
        }
#endif
        // accumulate packet states
        ruStat.packetStates[gbtT->getPacketState()]++;

        break;
      }
#ifdef _RAW_READER_ERROR_CHECKS_
      // check if the page counter increases
      if (rdhN->pageCnt != rdh->pageCnt + 1) {
        LOG(ERROR) << "FEE#" << rdh->feeId << " Discontinuity in the RDH page counter of the same RU trigger: old "
                   << rdh->pageCnt << " new: " << rdhN->pageCnt;
        ruStat.errorCounts[RUDecodingStat::ErrPageCounterDiscontinuity]++;
      }
#endif
      rdh = rdhN;
    }

#ifdef _RAW_READER_ERROR_CHECKS_
//    if (rdh->pageCnt && !rdh->stop) {
//      LOG(WARNING) << "Last packet(" << rdh->pageCnt << ") of GBT multi-packet is reached w/o STOP set in the RDH";
//    }
#endif

    return raw;
  }

  //_____________________________________
  int skimNextRUData(PayLoadCont& outBuffer)
  {
    if (mIOFile) {
      loadInput(mRawBuffer); // if needed, upload additional data to the buffer
    }

    int res = 0;
    if (!mRawBuffer.isEmpty()) {
      bool aborted = false;

      auto ptr = skimPaddedRUData(mRawBuffer.getPtr(), outBuffer, aborted);

      if (!aborted) {
        mRawBuffer.setPtr(ptr);
        res = 1; // success
        if (mRawBuffer.isEmpty()) {
          mRawBuffer.clear();
        }
      } else { // try to seek to the next RDH, can be done only for 128b padded GBT words
        if (findNextRDH(mRawBuffer)) {
          ptr = mRawBuffer.getPtr();
          res = 1;
        } else {
          mRawBuffer.clear(); // did not find new RDH
        }
      } // try to seek to the next ...
    }
    return res;
  }

  //_____________________________________
  uint8_t* skimPaddedRUData(uint8_t* raw, PayLoadCont& outBuffer, bool& aborted)
  {
    /// Skim CRU data with 128b-padded GBT words and fixed 8KB pages to 80b-GBT words and
    /// page size corresponding to real payload.

    aborted = false;

    // data must start by RDH
    auto rdh = reinterpret_cast<const o2::header::RAWDataHeader*>(raw);
#ifdef _RAW_READER_ERROR_CHECKS_
    if (!isRDHHeuristic(rdh)) {
      LOG(ERROR) << "Page does not start with RDH";
      for (int i = 0; i < 4; i++) {
        auto gbtD = reinterpret_cast<const o2::itsmft::GBTData*>(raw + i * 16);
        gbtD->printX(mPadding128);
      }
      aborted = true;
      return raw;
    }
#endif

    int ruIDSW = MAP.FEEId2RUSW(rdh->feeId);
    auto& ruDecode = getCreateRUDecode(ruIDSW);
    auto ruInfo = MAP.getRUInfoSW(ruIDSW);

    mInteractionRecord.bc = rdh->triggerBC;
    mInteractionRecord.orbit = rdh->triggerOrbit;

    mTrigger = rdh->triggerType;

    mInteractionRecordHB.bc = rdh->heartbeatBC;
    mInteractionRecordHB.orbit = rdh->heartbeatOrbit;

    auto& ruStat = ruDecode.statistics;
    ruStat.nPackets++;
    mDecodingStat.nRUsProcessed++;

    int sizeAtEntry = outBuffer.getSize(); // save the size of outbuffer size at entry, in case of severe error we will need to rewind to it.

    while (1) {
      mDecodingStat.nPagesProcessed++;
      mDecodingStat.nBytesProcessed += rdh->memorySize;
      raw += rdh->headerSize;
      // number of 128 b GBT words excluding header/trailer
      int nGBTWords = (rdh->memorySize - rdh->headerSize) / o2::itsmft::GBTPaddedWordLength - 2;
      auto gbtH = reinterpret_cast<const o2::itsmft::GBTDataHeader*>(raw); // process GBT header

#ifdef _RAW_READER_ERROR_CHECKS_
      if (mVerbose) {
        printRDH(*rdh);
        gbtH->printX(true);
        LOG(INFO) << "Expect " << nGBTWords << " GBT words";
      }
      if (!gbtH->isDataHeader()) {
        gbtH->printX(true);
        LOG(ERROR) << "FEE#" << rdh->feeId << " GBT payload header was expected, abort page decoding";
        gbtH->printX(true);
        ruStat.errorCounts[RUDecodingStat::ErrMissingGBTHeader]++;
        aborted = true;
        outBuffer.shrinkToSize(sizeAtEntry); // reset output buffer to initial state
        return raw;
      }
      if (gbtH->getPacketID() != rdh->pageCnt) {
        LOG(ERROR) << "FEE#" << rdh->feeId << " Different GBT header " << gbtH->getPacketID()
                   << " and RDH page " << rdh->pageCnt << " counters";
        ruStat.errorCounts[RUDecodingStat::ErrRDHvsGBTHPageCnt]++;
      }

      if (ruStat.lanesActive == ruStat.lanesStop) { // all lanes received their stop, new page 0 expected
        if (rdh->pageCnt) {                         // flag lanes of this FEE
          LOG(ERROR) << "FEE#" << rdh->feeId << " Non-0 page counter (" << rdh->pageCnt << ") while all lanes were stopped";
          ruStat.errorCounts[RUDecodingStat::ErrNonZeroPageAfterStop]++;
        }
      }

      ruStat.lanesActive = gbtH->getLanes(); // TODO do we need to update this for every page?

      if (!rdh->pageCnt) { // reset flags
        ruStat.lanesStop = 0;
        ruStat.lanesWithData = 0;
      }

#endif
      // start writting skimmed data for this page, making sure the buffer has enough free slots
      outBuffer.ensureFreeCapacity(8 * 1024);
      auto rdhS = reinterpret_cast<o2::header::RAWDataHeader*>(outBuffer.getEnd()); // save RDH and make saved copy editable
      outBuffer.addFast(reinterpret_cast<const uint8_t*>(rdh), rdh->headerSize);

      outBuffer.addFast(reinterpret_cast<const uint8_t*>(gbtH), mGBTWordSize); // save gbt header w/o 128b padding

      raw += o2::itsmft::GBTPaddedWordLength;
      for (int iw = 0; iw < nGBTWords; iw++, raw += o2::itsmft::GBTPaddedWordLength) {
        auto gbtD = reinterpret_cast<const o2::itsmft::GBTData*>(raw);
        // TODO: need to clarify if the nGBTWords from the rdh->memorySize is reliable estimate of the real payload, at the moment this is not the case

        if (mVerbose) {
          printf("W%4d |", iw);
          gbtD->printX(mPadding128);
        }
        if (gbtD->isDataTrailer()) {
          nGBTWords = iw;
          break; // this means that the nGBTWords estimate was wrong
        }

        int cableHW = gbtD->getCableID();
        int cableSW = MAP.cableHW2SW(ruInfo->ruType, cableHW);

        outBuffer.addFast(reinterpret_cast<const uint8_t*>(gbtD), mGBTWordSize); // save gbt word w/o 128b padding

#ifdef _RAW_READER_ERROR_CHECKS_
        ruStat.lanesWithData |= 0x1 << cableSW;    // flag that the data was seen on this lane
        if (ruStat.lanesStop & (0x1 << cableSW)) { // make sure stopped lanes do not transmit the data
          ruStat.errorCounts[RUDecodingStat::ErrDataForStoppedLane]++;
          LOG(ERROR) << "FEE#" << rdh->feeId << " Data received for stopped lane " << cableHW << " (sw:" << cableSW << ")";
        }
#endif

      } // we are at the trailer, packet is over, check if there are more for the same ru

      auto gbtT = reinterpret_cast<const o2::itsmft::GBTDataTrailer*>(raw); // process GBT trailer
#ifdef _RAW_READER_ERROR_CHECKS_

      if (mVerbose) {
        gbtT->printX(true);
      }

      if (!gbtT->isDataTrailer()) {
        gbtT->printX(true);
        LOG(ERROR) << "FEE#" << rdh->feeId << " GBT payload trailer was expected, abort page decoding at NW" << nGBTWords;
        ruStat.errorCounts[RUDecodingStat::ErrMissingGBTTrailer]++;
        aborted = true;
        outBuffer.shrinkToSize(sizeAtEntry); // reset output buffer to initial state
        return raw;
      }

      ruStat.lanesTimeOut |= gbtT->getLanesTimeout(); // register timeouts
      ruStat.lanesStop |= gbtT->getLanesStop();       // register stops
#endif

      outBuffer.addFast(reinterpret_cast<const uint8_t*>(gbtT), mGBTWordSize); // save gbt trailer w/o 128b padding

      raw += o2::itsmft::GBTPaddedWordLength;

      // we finished the GBT page, register in the stored RDH the memory size and new offset
      rdhS->memorySize = rdhS->headerSize + (2 + nGBTWords) * mGBTWordSize;
      rdhS->offsetToNext = rdhS->memorySize;

      if (!rdh->offsetToNext) { // RS TODO: what the last page in memory will contain as offsetToNext, is it 0?
        break;
      }

      raw = ((uint8_t*)rdh) + rdh->offsetToNext; // jump to the next packet:
      auto rdhN = reinterpret_cast<const o2::header::RAWDataHeader*>(raw);
      // check if data of given RU are over, i.e. we the page counter was wrapped to 0 (should be enough!) or other RU/trigger started
      if (!isSameRUandTrigger(rdh, rdhN)) {

#ifdef _RAW_READER_ERROR_CHECKS_
        // make sure all lane stops for finished page are received
        if (ruStat.lanesActive != ruStat.lanesStop) {
          if (rdh->triggerType != o2::trigger::SOT) { // only SOT trigger allows unstopped lanes?
            LOG(ERROR) << "FEE#" << rdh->feeId << " end of FEE data but not all lanes received stop";
            ruStat.errorCounts[RUDecodingStat::ErrUnstoppedLanes]++;
          }
        }

        // make sure all active lanes (except those in time-out) have sent some data
        if ((~ruStat.lanesWithData & ruStat.lanesActive) != ruStat.lanesTimeOut) {
          LOG(ERROR) << "FEE#" << rdh->feeId << " Lanes not in time-out but not sending data";
          ruStat.errorCounts[RUDecodingStat::ErrNoDataForActiveLane]++;
        }

        // accumulate packet states
        ruStat.packetStates[gbtT->getPacketState()]++;
#endif

        break;
      }
#ifdef _RAW_READER_ERROR_CHECKS_
      // check if the page counter increases
      if (rdhN->pageCnt != rdh->pageCnt + 1) {
        LOG(ERROR) << "FEE#" << rdh->feeId << " Discontinuity in the RDH page counter of the same RU trigger: old "
                   << rdh->pageCnt << " new: " << rdhN->pageCnt;
        ruStat.errorCounts[RUDecodingStat::ErrPageCounterDiscontinuity]++;
      }
#endif
      rdh = rdhN;
    }

#ifdef _RAW_READER_ERROR_CHECKS_
//    if (rdh->pageCnt && !rdh->stop) {
//      LOG(WARNING) << "Last packet(" << rdh->pageCnt << ") of GBT multi-packet is reached w/o STOP set in the RDH";
//    }
#endif

    return raw;
  }

  //_____________________________________
  bool isSameRUandTrigger(const o2::header::RAWDataHeader* rdhOld, const o2::header::RAWDataHeader* rdhNew) const
  {
    /// check if the rdhNew is just a continuation of the data described by the rdhOld
    if (rdhNew->pageCnt == 0 || rdhNew->feeId != rdhOld->feeId ||
        rdhNew->triggerOrbit != rdhOld->triggerOrbit ||
        rdhNew->triggerBC != rdhOld->triggerBC ||
        rdhNew->heartbeatOrbit != rdhOld->heartbeatOrbit ||
        rdhNew->heartbeatBC != rdhOld->heartbeatBC ||
        !(rdhNew->triggerType & rdhOld->triggerType)) {
      return false;
    }
    return true;
  }

  //_____________________________________
  int decodeAlpideData(RUDecodeData& decData)
  {
    /// decode the ALPIDE data from the buffer of single lane

    auto* chipData = &decData.chipsData[0];
    auto& ruStat = decData.statistics;

    decData.nChipsFired = decData.lastChipChecked = 0;
    int ntot = 0;
    for (int icab = 0; icab < decData.nCables; icab++) {
      auto& cableData = decData.cableData[icab];
      int res = 0;

#ifdef _RAW_READER_ERROR_CHECKS_
      // make sure the lane data starts with chip header or empty chip
      uint8_t h;
      if (cableData.current(h) && !mCoder.isChipHeaderOrEmpty(h)) {
        LOG(ERROR) << "FEE#" << decData.ruInfo->idHW << " cable " << icab << " data does not start with ChipHeader or ChipEmpty";
        ruStat.errorCounts[RUDecodingStat::ErrCableDataHeadWrong]++;
      }
#endif

      while ((res = mCoder.decodeChip(*chipData, cableData))) { // we register only chips with hits or errors flags set
        if (res > 0) {
#ifdef _RAW_READER_ERROR_CHECKS_
          // for the IB staves check if the cable ID is the same as the chip ID on the module
          if (decData.ruInfo->ruType == 0) { // ATTENTION: this is a hack tailored for temporary check
            if (chipData->getChipID() != icab) {
              LOG(ERROR) << "FEE#" << decData.ruInfo->idHW << " IB cable " << icab << " shipped chip ID= " << chipData->getChipID();
              ruStat.errorCounts[RUDecodingStat::ErrIBChipLaneMismatch]++;
            }
          }
#endif
          // convert HW chip id within the module to absolute chip id
          chipData->setChipID(MAP.getGlobalChipID(chipData->getChipID(), decData.cableHWID[icab], *decData.ruInfo));
          chipData->setInteractionRecord(mInteractionRecord);
          chipData->setTrigger(mTrigger);
          mDecodingStat.nNonEmptyChips++;
          mDecodingStat.nHitsDecoded += chipData->getData().size();
          ntot += res;
          // fetch next free chip
          if (++decData.nChipsFired < MaxChipsPerRU) {
            chipData = &decData.chipsData[decData.nChipsFired];
          } else {
            break; // last chip decoded
          }
        }
      }
    }
    return ntot;
  }

  //_____________________________________
  bool getNextChipData(ChipPixelData& chipData) override
  {
    /// read single chip data to the provided container

    if (mCurRUDecodeID >= 0) { // make sure current RU has fired chips to extract
      for (; mCurRUDecodeID < mNRUs; mCurRUDecodeID++) {
        auto& ru = mRUDecodeVec[mCurRUDecodeID];
        if (ru.lastChipChecked < ru.nChipsFired) {
          chipData.swap(ru.chipsData[ru.lastChipChecked++]);
          return true;
        }
      }
      mCurRUDecodeID = 0; // no more decoded data if reached this place,
    }
    // will need to decode new trigger
    if (mMinTriggersCached < 2) { // last trigger might be incomplete, need to cache more data
      cacheLinksData(mRawBuffer);
    }
    if (mMinTriggersCached < 1 || !decodeNextTrigger()) {
      mCurRUDecodeID = -1;
      return false; // nothing left
    }
    return getNextChipData(chipData); // is it ok to use recursion here?
  }

  //_____________________________________
  void openInput(const std::string filename)
  {
    // open input for raw data decoding from file
    mSWIO.Stop();
    mSWIO.Start();
    LOG(INFO) << "opening raw data input file " << filename << FairLogger::endl;
    mIOFile.open(filename.c_str(), std::ifstream::binary);
    assert(mIOFile.good());
    mRawBuffer.clear();
    mRawBuffer.expand(RawBufferSize);
    mSWIO.Stop();
  }

  //_____________________________________
  size_t loadInput(PayLoadCont& buffer)
  {
    /// assure the buffers are large enough
    static_assert(RawBufferMargin > MaxGBTPacketBytes * 100 &&
                    RawBufferSize > 3 * RawBufferMargin,
                  "raw buffer size is too small");

    if (!mIOFile) {
      return 0;
    }
    if (buffer.getUnusedSize() > RawBufferMargin) { // bytes read but not used yet are enough
      return 0;
    }
    mSWIO.Start(false);
    auto readFromFile = [this](uint8_t* ptr, int n) {
      mIOFile.read(reinterpret_cast<char*>(ptr), n);
      return mIOFile.gcount(); // fread( ptr, sizeof(uint8_t), n, mIOFile);
    };
    auto nread = buffer.append(readFromFile);
    mDecodingStat.nBytesProcessed += nread;
    mSWIO.Stop();
    return nread;
  }

  // get statics of FEE with sequential idSW
  const RUDecodingStat* getRUDecodingStatSW(uint16_t idSW) const
  {
    return mRUEntry[idSW] < 0 ? nullptr : &mRUDecodeVec[mRUEntry[idSW]].statistics;
  }

  // get statics of FEE with given HW id
  const RUDecodingStat* getRUDecodingStatHW(uint16_t idHW) const
  {
    int idsw = MAP.FEEId2RUSW(idHW);
    assert(idsw != 0xffff);
    return getRUDecodingStatSW(idsw);
  }

  // get global decoding statistics
  const RawDecodingStat& getDecodingStat() const { return mDecodingStat; }

  void setVerbosity(int v) { mVerbose = v; }
  int getVerbosity() const { return mVerbose; }

  Mapping& getMapping() { return MAP; }

  // get currently processed RU container
  const RUDecodeData* getCurrRUDecodeData() const { return mCurRUDecodeID < 0 ? nullptr : &mRUDecodeVec[mCurRUDecodeID]; }

  PayLoadCont& getRawBuffer() { return mRawBuffer; }

  // number of links seen in the data
  int getNLinks() const { return mNLinks; }

  // number of RUs seen in the data
  int getNRUs() const { return mNRUs; }

  // get vector of RU decode containers for RUs seen in the data
  const std::array<RUDecodeData, ChipMappingITS::getNRUs()>& getRUDecodeVec() const { return mRUDecodeVec; }

  const std::array<int, ChipMappingITS::getNRUs()>& getRUEntries() const { return mRUEntry; }

  // get RU decode container for RU with given SW ID
  const RUDecodeData* getRUDecode(int ruSW) const
  {
    return mRUEntry[ruSW] < 0 ? nullptr : &mRUDecodeVec[mRUEntry[ruSW]];
  }

  // get RU decode container for RU with given SW ID, if does not exist, create it
  RUDecodeData& getCreateRUDecode(int ruSW)
  {
    assert(ruSW < MAP.getNRUs());
    if (mRUEntry[ruSW] < 0) {
      mRUEntry[ruSW] = mNRUs++;
      mRUDecodeVec[mRUEntry[ruSW]].ruInfo = MAP.getRUInfoSW(ruSW); // info on the stave/RU
      LOG(INFO) << "Defining container for RU " << ruSW << " at slot " << mRUEntry[ruSW];
    }
    return mRUDecodeVec[mRUEntry[ruSW]];
  }

 private:
  std::ifstream mIOFile;
  Coder mCoder;
  Mapping MAP;
  int mVerbose = 0;            //! verbosity level
  int mCurRUDecodeID = -1;     //! index of currently processed RUDecode container

  PayLoadCont mRawBuffer; //! buffer for binary raw data file IO

  std::array<RUDecodeData, ChipMappingITS::getNRUs()> mRUDecodeVec; // decoding buffers for all active RUs
  std::array<int, ChipMappingITS::getNRUs()> mRUEntry;              //! entry of the RU with given SW ID in the  mRULinks
  int mNRUs = 0;                                                    //! total number of RUs seen
  int mNLinks = 0;                                                  //! total number of GBT links seen

  //! min number of triggers to cache per link (keep this > N pages per CRU superpage)
  int mMinTriggersToCache = NCRUPagesPerSuperpage + 10;
  int mMinTriggersCached = 0; //! actual minimum (among different links) number of triggers to cache

  // statistics
  RawDecodingStat mDecodingStat;                                  //! global decoding statistics

  TStopwatch mSWIO; //! timer for IO operations

  static constexpr int RawBufferMargin = 5000000;                      // keep uploaded at least this amount
  static constexpr int RawBufferSize = 10000000 + 2 * RawBufferMargin; // size in MB
  bool mPadding128 = true;                                             // is payload padded to 128 bits
  bool mImposeMaxPage = true;                                          // standard CRU data comes in 8KB pages
  // number of bytes the GBT word, including optional padding to 128 bits
  int mGBTWordSize = mPadding128 ? o2::itsmft::GBTPaddedWordLength : o2::itsmft::GBTWordLength;

  ClassDefOverride(RawPixelReader, 1);
};

template <class Mapping>
constexpr int RawPixelReader<Mapping>::RawBufferMargin;

template <class Mapping>
constexpr int RawPixelReader<Mapping>::RawBufferSize;

} // namespace itsmft
} // namespace o2

#endif /* ALICEO2_ITS_RAWPIXELREADER_H */
