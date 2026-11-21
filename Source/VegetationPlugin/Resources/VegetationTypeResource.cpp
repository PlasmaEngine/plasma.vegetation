#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <RendererFoundation/Device/Device.h>
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
  const plUInt8 uiVersion = 4;
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

  // version 4
  m_Impostor.Save(inout_stream);
  const bool bHasImpostor = m_pImpostorData != nullptr && m_pImpostorData->IsValid();
  inout_stream << bHasImpostor;
  if (bHasImpostor)
    m_pImpostorData->Save(inout_stream);

  const bool bHasProxy = m_pVoxelProxy != nullptr && m_pVoxelProxy->IsValid();
  inout_stream << bHasProxy;
  if (bHasProxy)
    m_pVoxelProxy->Save(inout_stream);
}

plResult plVegetationTypeResourceDescriptor::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;

  if (uiVersion < 1 || uiVersion > 4)
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

  // Older files have no impostor: their trees keep the merged mesh at every distance until the type is transformed again.
  m_Impostor = plVegetationImpostorSettings();
  m_pImpostorData.Clear();
  m_pVoxelProxy.Clear();
  if (uiVersion >= 4)
  {
    m_Impostor.Load(inout_stream);
    bool bHasImpostor = false;
    inout_stream >> bHasImpostor;
    if (bHasImpostor)
    {
      plSharedPtr<plVegetationImpostorData> pData = PL_DEFAULT_NEW(plVegetationImpostorData);
      if (pData->Load(inout_stream).Failed())
      {
        plLog::Error("Vegetation type has an unreadable impostor; it draws without one.");
        return PL_SUCCESS;
      }
      m_pImpostorData = pData;
    }

    bool bHasProxy = false;
    inout_stream >> bHasProxy;
    if (bHasProxy)
    {
      plSharedPtr<plVegetationVoxelProxy> pProxy = PL_DEFAULT_NEW(plVegetationVoxelProxy);
      if (pProxy->Load(inout_stream).Failed())
      {
        plLog::Error("Vegetation type has an unreadable voxel proxy; its cells draw it tree by tree.");
        return PL_SUCCESS;
      }
      m_pVoxelProxy = pProxy;
    }
  }

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
  ResetCompositeInfo();

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
  ResetCompositeInfo();
}

void plVegetationTypeResource::ResetCompositeInfo()
{
  {
    PL_LOCK(m_CompositeMutex);
    m_iCompositeState = -1;
    m_pComposite.Clear();
  }

  PL_LOCK(m_ImpostorMutex);
  m_iImpostorState = -1;
  m_pImpostor.Clear();
}

