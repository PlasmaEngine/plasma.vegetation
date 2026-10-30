#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plGrassAtlasRect, plNoBase, 1, plRTTIDefaultAllocator<plGrassAtlasRect>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("TexMulAdd", m_vTexMulAdd)->AddAttributes(new plDefaultValueAttribute(plVec4(1, 1, 0, 0))),
    PL_MEMBER_PROPERTY("Size", m_fSize)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 10.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeResource, 1, plRTTIDefaultAllocator<plGrassTypeResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plGrassTypeResource);

//////////////////////////////////////////////////////////////////////////

namespace
{
  float SanitizeFloat(float fValue, float fMin, float fMax, float fDefault)
  {
    return plMath::IsFinite(fValue) ? plMath::Clamp(fValue, fMin, fMax) : fDefault;
  }
} // namespace

bool plGrassSettings::RequiresRegrow(const plGrassSettings& other) const
{
  if (m_fStrandsPerSquareMeter != other.m_fStrandsPerSquareMeter || m_uiSegmentCount != other.m_uiSegmentCount || m_uiRandomSeed != other.m_uiRandomSeed ||
      m_fLength != other.m_fLength || m_fWidth != other.m_fWidth || m_fRandomness != other.m_fRandomness || m_fUniformity != other.m_fUniformity ||
      m_bProceduralBlades != other.m_bProceduralBlades || m_fClumpSize != other.m_fClumpSize || m_fBladeWidth != other.m_fBladeWidth ||
      m_fGroundCoverMaxSize != other.m_fGroundCoverMaxSize)
    return true;

  if (m_AtlasRects.GetCount() != other.m_AtlasRects.GetCount())
    return true;
  for (plUInt32 i = 0; i < m_AtlasRects.GetCount(); ++i)
  {
    if (!(m_AtlasRects[i] == other.m_AtlasRects[i]))
      return true;
  }

  return false;
}

void plGrassSettings::Sanitize()
{
  m_fStrandsPerSquareMeter = SanitizeFloat(m_fStrandsPerSquareMeter, 0.0f, 10000.0f, 120.0f);
  m_uiSegmentCount = plMath::Clamp(m_uiSegmentCount, 1u, 10u);
  m_uiBillboardCount = plMath::Clamp(m_uiBillboardCount, 1u, 10u);
  m_fLength = SanitizeFloat(m_fLength, 0.0f, 100.0f, 1.0f);
  m_fWidth = SanitizeFloat(m_fWidth, 0.0f, 10.0f, 1.0f);
  m_fRandomness = SanitizeFloat(m_fRandomness, 0.0f, 1.0f, 0.2f);
  m_fUniformity = SanitizeFloat(m_fUniformity, 0.01f, 100.0f, 1.0f);
  m_fBladeWidth = SanitizeFloat(m_fBladeWidth, 0.001f, 1.0f, 0.06f);
  m_fBladeTilt = SanitizeFloat(m_fBladeTilt, 0.0f, 0.8f, 0.15f);
  m_fBladeBend = SanitizeFloat(m_fBladeBend, 0.0f, 0.8f, 0.35f);
  m_fBladeSideCurve = SanitizeFloat(m_fBladeSideCurve, 0.0f, 0.3f, 0.08f);
  m_fClumpSize = SanitizeFloat(m_fClumpSize, 0.1f, 100.0f, 1.5f);
  m_fClumpStrength = SanitizeFloat(m_fClumpStrength, 0.0f, 1.0f, 0.65f);
  m_fClumpColorVariation = SanitizeFloat(m_fClumpColorVariation, 0.0f, 0.5f, 0.15f);
  m_fBladeNormalRoundness = SanitizeFloat(m_fBladeNormalRoundness, 0.0f, 1.0f, 0.65f);
  m_fFarDensity = SanitizeFloat(m_fFarDensity, 1.0f / 256.0f, 1.0f, 0.01f);
  m_fGroundCoverStartDistance = SanitizeFloat(m_fGroundCoverStartDistance, 1.0f, 10000.0f, 60.0f);
  m_fGroundCoverMaxSize = SanitizeFloat(m_fGroundCoverMaxSize, 0.01f, 4.0f, 2.0f);
  for (auto& rect : m_AtlasRects)
  {
    if (!rect.m_vTexMulAdd.IsValid())
      rect.m_vTexMulAdd = plVec4(1, 1, 0, 0);
    rect.m_fSize = SanitizeFloat(rect.m_fSize, 0.0f, 10.0f, 1.0f);
  }

  m_fStiffness = SanitizeFloat(m_fStiffness, 0.0f, 10.0f, 0.5f);
  m_fDrag = SanitizeFloat(m_fDrag, 0.0f, 1.0f, 0.1f);
  m_fGravityPower = SanitizeFloat(m_fGravityPower, 0.0f, 10.0f, 0.0f);
  m_fWindInfluence = SanitizeFloat(m_fWindInfluence, 0.0f, 10.0f, 1.0f);

  m_fTouchCrush = SanitizeFloat(m_fTouchCrush, 0.0f, 1.0f, 0.6f);
  m_fTouchRecovery = SanitizeFloat(m_fTouchRecovery, 0.0f, 100.0f, 1.5f);

  m_fViewDistance = SanitizeFloat(m_fViewDistance, 0.0f, 10000.0f, 200.0f);
  m_fSimulationDistance = SanitizeFloat(m_fSimulationDistance, 0.0f, 10000.0f, 40.0f);
  m_fLodStartDistance = SanitizeFloat(m_fLodStartDistance, 1.0f, 1000.0f, 15.0f);
  m_fShadowDistance = SanitizeFloat(m_fShadowDistance, 0.0f, 10000.0f, 25.0f);
  m_fPatchSize = SanitizeFloat(m_fPatchSize, 1.0f, 64.0f, 8.0f);
}

void plGrassSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fStrandsPerSquareMeter;
  inout_stream << m_uiSegmentCount;
  inout_stream << m_uiBillboardCount;
  inout_stream << m_uiRandomSeed;
  inout_stream << m_fLength;
  inout_stream << m_fWidth;
  inout_stream << m_fRandomness;
  inout_stream << m_fUniformity;

  inout_stream << m_AtlasRects.GetCount();
  for (const auto& rect : m_AtlasRects)
  {
    inout_stream << rect.m_vTexMulAdd;
    inout_stream << rect.m_fSize;
  }

  inout_stream << m_sMaterial;

  inout_stream << m_fStiffness;
  inout_stream << m_fDrag;
  inout_stream << m_fGravityPower;
  inout_stream << m_fWindInfluence;
  inout_stream << m_bCameraBend;

  inout_stream << m_fTouchCrush;
  inout_stream << m_fTouchRecovery;

  inout_stream << m_fViewDistance;
  inout_stream << m_fSimulationDistance;
  inout_stream << m_fLodStartDistance;
  inout_stream << m_fShadowDistance;
  inout_stream << m_bCastShadows;
  inout_stream << m_fPatchSize;
  inout_stream << m_bProceduralBlades << m_fBladeWidth << m_fBladeTilt << m_fBladeBend << m_fBladeSideCurve;
  inout_stream << m_fClumpSize << m_fClumpStrength << m_fClumpColorVariation << m_fBladeNormalRoundness;
  inout_stream << m_bAdaptiveDensity << m_fFarDensity;
  inout_stream << m_bFarGroundCover << m_fGroundCoverStartDistance << m_fGroundCoverMaxSize;
}

