#include <VegetationPlugin/VegetationPluginPCH.h>

#include <VegetationPlugin/World/VegetationCompositeSplit.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

namespace
{
  enum Band : plUInt32
  {
    AllNearer = 0,
    AllFurther = 1,
    Mixed = 2,
  };

  /// Nearer: distance < threshold (switch) or <= threshold (culls), as ClassifyTree compares.
  Band ClassifyBand(float fMinDistance, float fMaxDistance, float fMinThreshold, float fMaxThreshold, bool bInclusive)
  {
    if (bInclusive ? fMaxDistance <= fMinThreshold : fMaxDistance < fMinThreshold)
      return AllNearer;
    if (bInclusive ? fMinDistance > fMaxThreshold : fMinDistance >= fMaxThreshold)
      return AllFurther;
    return Mixed;
  }
} // namespace

void plVegetationCompositeSplit::Reset()
{
  m_States.Clear();
  m_Positions = plBoundingBox::MakeInvalid();
  m_uiBandKey = 0xFFFFFFFF;
  m_bValid = false;
  m_bEvaluatedPerTree = false;
  m_uiCounts[0] = m_uiCounts[1] = m_uiCounts[2] = 0;
}

plUInt8 plVegetationCompositeSplit::ClassifyTree(const plVegetationCellInstance& inst, float fDistance, const plVegetationCompositeThresholds& thresholds)
{
  // The colour seed doubles as a stable per-instance random value, as for the other types. There is no distance
  // thinning: the merged mesh is cut on the GPU and gets cheap with distance on its own.
  if (!inst.m_Flags.IsSet(plVegetationInstanceFlags::Hero) && (inst.m_uiColorSeed + 0.5f) / 256.0f > thresholds.m_fDensity)
    return plVegetationCompositeTreeState::Hidden;

  if (fDistance > thresholds.m_fCullDistance)
    return plVegetationCompositeTreeState::Hidden;

  plUInt8 uiState = fDistance < thresholds.m_fSwitchDistance * inst.m_fScale ? plVegetationCompositeTreeState::Parts : plVegetationCompositeTreeState::Merged;
  if (fDistance <= thresholds.m_fShadowCullDistance && !inst.m_Flags.IsSet(plVegetationInstanceFlags::NoShadow))
    uiState |= plVegetationCompositeTreeState::CastsShadow;

  return uiState;
}

plUInt32 plVegetationCompositeSplit::ComputeBandKey(const plVec3& vCamera, const plVegetationCompositeThresholds& thresholds) const
{
  const float fMinDistance = m_Positions.GetDistanceTo(vCamera);

  plVec3 corners[8];
  m_Positions.GetCorners(corners);
  float fMaxDistanceSq = 0.0f;
  for (const plVec3& vCorner : corners)
    fMaxDistanceSq = plMath::Max(fMaxDistanceSq, (vCorner - vCamera).GetLengthSquared());
  const float fMaxDistance = plMath::Sqrt(fMaxDistanceSq);

  const Band switchBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fSwitchDistance * m_fMinScale, thresholds.m_fSwitchDistance * m_fMaxScale, false);
  const Band cullBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fCullDistance, thresholds.m_fCullDistance, true);
  const Band shadowBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fShadowCullDistance, thresholds.m_fShadowCullDistance, true);

  return switchBand | (cullBand << 2) | (shadowBand << 4);
}

bool plVegetationCompositeSplit::Update(plArrayPtr<const plVegetationCellInstance> instances, const plVec3& vCamera, const plVegetationCompositeThresholds& thresholds)
{
  m_bEvaluatedPerTree = false;

  if (!m_bValid || m_States.GetCount() != instances.GetCount())
  {
    // New or changed instances (the cell resets its state when its version changes): bounds and every state again.
    Reset();
    m_States.SetCount(instances.GetCount());
    m_fMinScale = plMath::MaxValue<float>();
    m_fMaxScale = 0.0f;
    for (const plVegetationCellInstance& inst : instances)
    {
      m_Positions.ExpandToInclude(inst.m_vPosition);
      m_fMinScale = plMath::Min(m_fMinScale, inst.m_fScale);
      m_fMaxScale = plMath::Max(m_fMaxScale, inst.m_fScale);
    }
    if (instances.IsEmpty())
    {
      m_fMinScale = m_fMaxScale = 1.0f;
      m_Positions = plBoundingBox::MakeFromMinMax(plVec3::MakeZero(), plVec3::MakeZero());
    }
  }

  const bool bInputsChanged = !m_bValid || !(thresholds == m_Thresholds);
  if (!bInputsChanged && vCamera == m_vCamera)
    return false;

  const plUInt32 uiBandKey = ComputeBandKey(vCamera, thresholds);
  m_vCamera = vCamera;

  // Every threshold lies on the same side of the whole cell as before: no tree crossed one.
  const bool bStraddles = (uiBandKey & 0x3) == Mixed || ((uiBandKey >> 2) & 0x3) == Mixed || ((uiBandKey >> 4) & 0x3) == Mixed;
  if (!bInputsChanged && !bStraddles && uiBandKey == m_uiBandKey)
    return false;

  m_uiBandKey = uiBandKey;
  m_Thresholds = thresholds;
  m_bValid = true;
  m_bEvaluatedPerTree = true;

  bool bChanged = bInputsChanged;
  plUInt32 uiCounts[3] = {};
  for (plUInt32 i = 0; i < instances.GetCount(); ++i)
  {
    const plVegetationCellInstance& inst = instances[i];
    const plUInt8 uiState = ClassifyTree(inst, (inst.m_vPosition - vCamera).GetLength(), thresholds);
    bChanged |= uiState != m_States[i];
    m_States[i] = uiState;
    ++uiCounts[uiState & plVegetationCompositeTreeState::MainMask];
  }

  for (plUInt32 s = 0; s < 3; ++s)
    m_uiCounts[s] = uiCounts[s];

  return bChanged;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_World_VegetationCompositeSplit);
