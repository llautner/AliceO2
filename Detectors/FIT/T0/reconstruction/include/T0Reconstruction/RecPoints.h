// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file RecPoints.h
/// \brief Definition of the FIT RecPoints class
#ifndef ALICEO2_FIT_RECPOINTS_H
#define ALICEO2_FIT_RECPOINTS_H

#include "CommonDataFormat/TimeStamp.h"
#include <array>
#include "Rtypes.h"
#include <TObject.h>
#include <FITBase/Digit.h>

namespace o2
{
namespace t0
{
struct Channel {
  Int_t index;
  Float_t time, amp;
  ClassDefNV(Channel, 1);
};
class RecPoints
{
 public:
  RecPoints() = default;
  RecPoints(const std::array<Float_t, 3>& collisiontime,
            Float_t vertex,
            std::vector<o2::fit::ChannelData> timeamp)
    : mCollisionTime(collisiontime),
      mVertex(vertex),
      mTimeAmp(std::move(timeamp))
  {
  }
  ~RecPoints() = default;

  void FillFromDigits(const o2::fit::Digit& digit);
  Float_t GetCollisionTime(int side) const { return mCollisionTime[side]; }
  void setCollisionTime(Float_t time, int side) { mCollisionTime[side] = time; }

  Float_t GetTimeFromDigit() const { return mEventTime; }
  void setTimFromDigit(Float_t time) { mEventTime = time; }

  Float_t GetVertex(Float_t vertex) const { return mVertex; }
  void setVertex(Float_t vertex) { mVertex = vertex; }

  void SetMgrEventTime(Double_t time) { mEventTime = time; }
  void setBC(Int_t bc) { mBC = bc; }
  void setOrbit(Int_t orbit) { mOrbit = orbit; }

  const std::vector<o2::fit::ChannelData>& getChDgData() const { return mTimeAmp; }
  void setChDgData(const std::vector<o2::fit::ChannelData>& TimeAmp) { mTimeAmp = TimeAmp; }
  void setChDgData(std::vector<o2::fit::ChannelData>&& TimeAmp) { mTimeAmp = std::move(TimeAmp); }

 private:
  std::array<Float_t, 3> mCollisionTime;
  Float_t mVertex = 0;
  Double_t mEventTime; //event time from Fair for continuous
  std::vector<o2::fit::ChannelData> mTimeAmp;
  Int_t mBC = 0;    // BC from digits
  Int_t mOrbit = 0; // orbit from digits

  ClassDefNV(RecPoints, 1);
};
} // namespace t0
} // namespace o2
#endif
