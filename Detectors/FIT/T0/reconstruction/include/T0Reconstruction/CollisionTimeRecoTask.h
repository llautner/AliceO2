// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \file CollisionTimeRecoTask.h
/// \brief Definition of the FIT collision time reconstruction task
#ifndef ALICEO2_FIT_COLLISIONTIMERECOTASK_H
#define ALICEO2_FIT_COLLISIONTIMERECOTASK_H

#include <vector>
#include "FITBase/Digit.h"
#include "T0Reconstruction/RecPoints.h"
namespace o2
{
namespace t0
{
class CollisionTimeRecoTask
{
 public:
  CollisionTimeRecoTask() = default;
  ~CollisionTimeRecoTask() = default;
  void Process(const o2::fit::Digit& digits, RecPoints& recPoints) const;
  void FinishTask();

 private:
  ClassDefNV(CollisionTimeRecoTask, 1);
};
} // namespace t0
} // namespace o2
#endif
