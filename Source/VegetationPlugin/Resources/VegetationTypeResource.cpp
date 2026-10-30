#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeResource, 1, plRTTIDefaultAllocator<plVegetationTypeResource>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

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

void plVegetationTypeResourceDescriptor::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
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
}

plResult plVegetationTypeResourceDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion != 1)
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
  m_Placement.Load(inout_stream);
  m_Render.Load(inout_stream);

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
  return m_fBoundingRadius;
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

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Resources_VegetationTypeResource);
