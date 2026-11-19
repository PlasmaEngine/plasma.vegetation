#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeResource, 1, plRTTIDefaultAllocator<plVegetationTypeResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_STATIC_REFLECTED_ENUM(plVegetationRayTracingMode, 1)
  PL_ENUM_CONSTANTS(plVegetationRayTracingMode::Automatic, plVegetationRayTracingMode::Included, plVegetationRayTracingMode::Excluded)
PL_END_STATIC_REFLECTED_ENUM;
// clang-format on

namespace
{
  /// Types that stand at least this tall are part of the ray tracing scene when their mode is Automatic. Height
  /// separates ground cover from bushes and trees where the radius does not: a grass patch is wide but low.
  constexpr float c_fAutomaticRayTracingHeight = 1.2f;
} // namespace

plCVarInt cvar_VegetationRayTracingLodOverride("Vegetation.RayTracing.LodOverride", -1, plCVarFlags::Default, "Diagnostic: the LOD every vegetation type traces, instead of its own Ray Tracing Mesh setting. -1 = off.");

PL_RESOURCE_IMPLEMENT_COMMON_CODE(plVegetationTypeResource);

//////////////////////////////////////////////////////////////////////////

void plVegetationWindSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fBendStiffness;
  inout_stream << m_fBendHeight;
  inout_stream << m_fBranchAmplitude;
  inout_stream << m_fBranchFrequency;
  inout_stream << m_fEdgeAmplitude;
  inout_stream << m_fEdgeFrequency;
  inout_stream << m_fPhaseScale;
  inout_stream << m_fTouchStrength;
}

void plVegetationWindSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_fBendStiffness;
  inout_stream >> m_fBendHeight;
  inout_stream >> m_fBranchAmplitude;
  inout_stream >> m_fBranchFrequency;
  inout_stream >> m_fEdgeAmplitude;
  inout_stream >> m_fEdgeFrequency;
  inout_stream >> m_fPhaseScale;
  inout_stream >> m_fTouchStrength;
}

void plVegetationTouchSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fRecovery;
  inout_stream << m_fCrush;
  inout_stream << m_sCollisionPrefab;
  inout_stream << m_fCollisionRadius;
  inout_stream << m_sTouchTag;
}

void plVegetationTouchSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_fRecovery;
  inout_stream >> m_fCrush;
  inout_stream >> m_sCollisionPrefab;
  inout_stream >> m_fCollisionRadius;
  inout_stream >> m_sTouchTag;
}

void plVegetationPlacementSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fMinScale;
  inout_stream << m_fMaxScale;
  inout_stream << m_fAlignToNormal;
  inout_stream << m_MaxSlope;
  inout_stream << m_fSink;
  inout_stream << m_fMinSpacing;
  inout_stream << m_sColorGradient;
  inout_stream << m_fHueVariation;
}

void plVegetationPlacementSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_fMinScale;
  inout_stream >> m_fMaxScale;
  inout_stream >> m_fAlignToNormal;
  inout_stream >> m_MaxSlope;
  inout_stream >> m_fSink;
  inout_stream >> m_fMinSpacing;
  inout_stream >> m_sColorGradient;
  inout_stream >> m_fHueVariation;
}

void plVegetationPlacementSettings::SaveExtra(plStreamWriter& inout_stream) const
{
  inout_stream << m_fDensityWeight;
  inout_stream << m_fClearingAvoidance;
}

void plVegetationPlacementSettings::LoadExtra(plStreamReader& inout_stream)
{
  inout_stream >> m_fDensityWeight;
  inout_stream >> m_fClearingAvoidance;
}

void plVegetationRenderSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fCullDistance;
  inout_stream << m_fShadowCullDistance;
  inout_stream << m_fThinningStart;
  inout_stream << m_fLodCrossfade;
  inout_stream << m_bCastShadows;
  inout_stream << m_bAffectsGI;
  inout_stream << m_bUseImpostor;
}

void plVegetationRenderSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_fCullDistance;
  inout_stream >> m_fShadowCullDistance;
  inout_stream >> m_fThinningStart;
  inout_stream >> m_fLodCrossfade;
  inout_stream >> m_bCastShadows;
  inout_stream >> m_bAffectsGI;
  inout_stream >> m_bUseImpostor;
}

void plVegetationRayTracingSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_Mode;
  inout_stream << m_iLod;
  inout_stream << m_fRange;
}

void plVegetationRayTracingSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_Mode;
  inout_stream >> m_iLod;
  inout_stream >> m_fRange;
}

void plVegetationTypeResourceDescriptor::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 3;
  inout_stream << uiVersion;

  inout_stream << m_Lods.GetCount();
  for (const Lod& lod : m_Lods)
  {
    inout_stream << lod.m_sMesh;
    inout_stream << lod.m_fScreenSize;
  }

  inout_stream << m_Materials.GetCount();
  for (const plString& sMaterial : m_Materials)
  {
    inout_stream << sMaterial;
  }

  m_Wind.Save(inout_stream);
  m_Touch.Save(inout_stream);
  m_Placement.Save(inout_stream);
  m_Render.Save(inout_stream);
  m_RayTracing.Save(inout_stream);

  // version 3
  m_Placement.SaveExtra(inout_stream);
}

plResult plVegetationTypeResourceDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion < 1 || uiVersion > 3)
  {
    plLog::Error("Unknown vegetation type version {}", uiVersion);
    return PL_FAILURE;
  }

  plUInt32 uiCount = 0;
  inout_stream >> uiCount;
  m_Lods.SetCount(plMath::Min(uiCount, plVegetationMaxLods));
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    Lod lod;
    inout_stream >> lod.m_sMesh;
    inout_stream >> lod.m_fScreenSize;
    if (i < plVegetationMaxLods)
      m_Lods[i] = lod;
  }

  inout_stream >> uiCount;
  m_Materials.SetCount(uiCount);
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    inout_stream >> m_Materials[i];
  }

  m_Wind.Load(inout_stream);
  m_Touch.Load(inout_stream);

  // The version 3 fields of the placement keep their defaults for older files: every type weighs the same and no type
  // keeps a distance from clearings, which is how placement behaved before they existed.
  m_Placement = plVegetationPlacementSettings();
  m_Placement.Load(inout_stream);
  m_Render.Load(inout_stream);

  // Version 1 files carry no ray tracing settings: such a type is in the scene by its size.
  m_RayTracing = plVegetationRayTracingSettings();
  if (uiVersion >= 2)
    m_RayTracing.Load(inout_stream);

  if (uiVersion >= 3)
    m_Placement.LoadExtra(inout_stream);

  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////

plVegetationTypeResource::plVegetationTypeResource()
  : plResource(DoUpdate::OnAnyThread, 1)
{
}

PL_RESOURCE_IMPLEMENT_CREATEABLE(plVegetationTypeResource, plVegetationTypeResourceDescriptor)
{
  m_Descriptor = descriptor;
  ResolveHandles();

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Loaded;

  return res;
}

plResourceLoadDesc plVegetationTypeResource::UnloadData(Unload WhatToUnload)
{
  PL_IGNORE_UNUSED(WhatToUnload);

  m_Descriptor = plVegetationTypeResourceDescriptor();
  m_LodMeshes.Clear();
  m_MaterialOverrides.Clear();
  m_hColorGradient.Invalidate();
  m_fBoundingRadius = 0.0f;
  m_iFoliageComposite = -1;

  plResourceLoadDesc res;
  res.m_uiQualityLevelsDiscardable = 0;
  res.m_uiQualityLevelsLoadable = 0;
  res.m_State = plResourceState::Unloaded;

  return res;
}