const plVegetationCompositeInfo* plVegetationTypeResource::GetCompositeInfo(bool& out_bKnown) const
{
  out_bKnown = true;

  const plInt32 iState = m_iCompositeState;
  if (iState == 0)
    return nullptr;
  if (iState == 1)
    return m_pComposite.Borrow();

  PL_LOCK(m_CompositeMutex);
  if (m_iCompositeState >= 0)
    return m_iCompositeState == 1 ? m_pComposite.Borrow() : nullptr;

  if (m_LodMeshes.IsEmpty())
  {
    m_iCompositeState = 0;
    return nullptr;
  }

  plResourceLock<plMeshResource> pMesh(m_LodMeshes[0], plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
  {
    out_bKnown = false;
    return nullptr;
  }

  const plFoliageComposite* pComposite = pMesh->GetFoliageComposite();
  if (pComposite == nullptr || pComposite->IsEmpty() || pComposite->m_Parts.IsEmpty())
  {
    m_iCompositeState = 0;
    return nullptr;
  }

  m_pComposite = PL_DEFAULT_NEW(plVegetationCompositeInfo);
  m_pComposite->m_fSwitchError = pComposite->m_fSwitchError;
  m_pComposite->m_Instances = pComposite->m_Instances;
  for (const plString& sPart : pComposite->m_Parts)
    m_pComposite->m_PartMeshes.PushBack(sPart.IsEmpty() ? plMeshResourceHandle() : plResourceManager::LoadResource<plMeshResource>(sPart));

  const plFoliageSkeleton* pSkeleton = pMesh->GetFoliageSkeleton();
  m_pComposite->m_bHasSkeleton = pSkeleton != nullptr && !pSkeleton->IsEmpty();
  m_pComposite->ComputeSway(pSkeleton);

  // Published last: readers that see 1 find the info complete.
  m_iCompositeState = 1;
  return m_pComposite.Borrow();
}

namespace
{
  /// The impostor shader of this package (Assets/Shaders/Vegetation/VegetationImpostor.plShader).
  constexpr const char* s_szImpostorShader = "Shaders/Vegetation/VegetationImpostor.plShader";
  /// The merged voxel shell shader (Assets/Shaders/Vegetation/VegetationCellShell.plShader).
  constexpr const char* s_szCellShader = "Shaders/Vegetation/VegetationCellShell.plShader";

  plAtomicInteger32 s_iImpostorResources = 0;

  plTexture2DResourceHandle CreateImpostorTexture(plStringView sTypeId, plStringView sName, const plImage& image, bool bSRGB)
  {
    plTexture2DResourceDescriptor td;
    td.m_SamplerDesc.m_AddressU = plImageAddressMode::Clamp;
    td.m_SamplerDesc.m_AddressV = plImageAddressMode::Clamp;
    td.m_SamplerDesc.m_AddressW = plImageAddressMode::Clamp;
    td.m_SamplerDesc.m_MinFilter = plGALTextureFilterMode::Linear;
    td.m_SamplerDesc.m_MagFilter = plGALTextureFilterMode::Linear;
    td.m_SamplerDesc.m_MipFilter = plGALTextureFilterMode::Linear;

    plUInt32 uiMemory = 0;
    plHybridArray<plGALSystemMemoryDescription, 32> initData;
    plTexture2DResource::FillOutDescriptor(td, &image, bSRGB, image.GetNumMipLevels(), uiMemory, initData);
    td.m_InitialContent = initData;

    // Unique per creation: a reloaded type must not pick up the textures of its previous bake.
    plStringBuilder sId;
    sId.SetFormat("{}/Impostor{}/{}", sTypeId, s_iImpostorResources.Increment(), sName);
    return plResourceManager::CreateResource<plTexture2DResource>(sId, std::move(td), sId);
  }

  plMeshResourceHandle CreateImpostorMesh(plStringView sTypeId, const plVec3& vCenter, float fRadius)
  {
    // One quad (texture coordinates are the corners the vertex shader spreads toward the view) plus the corners of the
    // sphere's box, which no triangle uses: they give the mesh bounds that hold the tree from every side, for culling.
    plGeometry geom;
    plUInt32 quad[4];
    quad[0] = geom.AddVertex(vCenter, plVec3(0, 0, 1), plVec2(0, 0));
    quad[1] = geom.AddVertex(vCenter, plVec3(0, 0, 1), plVec2(1, 0));
    quad[2] = geom.AddVertex(vCenter, plVec3(0, 0, 1), plVec2(1, 1));
    quad[3] = geom.AddVertex(vCenter, plVec3(0, 0, 1), plVec2(0, 1));
    geom.AddPolygon(plArrayPtr<plUInt32>(quad), false);
    for (plUInt32 c = 0; c < 8; ++c)
    {
      const plVec3 vCorner((c & 1) ? fRadius : -fRadius, (c & 2) ? fRadius : -fRadius, (c & 4) ? fRadius : -fRadius);
      geom.AddVertex(vCenter + vCorner, plVec3(0, 0, 1), plVec2(0.5f, 0.5f));
    }

    plMeshResourceDescriptor desc;
    desc.MeshBufferDesc().AddCommonStreams();
    desc.MeshBufferDesc().AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);
    desc.AddSubMesh(desc.MeshBufferDesc().GetPrimitiveCount(), 0, 0);
    desc.ComputeBounds();

    plStringBuilder sId;
    sId.SetFormat("{}/Impostor{}/Quad", sTypeId, s_iImpostorResources.Increment());
    return plResourceManager::CreateResource<plMeshResource>(sId, std::move(desc), sId);
  }
} // namespace

