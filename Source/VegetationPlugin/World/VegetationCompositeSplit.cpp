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
  m_uiCounts[0] = m_uiCounts[1] = m_uiCounts[2] = m_uiCounts[3] = 0;
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

  // The cell's voxel shell beyond its hand-off band; inside it the tree and the shell both draw and dither.
  const bool bCell = thresholds.m_fCellFar > 0.0f;
  bool bInCell = false;
  if (bCell)
  {
    if (fDistance >= thresholds.m_fCellFar)
    {
      uiState = plVegetationCompositeTreeState::Hidden | plVegetationCompositeTreeState::InCell;
      bInCell = true;
    }
    else if (fDistance > thresholds.m_fCellNear)
    {
      uiState = plVegetationCompositeTreeState::Merged | plVegetationCompositeTreeState::CellFade;
    }
  }

  // Impostor beyond its hand-off band; inside it the impostor dithers against the merged mesh, or the shell.
  if (thresholds.m_fImpostorFar > 0.0f)
  {
    if (fDistance >= thresholds.m_fImpostorFar)
    {
      uiState = plVegetationCompositeTreeState::Impostor;
      bInCell = false;
    }
    else if (fDistance > thresholds.m_fImpostorNear)
    {
      uiState = (bInCell ? (plVegetationCompositeTreeState::Hidden | plVegetationCompositeTreeState::InCell) : plVegetationCompositeTreeState::Merged) | plVegetationCompositeTreeState::ImpostorFade;
    }
  }

  // The shell casts the shadows of the trees it holds, from the middle of its hand-off on.
  const bool bShellShadow = bCell && fDistance >= (thresholds.m_fCellNear + thresholds.m_fCellFar) * 0.5f;
  if (fDistance <= thresholds.m_fShadowCullDistance && !inst.m_Flags.IsSet(plVegetationInstanceFlags::NoShadow) && !bShellShadow)
  {
    uiState |= plVegetationCompositeTreeState::CastsShadow;
    // Every shadow view gets the cell's shadow batches, so a part instance there costs as many times as there are
    // shadow views: beyond a short distance the merged mesh casts the shadow, a difference no shadow texel shows.
    if ((uiState & plVegetationCompositeTreeState::MainMask) == plVegetationCompositeTreeState::Parts && fDistance < thresholds.m_fShadowPartsDistance * inst.m_fScale)
      uiState |= plVegetationCompositeTreeState::ShadowParts;

    // Shadows switch at one distance, without a band: they do not dither.
    if (thresholds.m_fShadowImpostorDistance > 0.0f && fDistance >= thresholds.m_fShadowImpostorDistance)
      uiState |= plVegetationCompositeTreeState::ShadowImpostor;
  }

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
  const Band shadowPartsBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fShadowPartsDistance * m_fMinScale, thresholds.m_fShadowPartsDistance * m_fMaxScale, false);

  // The far bands are plain distances.
  const Band impostorNearBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fImpostorNear, thresholds.m_fImpostorNear, false);
  const Band impostorFarBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fImpostorFar, thresholds.m_fImpostorFar, false);
  const Band shadowImpostorBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fShadowImpostorDistance, thresholds.m_fShadowImpostorDistance, false);
  const Band cellNearBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fCellNear, thresholds.m_fCellNear, false);
  const float fCellMid = (thresholds.m_fCellNear + thresholds.m_fCellFar) * 0.5f;
  const Band cellMidBand = ClassifyBand(fMinDistance, fMaxDistance, fCellMid, fCellMid, false);
  const Band cellFarBand = ClassifyBand(fMinDistance, fMaxDistance, thresholds.m_fCellFar, thresholds.m_fCellFar, false);

  return switchBand | (cullBand << 2) | (shadowBand << 4) | (shadowPartsBand << 6) | (impostorNearBand << 8) | (impostorFarBand << 10) | (shadowImpostorBand << 12) | (cellNearBand << 14) | (cellMidBand << 16) | (cellFarBand << 18);
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
  bool bStraddles = false;
  for (plUInt32 uiShift = 0; uiShift < 20; uiShift += 2)
    bStraddles |= ((uiBandKey >> uiShift) & 0x3) == Mixed;
  if (!bInputsChanged && !bStraddles && uiBandKey == m_uiBandKey)
    return false;

  m_uiBandKey = uiBandKey;
  m_Thresholds = thresholds;
  m_bValid = true;
  m_bEvaluatedPerTree = true;

  bool bChanged = bInputsChanged;
  plUInt32 uiCounts[4] = {};
  for (plUInt32 i = 0; i < instances.GetCount(); ++i)
  {
    const plVegetationCellInstance& inst = instances[i];
    const plUInt8 uiState = ClassifyTree(inst, (inst.m_vPosition - vCamera).GetLength(), thresholds);
    bChanged |= uiState != m_States[i];
    m_States[i] = uiState;
    ++uiCounts[uiState & plVegetationCompositeTreeState::MainMask];
  }

  for (plUInt32 s = 0; s < 4; ++s)
    m_uiCounts[s] = uiCounts[s];

  return bChanged;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_World_VegetationCompositeSplit);
