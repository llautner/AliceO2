#ifndef ALICEO2_T0_DIGITIZATION_PARAMETERS
#define ALICEO2_T0_DIGITIZATION_PARAMETERS
#include <FITSimulation/DigitizationParameters.h>
#include <T0Base/Geometry.h>

namespace o2::t0
{
inline o2::fit::DigitizationParameters T0DigitizationParameters()
{
  o2::fit::DigitizationParameters result;
  result.NCellsA = Geometry::NCellsA;
  result.NCellsC = Geometry::NCellsC;
  result.ZdetA = Geometry::ZdetA;
  result.ZdetC = Geometry::ZdetC;
  result.ChannelWidth = Geometry::ChannelWidth;
  result.mBC_clk_center = 12.5;                               // clk center
  result.mMCPs = (Geometry::NCellsA + Geometry::NCellsC) * 4; //number of MCPs
  result.mCFD_trsh_mip = 0.4;                                 // = 4[mV] / 10[mV/mip]
  result.mTime_trg_gate = 4.;                                 // ns
  result.mAmpThreshold = 100;                                 // number of photoelectrons
  result.mTimeDiffAC = (Geometry::ZdetA - Geometry::ZdetC) * TMath::C();
  result.mIsT0 = true;

  return result;
}
} // namespace o2::t0
#endif