plResourceLoadDesc plVegetationTypeResource::UpdateContent(plStreamReader* Stream)
{
  PL_LOG_BLOCK("plVegetationTypeResource::UpdateContent", GetResourceIdOrDescription());

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

void plVegetationTypeResource::UpdateMemoryUsage(MemoryUsage& out_NewMemoryUsage)
{
  out_NewMemoryUsage.m_uiMemoryGPU = 0;
  out_NewMemoryUsage.m_uiMemoryCPU = sizeof(plVegetationTypeResource);
}

void plVegetationTypeResource::ResolveHandles()
{
  m_LodMeshes.Clear();
  for (const auto& lod : m_Descriptor.m_Lods)
  {
    if (lod.m_sMesh.IsEmpty())
      continue;

    m_LodMeshes.PushBack(plResourceManager::LoadResource<plMeshResource>(lod.m_sMesh));
  }

  m_MaterialOverrides.Clear();
  for (const plString& sMaterial : m_Descriptor.m_Materials)
  {
    m_MaterialOverrides.PushBack(sMaterial.IsEmpty() ? plMaterialResourceHandle() : plResourceManager::LoadResource<plMaterialResource>(sMaterial));
  }

  m_hColorGradient.Invalidate();
  if (!m_Descriptor.m_Placement.m_sColorGradient.IsEmpty())
  {
    m_hColorGradient = plResourceManager::LoadResource<plColorGradientResource>(m_Descriptor.m_Placement.m_sColorGradient);
  }

  m_fBoundingRadius = 0.0f;
  m_iFoliageComposite = -1;
}

plMaterialResourceHandle plVegetationTypeResource::GetMaterialOverride(plUInt32 uiSlot) const
{
  if (uiSlot < m_MaterialOverrides.GetCount())
    return m_MaterialOverrides[uiSlot];

  return plMaterialResourceHandle();
}

float plVegetationTypeResource::GetBoundingRadius() const
{
  if (m_fBoundingRadius > 0.0f)
    return m_fBoundingRadius;

  if (m_LodMeshes.IsEmpty())
    return 1.0f;

  plResourceLock<plMeshResource> pMesh(m_LodMeshes[0], plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return 1.0f;

  const plBoundingBoxSphere& bounds = pMesh->GetBounds();
  m_fBoundingRadius = bounds.IsValid() ? plMath::Max(bounds.m_fSphereRadius + bounds.m_vCenter.GetLength(), 0.01f) : 1.0f;
  m_fBoundingHeight = bounds.IsValid() ? plMath::Max(bounds.m_vCenter.z + bounds.m_vBoxHalfExtends.z, 0.0f) : 1.0f;
  return m_fBoundingRadius;
}

float plVegetationTypeResource::GetBoundingHeight() const
{
  GetBoundingRadius();
  return m_fBoundingHeight;
}

plUInt32 plVegetationTypeResource::SelectLod(float fScreenSize) const
{
  const plUInt32 uiCount = m_LodMeshes.GetCount();
  if (uiCount <= 1)
    return 0;

  // thresholds are authored descending; the first level whose threshold the size still reaches wins
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    if (fScreenSize >= m_Descriptor.m_Lods[i].m_fScreenSize)
      return i;
  }

  return uiCount - 1;
}

bool plVegetationTypeResource::IsInRayTracingScene(bool& out_bKnown) const
{
  out_bKnown = true;

  switch (m_Descriptor.m_RayTracing.m_Mode)
  {
    case plVegetationRayTracingMode::Included:
      return true;
    case plVegetationRayTracingMode::Excluded:
      return false;
    default:
      break;
  }

  // GetBoundingRadius caches the bounds only once the first LOD mesh has loaded.
  GetBoundingRadius();
  out_bKnown = m_fBoundingRadius > 0.0f;
  return out_bKnown && m_fBoundingHeight >= c_fAutomaticRayTracingHeight;
}

plUInt32 plVegetationTypeResource::GetRayTracingLod() const
{
  const plUInt32 uiCount = m_LodMeshes.GetCount();
  if (uiCount == 0)
    return 0;

  if (const plInt32 iOverride = cvar_VegetationRayTracingLodOverride; iOverride >= 0)
    return plMath::Min(static_cast<plUInt32>(iOverride), uiCount - 1);

  const plInt8 iLod = m_Descriptor.m_RayTracing.m_iLod;
  return plMath::Min(static_cast<plUInt32>(plMath::Max<plInt8>(iLod, 0)), uiCount - 1);
}

float plVegetationTypeResource::GetRayTracingRange() const
{
  const float fRange = m_Descriptor.m_RayTracing.m_fRange;
  return fRange > 0.0f ? fRange : m_Descriptor.m_Render.m_fCullDistance;
}

bool plVegetationTypeResource::IsFoliageComposite() const
{
  if (m_iFoliageComposite >= 0)
    return m_iFoliageComposite > 0;

  if (m_LodMeshes.IsEmpty())
    return false;

  plResourceLock<plMeshResource> pMesh(m_LodMeshes[0], plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return false;

  m_iFoliageComposite = pMesh->GetFoliageComposite() != nullptr ? 1 : 0;
  return m_iFoliageComposite > 0;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Resources_VegetationTypeResource);