plResult plGrassSettings::Load(plStreamReader& inout_stream, plUInt8 uiVersion)
{
  // Reloading an older asset must also reset fields absent from its stream.
  *this = plGrassSettings();
  inout_stream >> m_fStrandsPerSquareMeter;
  inout_stream >> m_uiSegmentCount;
  inout_stream >> m_uiBillboardCount;
  inout_stream >> m_uiRandomSeed;
  inout_stream >> m_fLength;
  inout_stream >> m_fWidth;
  inout_stream >> m_fRandomness;
  inout_stream >> m_fUniformity;

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  if (uiCount > 1024)
  {
    plLog::Error("Grass type has an invalid atlas rect count {}", uiCount);
    return PL_FAILURE;
  }

  m_AtlasRects.SetCount(uiCount);
  for (auto& rect : m_AtlasRects)
  {
    inout_stream >> rect.m_vTexMulAdd;
    inout_stream >> rect.m_fSize;
  }

  inout_stream >> m_sMaterial;

  inout_stream >> m_fStiffness;
  inout_stream >> m_fDrag;
  inout_stream >> m_fGravityPower;
  inout_stream >> m_fWindInfluence;
  inout_stream >> m_bCameraBend;

  if (uiVersion < 2)
  {
    // Version 1 carries collision values nothing reads.
    bool bCollision = false;
    plUInt8 uiCollisionLayer = 0;
    float fCollisionRadius = 0.0f;
    inout_stream >> bCollision;
    inout_stream >> uiCollisionLayer;
    inout_stream >> fCollisionRadius;
  }
  inout_stream >> m_fTouchCrush;
  inout_stream >> m_fTouchRecovery;

  inout_stream >> m_fViewDistance;
  inout_stream >> m_fSimulationDistance;
  inout_stream >> m_fLodStartDistance;
  inout_stream >> m_fShadowDistance;
  m_bCastShadows = true;
  if (uiVersion >= 2)
    inout_stream >> m_bCastShadows;
  inout_stream >> m_fPatchSize;

  if (uiVersion >= 3)
  {
    inout_stream >> m_bProceduralBlades >> m_fBladeWidth >> m_fBladeTilt >> m_fBladeBend >> m_fBladeSideCurve;
    inout_stream >> m_fClumpSize >> m_fClumpStrength >> m_fClumpColorVariation >> m_fBladeNormalRoundness;
    inout_stream >> m_bAdaptiveDensity >> m_fFarDensity;
    inout_stream >> m_bFarGroundCover >> m_fGroundCoverStartDistance >> m_fGroundCoverMaxSize;
  }

  Sanitize();
  return PL_SUCCESS;
}

void plGrassTypeResourceDescriptor::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 3;
  inout_stream << uiVersion;

  m_Settings.Save(inout_stream);
}

plResult plGrassTypeResourceDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion < 1 || uiVersion > 3)
  {
    plLog::Error("Unknown grass type version {}", uiVersion);
    return PL_FAILURE;
  }

  return m_Settings.Load(inout_stream, uiVersion);
}

//////////////////////////////////////////////////////////////////////////

plGrassTypeResource::plGrassTypeResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plGrassTypeResource, plGrassTypeResourceDescriptor)
{
  m_Descriptor = descriptor;
  m_Descriptor.m_Settings.Sanitize();
  ResolveHandles();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

plResourceLoadDesc plGrassTypeResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Descriptor = plGrassTypeResourceDescriptor();
  m_hMaterial.Invalidate();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plGrassTypeResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plGrassTypeResource::UpdateContent", GetResourceIdOrDescription());

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;

  if (Stream == nullptr)
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  // skip the absolute file path data that the standard file reader writes into the stream
  {
    plStringBuilder sAbsFilePath;
    (*Stream) >> sAbsFilePath;
  }

  // skip the asset file header at the start of the file
  plAssetFileHeader AssetHash;
  AssetHash.Read(*Stream).IgnoreResult();

  if (m_Descriptor.Load(*Stream).Failed())
  {
    res.m_State = plResourceState::LoadedResourceMissing;
    return res;
  }

  ResolveHandles();

  res.m_State = plResourceState::Loaded;
  return res;
}

void plGrassTypeResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plGrassTypeResource);
}

void plGrassTypeResource::ResolveHandles()
{
  m_hMaterial.Invalidate();
  if (!m_Descriptor.m_Settings.m_sMaterial.IsEmpty())
  {
    m_hMaterial = plResourceManager::LoadResource<plMaterialResource>(m_Descriptor.m_Settings.m_sMaterial);
  }
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassTypeResource);