const plVegetationImpostorGpu* plVegetationTypeResource::GetImpostor(bool& out_bKnown) const
{
  out_bKnown = true;

  const plInt32 iState = m_iImpostorState;
  if (iState == 0)
    return nullptr;
  if (iState == 1)
    return m_pImpostor.Borrow();

  PL_LOCK(m_ImpostorMutex);
  if (m_iImpostorState >= 0)
    return m_iImpostorState == 1 ? m_pImpostor.Borrow() : nullptr;

  const plVegetationImpostorData* pData = m_Descriptor.m_pImpostorData.Borrow();
  if (pData == nullptr || !pData->IsValid() || m_Descriptor.m_Impostor.m_fDistance <= 0.0f || m_LodMeshes.IsEmpty() || plGALDevice::GetDefaultDevice() == nullptr)
  {
    m_iImpostorState = 0;
    return nullptr;
  }

  // The impostor shades with the material of its baked slot: the type's override, else the merged mesh's material.
  plMaterialResourceHandle hBaseMaterial = GetMaterialOverride(pData->m_uiMaterialSlot);
  if (!hBaseMaterial.IsValid())
  {
    plResourceLock<plMeshResource> pMesh(m_LodMeshes[0], plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    {
      out_bKnown = false;
      return nullptr;
    }

    if (pData->m_uiMaterialSlot < pMesh->GetMaterials().GetCount())
      hBaseMaterial = pMesh->GetMaterials()[pData->m_uiMaterialSlot];
  }

  if (hBaseMaterial.IsValid())
  {
    // Material creation flattens the base chain and blocks on it: only start once it is there.
    plResourceLock<plMaterialResource> pBase(hBaseMaterial, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pBase.GetAcquireResult() == plResourceAcquireResult::LoadingFallback)
    {
      out_bKnown = false;
      return nullptr;
    }
    if (pBase.GetAcquireResult() != plResourceAcquireResult::Final)
      hBaseMaterial.Invalidate();
  }

  plUniquePtr<plVegetationImpostorGpu> pGpu = PL_DEFAULT_NEW(plVegetationImpostorGpu);
  pGpu->m_fRadius = pData->m_fRadius;
  pGpu->m_vCenter = pData->m_vCenter;

  const plStringView sTypeId = GetResourceID();
  pGpu->m_hAlbedo = CreateImpostorTexture(sTypeId, "Albedo", pData->m_Albedo, true);
  pGpu->m_hNormalDepth = CreateImpostorTexture(sTypeId, "NormalDepth", pData->m_NormalDepth, false);
  pGpu->m_hMesh = CreateImpostorMesh(sTypeId, pData->m_vCenter, pData->m_fRadius);

  plMaterialResourceDescriptor md;
  md.m_hBaseMaterial = hBaseMaterial;
  md.m_hShader = plResourceManager::LoadResource<plShaderResource>(s_szImpostorShader);
  md.m_RenderDataCategory = plDefaultRenderDataCategories::LitMasked;

  auto AddPermutation = [&](const char* szName, const char* szValue)
  {
    plPermutationVar& var = md.m_PermutationVars.ExpandAndGetRef();
    var.m_sName.Assign(szName);
    var.m_sValue.Assign(szValue);
  };
  AddPermutation("BLEND_MODE", "BLEND_MODE_MASKED");
  AddPermutation("SHADING_MODE", "SHADING_MODE_LIT");
  // Whichever way the quad's winding faces the view, it is drawn.
  AddPermutation("TWO_SIDED", "TRUE");

  auto AddParameter = [&](const char* szName, const plVariant& value)
  {
    plMaterialResourceDescriptor::Parameter& param = md.m_Parameters.ExpandAndGetRef();
    param.m_Name.Assign(szName);
    param.m_Value = value;
  };
  const plUInt32 uiMips = plMath::Min(pData->m_Albedo.GetNumMipLevels(), pData->m_NormalDepth.GetNumMipLevels());
  AddParameter("ImpostorSphere", plVec4(pData->m_vCenter.x, pData->m_vCenter.y, pData->m_vCenter.z, pData->m_fRadius));
  AddParameter("ImpostorLayout", plVec4(static_cast<float>(pData->m_uiViews), static_cast<float>(pData->m_uiResolution), static_cast<float>(uiMips), static_cast<float>(pData->m_uiViews * pData->m_uiResolution)));

  auto AddTexture = [&](const char* szName, const plTexture2DResourceHandle& hTexture)
  {
    plMaterialResourceDescriptor::Texture2DBinding& binding = md.m_Texture2DBindings.ExpandAndGetRef();
    binding.m_Name.Assign(szName);
    binding.m_Value = hTexture;
  };
  AddTexture("ImpostorAlbedo", pGpu->m_hAlbedo);
  AddTexture("ImpostorNormalDepth", pGpu->m_hNormalDepth);

  // The merged voxel shell shades with the same base material: its vertices carry colour, coverage and translucency.
  plMaterialResourceDescriptor cellDesc;
  cellDesc.m_hBaseMaterial = hBaseMaterial;
  cellDesc.m_hShader = plResourceManager::LoadResource<plShaderResource>(s_szCellShader);
  cellDesc.m_RenderDataCategory = plDefaultRenderDataCategories::LitMasked;
  cellDesc.m_PermutationVars = md.m_PermutationVars;

  plStringBuilder sMaterialId;
  sMaterialId.SetFormat("{}/Impostor{}/Material", sTypeId, s_iImpostorResources.Increment());
  pGpu->m_hMaterial = plResourceManager::CreateResource<plMaterialResource>(sMaterialId, std::move(md), sMaterialId);

  sMaterialId.SetFormat("{}/Impostor{}/CellMaterial", sTypeId, s_iImpostorResources.Increment());
  pGpu->m_hCellMaterial = plResourceManager::CreateResource<plMaterialResource>(sMaterialId, std::move(cellDesc), sMaterialId);

  m_pImpostor = std::move(pGpu);
  // Published last: readers that see 1 find the impostor complete.
  m_iImpostorState = 1;
  return m_pImpostor.Borrow();
}

float plVegetationCompositeInfo::ComputeSwitchDistance(float fSwitchError, float fScale, float fViewportHeight, float fTanHalfFovY, float fErrorPixels)
{
  // The merged mesh's finest error, at this distance, projects to fErrorPixels on a viewport fViewportHeight tall: the
  // same switch plFoliageCompositeComponent makes.
  if (fSwitchError <= 0.0f || fViewportHeight <= 0.0f || fTanHalfFovY <= 0.0f)
    return 0.0f;

  return fSwitchError * fScale * fViewportHeight / (2.0f * fTanHalfFovY * plMath::Max(fErrorPixels, 0.01f));
}

void plVegetationCompositeInfo::ComputeSway(const plFoliageSkeleton* pSkeleton)
{
  m_InstanceSway.SetCount(m_Instances.GetCount());
  m_InstanceSwayPerMetre.SetCount(m_Instances.GetCount());

  if (pSkeleton == nullptr || pSkeleton->IsEmpty())
  {
    for (plUInt32 i = 0; i < m_Instances.GetCount(); ++i)
    {
      m_InstanceSway[i] = 0.0f;
      m_InstanceSwayPerMetre[i] = 0.0f;
    }
    return;
  }

  const plUInt32 uiBoneCount = pSkeleton->m_Bones.GetCount();

  // Per bone: the sum of the chords of the maximum bends up its chain. ComputeDisplacementBound adds chord * path length
  // per bone; a point r further from the origin lengthens every path by at most r.
  auto Chord = [](float fAngle) { return 2.0f * plMath::Sin(plAngle::MakeFromRadian(plMath::Min(fAngle, plMath::Pi<float>()) * 0.5f)); };
  plDynamicArray<float> chainChords;
  chainChords.SetCount(uiBoneCount);
  float fMaxChainChord = 0.0f;
  for (plUInt32 b = 0; b < uiBoneCount; ++b)
  {
    // Parents come before their children (plFoliageSkeleton::Finalize).
    const plUInt16 uiParent = pSkeleton->m_Bones[b].m_uiParent;
    const float fParent = (uiParent != plFoliageBone::InvalidParent && uiParent < b) ? chainChords[uiParent] : 0.0f;
    chainChords[b] = fParent + Chord(pSkeleton->m_Bones[b].m_fMaxBend);
    fMaxChainChord = plMath::Max(fMaxChainChord, chainChords[b]);
  }

  // The path form of plFoliageSkeleton::ComputeDisplacementBound: each bone up the chain moves the point by at most the
  // chord of its maximum bend times the longest path from its pivot to the point. Unlike the tighter of the two bounds that
  // function returns, it grows by exactly r times the chain's chords when the point is r further away.
  auto PathBound = [&](const plVec3& vPoint, plUInt32 uiBone)
  {
    float fBound = 0.0f;
    float fPath = (vPoint - pSkeleton->m_Bones[uiBone].m_vPivot).GetLength();
    plUInt32 b = uiBone;
    for (plUInt32 uiGuard = 0; uiGuard < uiBoneCount; ++uiGuard)
    {
      fBound += Chord(pSkeleton->m_Bones[b].m_fMaxBend) * fPath;
      const plUInt16 uiParent = pSkeleton->m_Bones[b].m_uiParent;
      if (uiParent == plFoliageBone::InvalidParent || uiParent >= uiBoneCount)
        break;
      fPath += (pSkeleton->m_Bones[b].m_vPivot - pSkeleton->m_Bones[uiParent].m_vPivot).GetLength();
      b = uiParent;
    }
    return fBound;
  };

  for (plUInt32 i = 0; i < m_Instances.GetCount(); ++i)
  {
    const plFoliageCompositeInstance& inst = m_Instances[i];
    const plVec3 vOrigin = inst.m_Transform.m_vPosition;

    float fSway = 0.0f;
    float fPerMetre = 0.0f;
    if (inst.GetDominantBone() == plFoliageCompositeInstance::InvalidBone)
    {
      // Every vertex follows its own vertex bone: any bone of the skeleton.
      for (plUInt32 b = 0; b < uiBoneCount; ++b)
        fSway = plMath::Max(fSway, PathBound(vOrigin, b));
      fPerMetre = fMaxChainChord;
    }
    else
    {
      // A rigid blend of its bones never moves further than the furthest of them.
      for (plUInt32 k = 0; k < plFoliageCompositeInstance::MaxBones; ++k)
      {
        const plUInt16 uiBone = inst.m_Bones[k];
        if (uiBone == plFoliageCompositeInstance::InvalidBone || uiBone >= uiBoneCount)
          continue;
        fSway = plMath::Max(fSway, PathBound(vOrigin, uiBone));
        fPerMetre = plMath::Max(fPerMetre, chainChords[uiBone]);
      }
    }

    m_InstanceSway[i] = fSway;
    m_InstanceSwayPerMetre[i] = fPerMetre;
  }
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

  // A composite traces its merged mesh; the other LODs are not drawn.
  if (IsFoliageComposite())
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

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Resources_VegetationTypeResource);
