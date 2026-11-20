#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Camera.h>
#include <Foundation/Configuration/CVar.h>
#include <RendererCore/Lights/Implementation/VirtualShadowPool.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Scene/SceneRegistry.h>
#include <RendererCore/Wind/FoliageBonePool.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/DynamicBuffer.h>
#include <VegetationPlugin/Components/VegetationCellComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

#include <Shaders/Vegetation/FoliageBonesData.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plVegetationCellComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgExtractRenderData, OnMsgExtractRenderData),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plHiddenAttribute(),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plCVarBool cvar_VegetationEnabled("Vegetation.Enabled", true, plCVarFlags::Save, "Renders vegetation instances");
plCVarFloat cvar_VegetationDrawDistanceScale("Vegetation.DrawDistanceScale", 1.0f, plCVarFlags::Save, "Scales every vegetation type's cull and shadow distance");
plCVarFloat cvar_VegetationCompositeMaxPartsDistance("Vegetation.Composite.MaxPartsDistance", 60.0f, plCVarFlags::Default,
  "Foliage composite trees in vegetation layers show their parts at most this close (metres, at instance scale 1), whatever their switch error asks; the merged mesh beyond. 0 = no cap");
plCVarFloat cvar_VegetationCompositeShadowPartsDistance("Vegetation.Composite.ShadowPartsDistance", 20.0f, plCVarFlags::Default,
  "Foliage composite trees in vegetation layers cast their shadow with their parts only this close (metres, at instance scale 1); beyond, the merged mesh casts it");
plCVarFloat cvar_VegetationDensityScale("Vegetation.DensityScale", 1.0f, plCVarFlags::Save, "Fraction of instances kept by distance thinning, 0..1");
plCVarFloat cvar_VegetationRayTracingRange("Vegetation.RayTracing.Range", 128.0f, plCVarFlags::Default, "Largest distance in metres up to which vegetation is part of the ray tracing scene, whatever a type asks for. 0 keeps vegetation out of it.");

extern plCVarInt cvar_VegetationRayTracingLodOverride;
plCVarBool cvar_VegetationRayTracingLogTypes("Vegetation.RayTracing.LogTypes", false, plCVarFlags::Default, "Log once per vegetation type whether it is part of the ray tracing scene, with its size, traced LOD and range.");

namespace
{
  /// Frames a replaced ray tracing snapshot's instance data stays allocated: a frame in flight may still read it.
  constexpr plUInt64 c_uiRTRetireFrames = 8;

  /// Vegetation.RayTracing.LogTypes: one line per type and decision.
  void LogRayTracingDecision(const plVegetationTypeResourceHandle& hType, const plVegetationTypeResource& type, bool bIncluded)
  {
    static plMutex s_Mutex;
    static plSet<plUInt64> s_Logged;

    {
      PL_LOCK(s_Mutex);
      const plUInt64 uiKey = plHashingUtils::xxHash64(&bIncluded, sizeof(bIncluded), hType.GetResourceIDHash());
      if (s_Logged.Contains(uiKey))
        return;
      s_Logged.Insert(uiKey);
    }

    const char* szMode = "Automatic";
    if (type.GetDescriptor().m_RayTracing.m_Mode == plVegetationRayTracingMode::Included)
      szMode = "On";
    else if (type.GetDescriptor().m_RayTracing.m_Mode == plVegetationRayTracingMode::Excluded)
      szMode = "Off";

    if (bIncluded)
    {
      plLog::Info("Vegetation type '{}' is in the ray tracing scene (Affects GI: {}, radius {} m, height {} m, LOD {} of {}, range {} m).", type.GetResourceIdOrDescription(), szMode, plArgF(type.GetBoundingRadius(), 2), plArgF(type.GetBoundingHeight(), 2), type.GetRayTracingLod(), type.GetLodCount(), plArgF(type.GetRayTracingRange(), 0));
    }
    else
    {
      plLog::Info("Vegetation type '{}' is not in the ray tracing scene (Affects GI: {}, radius {} m, height {} m).", type.GetResourceIdOrDescription(), szMode, plArgF(type.GetBoundingRadius(), 2), plArgF(type.GetBoundingHeight(), 2));
    }
  }

  /// Vegetation.Composite.SwitchErrorPixels, owned by plFoliageCompositeComponent, so cells switch where it does.
  float GetCompositeSwitchErrorPixels()
  {
    static plCVar* s_pCVar = plCVar::FindCVarByName("Vegetation.Composite.SwitchErrorPixels");
    if (s_pCVar != nullptr && s_pCVar->GetType() == plCVarType::Float)
      return static_cast<plCVarFloat*>(s_pCVar)->GetValue();
    return 1.0f;
  }

  /// The instance colour of every colour seed of a type; white without a gradient.
  void ResolveSeedColors(const plVegetationTypeResource& type, plColor out_colors[256])
  {
    for (plUInt32 s = 0; s < 256; ++s)
      out_colors[s] = plColor::White;

    // Locking an invalid handle asserts.
    if (const plColorGradientResourceHandle hGradient = type.GetColorGradient(); hGradient.IsValid())
    {
      plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
      if (pGradient.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        for (plUInt32 s = 0; s < 256; ++s)
          out_colors[s] = pGradient->Evaluate(s / 255.0);
      }
    }
  }
} // namespace

plVegetationCellComponent::plVegetationCellComponent() = default;
plVegetationCellComponent::~plVegetationCellComponent() = default;

void plVegetationCellComponent::OnDeactivated()
{
  // the cell's shadows must not outlive it in the cached pages
  if (m_LastCasterBox.IsValid())
  {
    plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), m_LastCasterBox);
    m_LastCasterBox = plBoundingBox::MakeInvalid();
  }

  ReleaseBatches(GetWorld()->GetModuleReadOnly<plRenderDataManager>());

  {
    PL_LOCK(m_RTMutex);
    ReleaseRTGroups(GetWorld()->GetModuleReadOnly<plRenderDataManager>(), true);
  }

  SUPER::OnDeactivated();
}

void plVegetationCellComponent::SetCell(plVegetationCell* pCell)
{
  m_pCell = pCell;
  m_uiSyncedVersion = 0xFFFFFFFF;

  if (IsActiveAndInitialized())
  {
    TriggerLocalBoundsUpdate();
  }
}

void plVegetationCellComponent::OnCellChanged()
{
  if (IsActiveAndInitialized())
  {
    TriggerLocalBoundsUpdate();
    InvalidateCachedShadows();
  }
}

plBoundingBox plVegetationCellComponent::ComputeWorldBox() const
{
  plBoundingBox box = plBoundingBox::MakeInvalid();
  if (m_pCell == nullptr)
    return box;

  for (const auto& group : m_pCell->m_Groups)
  {
    float fRadius = 1.0f;
    {
      plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
      if (pType.GetAcquireResult() == plResourceAcquireResult::Final)
        fRadius = pType->GetBoundingRadius();
    }

    for (const auto& inst : group.m_Instances)
    {
      const float r = fRadius * inst.m_fScale;
      box.ExpandToInclude(inst.m_vPosition - plVec3(r));
      box.ExpandToInclude(inst.m_vPosition + plVec3(r));
    }
  }

  return box;
}

void plVegetationCellComponent::InvalidateCachedShadows()
{
  // Continuous VSM updates handle wind and touch. Explicit content invalidation also covers topology changes
  // from painting and erasing, including any depth cached before the cell opted into continuous shadows.
  const plBoundingBox newBox = ComputeWorldBox();

  plBoundingBox invalidate = newBox;
  if (m_LastCasterBox.IsValid())
  {
    if (invalidate.IsValid())
      invalidate.ExpandToInclude(m_LastCasterBox);
    else
      invalidate = m_LastCasterBox;
  }

  if (invalidate.IsValid())
  {
    plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), invalidate);
  }

  m_LastCasterBox = newBox;
}

void plVegetationCellComponent::ReleaseBatches(const plRenderDataManager* pRenderDataManager) const
{
  // Called from extraction (read-only world) and from deactivation; the manager comes from the caller so no
  // writable world lookup happens here.
  for (GroupState& state : m_GroupStates)
  {
    for (plUInt32 lod = 0; lod < plVegetationMaxLods; ++lod)
    {
      for (LodBatch* pBatch : {&state.m_Main.m_Lods[lod], &state.m_Shadow.m_Lods[lod]})
      {
        if (pRenderDataManager != nullptr && !pBatch->m_Offset.IsInvalidated())
        {
          pRenderDataManager->DeleteInstanceData(pBatch->m_Offset);
        }

        *pBatch = LodBatch();
      }
    }

    ReleaseCompositeSet(pRenderDataManager, state.m_Composite.m_Main);
    ReleaseCompositeSet(pRenderDataManager, state.m_Composite.m_Shadow);
  }

  m_GroupStates.Clear();
  m_uiSyncedVersion = 0xFFFFFFFF;
}

void plVegetationCellComponent::ReleaseCompositeBatch(const plRenderDataManager* pRenderDataManager, CompositeBatch& ref_batch) const
{
  if (pRenderDataManager != nullptr)
  {
    if (!ref_batch.m_Offset.IsInvalidated())
      pRenderDataManager->DeleteInstanceData(ref_batch.m_Offset);

    if (!ref_batch.m_BonesOffset.IsInvalidated() && m_uiFoliageBonesDataIndex != 0xFFFFFFFF)
      pRenderDataManager->DeleteCustomInstanceData(m_uiFoliageBonesDataIndex, ref_batch.m_BonesOffset);
  }

  ref_batch = CompositeBatch();
}

void plVegetationCellComponent::ReleaseCompositeSet(const plRenderDataManager* pRenderDataManager, CompositeSet& ref_set) const
{
  for (CompositeBatch& batch : ref_set.m_Parts)
    ReleaseCompositeBatch(pRenderDataManager, batch);
  ReleaseCompositeBatch(pRenderDataManager, ref_set.m_Merged);

  ref_set = CompositeSet();
}

plResult plVegetationCellComponent::GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg)
{
  PL_IGNORE_UNUSED(ref_msg);
  ref_bAlwaysVisible = false;

  if (m_pCell == nullptr || m_pCell->m_Groups.IsEmpty())
    return PL_FAILURE;

  plBoundingBox box = ComputeWorldBox();
  if (!box.IsValid())
    return PL_FAILURE;

  // the owner sits at the cell origin; bounds are reported in its local space
  box.m_vMin -= m_pCell->m_vOrigin;
  box.m_vMax -= m_pCell->m_vOrigin;

  ref_bounds = plBoundingBoxSphere::MakeFromBox(box);
  return PL_SUCCESS;
}

void plVegetationCellComponent::SyncGroupStates(const plRenderDataManager* pRenderDataManager) const
{
  if (m_pCell == nullptr)
    return;

  if (m_uiSyncedVersion == m_pCell->m_uiVersion && m_GroupStates.GetCount() == m_pCell->m_Groups.GetCount())
    return;

  // Instance counts or groups changed: allocations are re-made lazily with the new capacity.
  ReleaseBatches(pRenderDataManager);
  m_GroupStates.SetCount(m_pCell->m_Groups.GetCount());
  m_uiSyncedVersion = m_pCell->m_uiVersion;
}

void plVegetationCellComponent::OnMsgExtractRenderData(plMsgExtractRenderData& msg) const
{
  if (!cvar_VegetationEnabled || m_pCell == nullptr || msg.m_pView == nullptr)
    return;

  const plCameraUsageHint::Enum usage = msg.m_pView->GetCameraUsageHint();
  const bool bShadow = usage == plCameraUsageHint::Shadow;
  const bool bMain = usage == plCameraUsageHint::MainView || usage == plCameraUsageHint::EditorView;

  if (!bShadow && !bMain && usage != plCameraUsageHint::Thumbnail && usage != plCameraUsageHint::RenderTarget)
    return;

  const plVegetationWorldModule* pModule = GetWorld()->GetModuleReadOnly<plVegetationWorldModule>();
  if (pModule == nullptr)
    return;

  const plCamera* pCamera = msg.m_pView->GetCullingCamera();

  plVec3 vCameraPosition;
  float fTanHalfFov;

  if (bShadow || pCamera->IsOrthographic())
  {
    vCameraPosition = pModule->GetMainCameraPosition();
    fTanHalfFov = pModule->GetMainCameraTanHalfFov();
  }
  else
  {
    vCameraPosition = pCamera->GetCenterPosition();
    // Match the projection used by Vegetation.plShader. A square aspect selects a different LOD on
    // horizontal-FOV cameras, whose leaves are then discarded by the shader's crossfade.
    const auto& viewport = msg.m_pView->GetViewport();
    const float fAspectRatio = viewport.width / plMath::Max(viewport.height, 1.0f);
    fTanHalfFov = plMath::Tan(pCamera->GetFovY(fAspectRatio) * 0.5f);

    if (bMain)
    {
      pModule->ReportMainCamera(vCameraPosition, fTanHalfFov, viewport.height);
    }
  }

  // Main views and multiple shadow union views extract this component concurrently.
  // Keep batch allocation, refill and emitted offsets together so no view observes a
  // partially rebuilt set or races the version synchronization.
  PL_LOCK(m_BatchMutex);
  m_uiFoliageBonesDataIndex = pModule->GetFoliageBonesDataIndex();
  SyncGroupStates(msg.m_pRenderDataManager);

  for (plUInt32 g = 0; g < m_pCell->m_Groups.GetCount(); ++g)
  {
    BuildBatches(m_pCell->m_Groups[g], m_GroupStates[g], bShadow, vCameraPosition, fTanHalfFov, msg);
  }
}

void plVegetationCellComponent::BuildBatches(const plVegetationCellGroup& group, GroupState& state, bool bShadow, const plVec3& vCameraPosition, float fTanHalfFov, plMsgExtractRenderData& msg) const
{
  plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pType.GetAcquireResult() != plResourceAcquireResult::Final || pType->GetLodCount() == 0)
    return;

  // Composites draw their parts and merged mesh; until LOD 0 has loaded the type is drawn as any other.
  bool bCompositeKnown = false;
  if (const plVegetationCompositeInfo* pComposite = pType->GetCompositeInfo(bCompositeKnown))
  {
    if (const plVegetationWorldModule* pModule = GetWorld()->GetModuleReadOnly<plVegetationWorldModule>())
      BuildCompositeBatches(group, state.m_Composite, *pType.GetPointer(), *pComposite, bShadow, *pModule, msg);
    return;
  }

  const auto& render = pType->GetDescriptor().m_Render;
  if (bShadow && !render.m_bCastShadows)
    return;

  const plUInt32 uiLodCount = plMath::Min(pType->GetLodCount(), plVegetationMaxLods);
  SetState& set = bShadow ? state.m_Shadow : state.m_Main;
  LodBatch* pBatches = set.m_Lods;
  const plUInt64 uiCurrentFrame = plRenderWorld::GetFrameCounter();

  const float fDistanceScale = plMath::Max(cvar_VegetationDrawDistanceScale.GetValue(), 0.0f);
  const float fDensity = plMath::Clamp(cvar_VegetationDensityScale.GetValue(), 0.0f, 1.0f);
  float fCullDistance = render.m_fCullDistance * fDistanceScale;
  if (bShadow)
    fCullDistance = plMath::Min(fCullDistance, render.m_fShadowCullDistance * fDistanceScale);

  // The shader evaluates crossfade against this frame's camera. Cached membership must follow that camera
  // too: a percentage of cull distance can leave a nearby tree in a fully discarded LOD for tens of metres.
  // Keep allocations and reuse membership only while the camera/projection and other inputs are unchanged.
  const bool bRefill = set.m_uiFrame == 0 || set.m_fFillDistanceScale != fDistanceScale || set.m_fFillDensity != fDensity ||
                       set.m_fFillTanHalfFov != fTanHalfFov || !vCameraPosition.IsEqual(set.m_vFillCamera, 0.0001f);

  if (bRefill && set.m_uiFrame != uiCurrentFrame)
  {
    set.m_uiFrame = uiCurrentFrame;
    set.m_vFillCamera = vCameraPosition;
    set.m_fFillTanHalfFov = fTanHalfFov;
    set.m_fFillDistanceScale = fDistanceScale;
    set.m_fFillDensity = fDensity;

    const float fThinStart = fCullDistance * plMath::Clamp(render.m_fThinningStart, 0.0f, 1.0f);
    const float fRadius = pType->GetBoundingRadius();
    const float fCrossfade = plMath::Clamp(render.m_fLodCrossfade, 0.0f, 0.5f);

    // Each entry: instance index plus the LOD it goes to. Instances inside a crossfade band are listed twice; the
    // shader dithers the two levels against each other from the thresholds packed into CustomData.
    plHybridArray<plUInt32, 256> visible[plVegetationMaxLods];

    for (plUInt32 i = 0; i < group.m_Instances.GetCount(); ++i)
    {
      const plVegetationCellInstance& inst = group.m_Instances[i];

      if (bShadow && inst.m_Flags.IsSet(plVegetationInstanceFlags::NoShadow))
        continue;

      const float fDistance = (inst.m_vPosition - vCameraPosition).GetLength();
      if (fDistance > fCullDistance)
        continue;

      if (!inst.m_Flags.IsSet(plVegetationInstanceFlags::Hero))
      {
        float fKeep = fDensity;
        if (fDistance > fThinStart && fCullDistance > fThinStart)
        {
          fKeep *= 1.0f - (fDistance - fThinStart) / (fCullDistance - fThinStart);
        }

        // The colour seed doubles as a stable per-instance random value.
        if ((inst.m_uiColorSeed + 0.5f) / 256.0f > fKeep)
          continue;
      }

      const float fScreenSize = (fRadius * inst.m_fScale) / (plMath::Max(fDistance, 0.01f) * fTanHalfFov);
      plUInt32 uiLod = pType->SelectLod(fScreenSize);
      if (bShadow && uiLod + 1 < uiLodCount)
        ++uiLod;
      uiLod = plMath::Min(uiLod, uiLodCount - 1);

      visible[uiLod].PushBack(i);

      // Crossfade bands around the thresholds this instance is next to (main views only; shadows pop).
      if (!bShadow && fCrossfade > 0.0f)
      {
        if (uiLod > 0)
        {
          const float fUpper = pType->GetLodScreenSize(uiLod - 1);
          if (fScreenSize >= fUpper * (1.0f - fCrossfade))
            visible[uiLod - 1].PushBack(i);
        }
        if (uiLod + 1 < uiLodCount)
        {
          const float fLower = pType->GetLodScreenSize(uiLod);
          if (fScreenSize <= fLower * (1.0f + fCrossfade))
            visible[uiLod + 1].PushBack(i);
        }
      }
    }

    // Colour per seed, resolved once per refill.
    plColor seedColors[256];
    ResolveSeedColors(*pType.GetPointer(), seedColors);

    const plUInt32 uiOwnerId = GetUniqueIdForRendering();

    for (plUInt32 lod = 0; lod < uiLodCount; ++lod)
    {
      LodBatch& batch = pBatches[lod];
      batch.m_uiVisible = visible[lod].GetCount();

      if (batch.m_uiVisible == 0)
        continue;

      // Capacity grows to what was needed; the allocation survives frames and smaller counts.
      const plUInt32 uiCapacity = plMath::Max(batch.m_uiVisible, batch.m_uiCapacity);
      if (!batch.m_Offset.IsInvalidated() && batch.m_uiCapacity < uiCapacity)
      {
        msg.m_pRenderDataManager->DeleteInstanceData(batch.m_Offset);
        batch.m_Offset = plInstanceDataOffset();
      }

      plArrayPtr<plPerInstanceData> instanceData = msg.m_pRenderDataManager->GetOrCreateInstanceData(this, true, batch.m_hBuffer, batch.m_Offset, uiCapacity);
      if (instanceData.GetCount() < batch.m_uiVisible)
      {
        batch.m_uiVisible = 0;
        continue;
      }

      batch.m_uiCapacity = uiCapacity;

      // CustomData: x = this LOD's lower screen-size threshold, y = the finer LOD's threshold (fade out beyond),
      // z = bounding radius at instance scale, w = crossfade band fraction. Zero w disables the dither.
      const float fLower = lod + 1 < uiLodCount ? pType->GetLodScreenSize(lod) : 0.0f;
      const float fUpper = lod > 0 ? pType->GetLodScreenSize(lod - 1) : 1.0e9f;
      const float fBand = bShadow ? 0.0f : fCrossfade;

      for (plUInt32 k = 0; k < batch.m_uiVisible; ++k)
      {
        const plVegetationCellInstance& inst = group.m_Instances[visible[lod][k]];

        plTransform transform;
        transform.m_vPosition = inst.m_vPosition;
        transform.m_qRotation = inst.m_qRotation;
        transform.m_vScale.Set(inst.m_fScale);

        const plColor color = seedColors[inst.m_uiColorSeed];
        const plUInt32 uiSeed = plHashingUtils::xxHash32(&inst.m_vPosition, sizeof(plVec3)) ^ inst.m_uiColorSeed;
        const plVec4 vCustomData(fLower, fUpper, fRadius * inst.m_fScale, fBand);

        plRenderDataManager::FillPerInstanceData(instanceData[k], nullptr, transform, uiOwnerId, color, vCustomData, fRadius * inst.m_fScale, uiSeed);
      }
    }
  }

  const plBoundingBoxSphere cellBounds = GetOwner()->GetGlobalBounds();

  for (plUInt32 lod = 0; lod < uiLodCount; ++lod)
  {
    const LodBatch& batch = pBatches[lod];
    if (batch.m_uiVisible == 0 || batch.m_Offset.IsInvalidated())
      continue;

    const plMeshResourceHandle& hMesh = pType->GetLodMesh(lod);
    plResourceLock<plMeshResource> pMesh(hMesh, plResourceAcquireMode::AllowLoadingFallback);
    plArrayPtr<const plMeshResourceDescriptor::SubMesh> parts = pMesh->GetSubMeshes();
    plArrayPtr<const plMaterialResourceHandle> meshMaterials = pMesh->GetMaterials();

    for (plUInt32 uiPart = 0; uiPart < parts.GetCount(); ++uiPart)
    {
      const plUInt32 uiMaterialIndex = parts[uiPart].m_uiMaterialIndex;

      plMaterialResourceHandle hMaterial = pType->GetMaterialOverride(uiMaterialIndex);
      if (!hMaterial.IsValid() && uiMaterialIndex < meshMaterials.GetCount())
        hMaterial = meshMaterials[uiMaterialIndex];

      if (!hMaterial.IsValid())
        continue;

      plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
      pRenderData->m_Flags.Add(plRenderData::Flags::Dynamic);
      // These batches follow the view. The ray tracing scene gets the cell's snapshots instead (GetRTChunks).
      pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
      pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
      pRenderData->SetFallbackGlobalBounds(cellBounds);
      pRenderData->Fill(batch.m_Offset, batch.m_hBuffer, hMaterial, hMesh, uiMaterialIndex, uiPart, batch.m_uiVisible);

      bool bDontCacheYet = false;
      const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);
      msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
    }
  }
}

void plVegetationCellComponent::BuildCompositeBatches(const plVegetationCellGroup& group, CompositeState& ref_state, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bShadow, const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const
{
  const auto& render = type.GetDescriptor().m_Render;
  if (bShadow && !render.m_bCastShadows)
    return;

  // A reloaded type may name other part meshes.
  if (ref_state.m_uiTypeChangeCounter != type.GetCurrentResourceChangeCounter())
  {
    ReleaseCompositeSet(msg.m_pRenderDataManager, ref_state.m_Main);
    ReleaseCompositeSet(msg.m_pRenderDataManager, ref_state.m_Shadow);
    ref_state.m_Split.Reset();
    ref_state.m_uiTypeChangeCounter = type.GetCurrentResourceChangeCounter();
  }

  // Once per frame, from the main camera, for every view: shadow views and the main view see the same trees as parts.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (ref_state.m_uiFrame != uiFrame)
  {
    ref_state.m_uiFrame = uiFrame;

    const float fDistanceScale = plMath::Max(cvar_VegetationDrawDistanceScale.GetValue(), 0.0f);

    plVegetationCompositeThresholds thresholds;
    thresholds.m_fSwitchDistance = plVegetationCompositeInfo::ComputeSwitchDistance(info.m_fSwitchError, 1.0f, module.GetMainCameraViewportHeight(), module.GetMainCameraTanHalfFov(), GetCompositeSwitchErrorPixels());
    // A coarse switch error would keep a tree in parts far out (a 0.2 m error at 1 px is ~190 m), and parts multiply
    // the instance count by the placements per tree: cap it. The merged mesh keeps the detail out there.
    if (cvar_VegetationCompositeMaxPartsDistance.GetValue() > 0.0f)
      thresholds.m_fSwitchDistance = plMath::Min(thresholds.m_fSwitchDistance, cvar_VegetationCompositeMaxPartsDistance.GetValue());
    thresholds.m_fShadowPartsDistance = plMath::Min(thresholds.m_fSwitchDistance, plMath::Max(cvar_VegetationCompositeShadowPartsDistance.GetValue(), 0.0f));
    thresholds.m_fCullDistance = render.m_fCullDistance * fDistanceScale;
    thresholds.m_fShadowCullDistance = render.m_bCastShadows ? plMath::Min(thresholds.m_fCullDistance, render.m_fShadowCullDistance * fDistanceScale) : -1.0f;
    thresholds.m_fDensity = plMath::Clamp(cvar_VegetationDensityScale.GetValue(), 0.0f, 1.0f);

    if (ref_state.m_Split.Update(group.m_Instances, module.GetMainCameraPosition(), thresholds))
    {
      ref_state.m_Main.m_bDirty = true;
      ref_state.m_Shadow.m_bDirty = true;
    }
  }

  CompositeSet& set = bShadow ? ref_state.m_Shadow : ref_state.m_Main;

  bool bBonesStale = set.m_bBonesPending || set.m_uiBonesGeneration != group.m_uiBonesGeneration;
  if (set.m_bDirty)
  {
    FillCompositeSet(group, set, ref_state.m_Split.GetStates(), type, info, bShadow, msg);
    bBonesStale = true;
  }

  if (bBonesStale)
    WriteCompositeBones(group, set, msg);

  const plBoundingBoxSphere cellBounds = GetOwner()->GetGlobalBounds();
  const plMeshResourceHandle& hMergedMesh = type.GetLodMesh(0);

  for (plUInt32 p = 0; p < set.m_Parts.GetCount() && p < info.m_PartMeshes.GetCount(); ++p)
    EmitCompositeBatch(set.m_Parts[p], info.m_PartMeshes[p], type, hMergedMesh, cellBounds, msg);

  EmitCompositeBatch(set.m_Merged, hMergedMesh, type, hMergedMesh, cellBounds, msg);
}

void plVegetationCellComponent::FillCompositeSet(const plVegetationCellGroup& group, CompositeSet& ref_set, plArrayPtr<const plUInt8> states, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bShadow, plMsgExtractRenderData& msg) const
{
  ref_set.m_bDirty = false;

  const plUInt32 uiPartCount = info.m_PartMeshes.GetCount();
  ref_set.m_Parts.SetCount(uiPartCount);

  // Part meshes: their reach around the placement origin, and whether they can be drawn yet. Trees whose parts are still
  // loading show the merged mesh meanwhile, and the set is filled again next frame.
  plHybridArray<float, 8> partRadius;
  plHybridArray<plUInt32, 8> partPlacements;
  partRadius.SetCount(uiPartCount);
  partPlacements.SetCount(uiPartCount);
  bool bPartsReady = true;
  for (plUInt32 p = 0; p < uiPartCount; ++p)
  {
    partRadius[p] = 0.0f;
    partPlacements[p] = 0;
    if (!info.m_PartMeshes[p].IsValid())
      continue;

    plResourceLock<plMeshResource> pPart(info.m_PartMeshes[p], plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pPart.GetAcquireResult() == plResourceAcquireResult::Final)
    {
      const plBoundingBoxSphere& bounds = pPart->GetBounds();
      partRadius[p] = bounds.IsValid() ? bounds.m_vCenter.GetLength() + bounds.m_fSphereRadius : 0.0f;
    }
    else if (pPart.GetAcquireResult() == plResourceAcquireResult::LoadingFallback)
    {
      bPartsReady = false;
    }
  }
  for (const plFoliageCompositeInstance& placement : info.m_Instances)
  {
    if (placement.m_uiPart < uiPartCount)
      ++partPlacements[placement.m_uiPart];
  }

  if (!bPartsReady)
    ref_set.m_bDirty = true;

  // What a tree draws in this set: shadows keep parts only for the trees near enough (ShadowParts).
  auto DrawnState = [bShadow](plUInt8 uiState) -> plUInt8
  {
    const plUInt8 uiMain = uiState & plVegetationCompositeTreeState::MainMask;
    if (bShadow && uiMain == plVegetationCompositeTreeState::Parts && (uiState & plVegetationCompositeTreeState::ShadowParts) == 0)
      return plVegetationCompositeTreeState::Merged;
    return uiMain;
  };

  // Counts first, so every batch is allocated once.
  plUInt32 uiPartTrees = 0;
  plUInt32 uiMergedTrees = 0;
  for (plUInt32 i = 0; i < states.GetCount(); ++i)
  {
    const plUInt8 uiState = states[i];
    if (bShadow && (uiState & plVegetationCompositeTreeState::CastsShadow) == 0)
      continue;

    const plUInt8 uiMain = DrawnState(uiState);
    if (uiMain == plVegetationCompositeTreeState::Parts && bPartsReady)
      ++uiPartTrees;
    else if (uiMain != plVegetationCompositeTreeState::Hidden)
      ++uiMergedTrees;
  }

  auto Allocate = [&](CompositeBatch& ref_batch, plUInt32 uiCount) -> plArrayPtr<plPerInstanceData>
  {
    ref_batch.m_uiCount = 0;
    ref_batch.m_Trees.Clear();
    if (uiCount == 0)
      return {};

    // Capacity grows to what was needed; the allocation survives smaller counts.
    const plUInt32 uiCapacity = plMath::Max(uiCount, ref_batch.m_uiCapacity);
    if (!ref_batch.m_Offset.IsInvalidated() && ref_batch.m_uiCapacity < uiCapacity)
    {
      msg.m_pRenderDataManager->DeleteInstanceData(ref_batch.m_Offset);
      ref_batch.m_Offset = plInstanceDataOffset();
    }

    plArrayPtr<plPerInstanceData> data = msg.m_pRenderDataManager->GetOrCreateInstanceData(this, true, ref_batch.m_hBuffer, ref_batch.m_Offset, uiCapacity);
    if (data.GetCount() < uiCount)
      return {};

    ref_batch.m_uiCapacity = uiCapacity;
    ref_batch.m_uiCount = uiCount;
    ref_batch.m_Trees.SetCountUninitialized(uiCount);
    return data;
  };

  plHybridArray<plArrayPtr<plPerInstanceData>, 8> partData;
  partData.SetCount(uiPartCount);
  for (plUInt32 p = 0; p < uiPartCount; ++p)
    partData[p] = Allocate(ref_set.m_Parts[p], info.m_PartMeshes[p].IsValid() ? uiPartTrees * partPlacements[p] : 0);
  plArrayPtr<plPerInstanceData> mergedData = Allocate(ref_set.m_Merged, uiMergedTrees);

  plColor seedColors[256];
  ResolveSeedColors(type, seedColors);

  const float fMergedRadius = type.GetBoundingRadius();
  const plUInt32 uiOwnerId = GetUniqueIdForRendering();
  plHybridArray<plUInt32, 8> partCursor;
  partCursor.SetCount(uiPartCount);
  for (plUInt32 p = 0; p < uiPartCount; ++p)
    partCursor[p] = 0;
  plUInt32 uiMergedCursor = 0;

  for (plUInt32 i = 0; i < states.GetCount(); ++i)
  {
    const plUInt8 uiState = states[i];
    if (bShadow && (uiState & plVegetationCompositeTreeState::CastsShadow) == 0)
      continue;

    plUInt8 uiMain = DrawnState(uiState);
    if (uiMain == plVegetationCompositeTreeState::Hidden)
      continue;
    if (!bPartsReady)
      uiMain = plVegetationCompositeTreeState::Merged;

    const plVegetationCellInstance& inst = group.m_Instances[i];

    plTransform tree;
    tree.m_vPosition = inst.m_vPosition;
    tree.m_qRotation = inst.m_qRotation;
    tree.m_vScale.Set(inst.m_fScale);

    const plColor color = seedColors[inst.m_uiColorSeed];
    const plUInt32 uiSeed = plHashingUtils::xxHash32(&inst.m_vPosition, sizeof(plVec3)) ^ inst.m_uiColorSeed;

    if (uiMain == plVegetationCompositeTreeState::Merged)
    {
      if (uiMergedCursor >= ref_set.m_Merged.m_uiCount)
        continue;

      // CustomData as the other types write it, with the LOD crossfade off. The merged mesh's bones come from its
      // vertices (TexCoord1), so Wind stays zero.
      const float fRadius = fMergedRadius * inst.m_fScale;
      plRenderDataManager::FillPerInstanceData(mergedData[uiMergedCursor], nullptr, tree, uiOwnerId, color, plVec4(0.0f, 1.0e9f, fRadius, 0.0f), fRadius, uiSeed);
      ref_set.m_Merged.m_Trees[uiMergedCursor] = i;
      ++uiMergedCursor;
      continue;
    }

    for (plUInt32 j = 0; j < info.m_Instances.GetCount(); ++j)
    {
      const plFoliageCompositeInstance& placement = info.m_Instances[j];
      const plUInt32 p = placement.m_uiPart;
      if (p >= uiPartCount || partCursor[p] >= ref_set.m_Parts[p].m_uiCount)
        continue;

      // The part's reach in the composite's space, grown by how far its bones can carry it.
      const plVec3 vPlacementScale = placement.m_Transform.m_vScale;
      const float fLocalRadius = partRadius[p] * plMath::Max(plMath::Abs(vPlacementScale.x), plMath::Abs(vPlacementScale.y), plMath::Abs(vPlacementScale.z));
      const float fRadius = (fLocalRadius + info.m_InstanceSway[j] + fLocalRadius * info.m_InstanceSwayPerMetre[j]) * inst.m_fScale;

      plPerInstanceData& data = partData[p][partCursor[p]];
      plRenderDataManager::FillPerInstanceData(data, nullptr, plTransform::MakeGlobalTransform(tree, placement.m_Transform), uiOwnerId, color, plVec4(0.0f, 1.0e9f, fRadius, 0.0f), fRadius, uiSeed);
      // Parts without a bone of their own keep their vertex bones: an all-zero Wind.
      data.Wind = placement.EncodeBones();

      ref_set.m_Parts[p].m_Trees[partCursor[p]] = i;
      ++partCursor[p];
    }
  }

  // An allocation that failed draws nothing rather than stale instances.
  ref_set.m_Merged.m_uiCount = uiMergedCursor;
  for (plUInt32 p = 0; p < uiPartCount; ++p)
    ref_set.m_Parts[p].m_uiCount = partCursor[p];
}

void plVegetationCellComponent::WriteCompositeBones(const plVegetationCellGroup& group, CompositeSet& ref_set, plMsgExtractRenderData& msg) const
{
  ref_set.m_uiBonesGeneration = group.m_uiBonesGeneration;
  ref_set.m_bBonesPending = false;

  if (m_uiFoliageBonesDataIndex == 0xFFFFFFFF)
  {
    ref_set.m_bBonesPending = true;
    return;
  }

  // Each tree's palette location, read from the pool once however many of its instances are drawn.
  plDynamicArray<plFoliageInstanceBones> treeBones;
  plDynamicArray<plUInt8> treeRead;
  treeBones.SetCount(group.m_Instances.GetCount());
  treeRead.SetCount(group.m_Instances.GetCount());

  auto Write = [&](CompositeBatch& ref_batch, plUInt32 uiFlags)
  {
    if (ref_batch.m_uiCount == 0)
      return;

    const plUInt32 uiCapacity = plMath::Max(ref_batch.m_uiCount, ref_batch.m_uiBonesCapacity);
    if (!ref_batch.m_BonesOffset.IsInvalidated() && ref_batch.m_uiBonesCapacity < uiCapacity)
      msg.m_pRenderDataManager->DeleteCustomInstanceData(m_uiFoliageBonesDataIndex, ref_batch.m_BonesOffset);

    plArrayPtr<plFoliageInstanceBones> slots = msg.m_pRenderDataManager->GetOrCreateCustomInstanceData<plFoliageInstanceBones>(m_uiFoliageBonesDataIndex, this, ref_batch.m_hBonesBuffer, ref_batch.m_BonesOffset, uiCapacity);
    if (slots.GetCount() < ref_batch.m_uiCount)
    {
      ref_set.m_bBonesPending = true;
      return;
    }
    ref_batch.m_uiBonesCapacity = uiCapacity;

    for (plUInt32 k = 0; k < ref_batch.m_uiCount; ++k)
    {
      const plUInt32 t = ref_batch.m_Trees[k];
      if (treeRead[t] == 0)
      {
        treeRead[t] = 1;
        const plUInt32 uiPoolInstance = group.m_Instances[t].m_uiBoneInstance;
        plFoliageBonePool::GetInstanceBones(uiPoolInstance, treeBones[t]);
        // Registered but not dispatched yet: the palette holds no pose. Read again next frame.
        ref_set.m_bBonesPending |= uiPoolInstance != plFoliageBonePool::InvalidInstance && treeBones[t].Valid == 0;
      }

      slots[k] = treeBones[t];
      slots[k].Flags |= uiFlags;
    }
  };

  for (CompositeBatch& batch : ref_set.m_Parts)
    Write(batch, 0);

  // The merged mesh gives each vertex the two heaviest bones of its part instance (TexCoord1.x and .y).
  Write(ref_set.m_Merged, FOLIAGE_BONES_FLAG_VERTEX_WEIGHTS);
}

void plVegetationCellComponent::EmitCompositeBatch(const CompositeBatch& batch, const plMeshResourceHandle& hMesh, const plVegetationTypeResource& type, const plMeshResourceHandle& hMergedMesh, const plBoundingBoxSphere& cellBounds, plMsgExtractRenderData& msg) const
{
  if (batch.m_uiCount == 0 || batch.m_Offset.IsInvalidated() || !hMesh.IsValid())
    return;

  plResourceLock<plMeshResource> pMesh(hMesh, plResourceAcquireMode::AllowLoadingFallback);
  if (!pMesh.IsValid())
    return;

  plArrayPtr<const plMeshResourceDescriptor::SubMesh> subMeshes = pMesh->GetSubMeshes();
  plArrayPtr<const plMaterialResourceHandle> meshMaterials = pMesh->GetMaterials();

  // Parts use the composite's material slots: the type's override, else the merged mesh's material, else their own.
  plHybridArray<plMaterialResourceHandle, 8> mergedMaterials;
  if (hMesh != hMergedMesh)
  {
    plResourceLock<plMeshResource> pMerged(hMergedMesh, plResourceAcquireMode::PointerOnly);
    if (pMerged.IsValid())
      mergedMaterials = pMerged->GetMaterials();
  }

  const bool bHasBones = !batch.m_BonesOffset.IsInvalidated();

  for (plUInt32 uiSubMesh = 0; uiSubMesh < subMeshes.GetCount(); ++uiSubMesh)
  {
    const plUInt32 uiMaterialIndex = subMeshes[uiSubMesh].m_uiMaterialIndex;

    plMaterialResourceHandle hMaterial = type.GetMaterialOverride(uiMaterialIndex);
    if (!hMaterial.IsValid() && uiMaterialIndex < mergedMaterials.GetCount())
      hMaterial = mergedMaterials[uiMaterialIndex];
    if (!hMaterial.IsValid() && uiMaterialIndex < meshMaterials.GetCount())
      hMaterial = meshMaterials[uiMaterialIndex];

    if (!hMaterial.IsValid())
      continue;

    plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
    // Not Dynamic: the trees stand still and only the bone shader moves them, so they keep the static categories and
    // the virtual shadow map caches their shadows instead of redrawing every clipmap level each frame.
    // The ray tracing scene gets the cell's rest-pose snapshot of the merged mesh instead (GetRTChunks).
    pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
    pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
    pRenderData->SetFallbackGlobalBounds(cellBounds);
    pRenderData->Fill(batch.m_Offset, batch.m_hBuffer, hMaterial, hMesh, uiMaterialIndex, uiSubMesh, batch.m_uiCount);

    // Instance k reads its bones at the batch's slot + k.
    if (bHasBones)
    {
      pRenderData->m_DataOffsets.m_uiCustomInstance = batch.m_BonesOffset.m_uiOffset;
      pRenderData->m_hCustomInstanceDataBuffer = batch.m_hBonesBuffer;
    }

    bool bDontCacheYet = false;
    const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);
    msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
  }
}

void plVegetationCellComponent::GetRTChunks(const plRenderDataManager* pRenderDataManager, const plVec3& vCenter, float fRangeCap, plDynamicArray<plSharedPtr<plRefCounted>>& out_chunks) const
{
  if (m_pCell == nullptr || pRenderDataManager == nullptr)
    return;

  PL_LOCK(m_RTMutex);

  ReleaseRTGroups(pRenderDataManager, false);

  // Instances or groups changed: every snapshot is made again.
  if (m_uiRTSyncedVersion != m_pCell->m_uiVersion || m_RTGroups.GetCount() != m_pCell->m_Groups.GetCount())
  {
    for (RTGroup& state : m_RTGroups)
      RetireRTGroup(state);

    m_RTGroups.Clear();
    m_RTGroups.SetCount(m_pCell->m_Groups.GetCount());
    m_uiRTSyncedVersion = m_pCell->m_uiVersion;
  }

  for (plUInt32 g = 0; g < m_pCell->m_Groups.GetCount(); ++g)
  {
    const plVegetationCellGroup& group = m_pCell->m_Groups[g];
    RTGroup& state = m_RTGroups[g];

    if (state.m_bBuilt)
    {
      // A reloaded type may have another mesh, range or mode, and the LOD override picks another mesh.
      plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::PointerOnly);
      if ((pType && pType->GetCurrentResourceChangeCounter() != state.m_uiTypeChangeCounter) || state.m_iLodOverride != cvar_VegetationRayTracingLodOverride.GetValue())
      {
        RetireRTGroup(state);
        state = RTGroup();
      }
    }

    if (!state.m_bBuilt)
      BuildRTGroup(group, state, pRenderDataManager);

    if (state.m_pChunk == nullptr)
      continue;

    const float fRange = plMath::Min(state.m_fRange, fRangeCap);
    if (state.m_Bounds.GetDistanceSquaredTo(vCenter) > fRange * fRange)
      continue;

    out_chunks.PushBack(plSharedPtr<plRefCounted>(state.m_pChunk));
  }
}

void plVegetationCellComponent::BuildRTGroup(const plVegetationCellGroup& group, RTGroup& ref_state, const plRenderDataManager* pRenderDataManager) const
{
  // Every early return without m_bBuilt waits for a resource and is tried again with the next request.
  ref_state.m_iLodOverride = cvar_VegetationRayTracingLodOverride.GetValue();

  const plUInt32 uiCount = group.m_Instances.GetCount();
  if (uiCount == 0)
  {
    ref_state.m_bBuilt = true;
    return;
  }

  plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pType.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  ref_state.m_uiTypeChangeCounter = pType->GetCurrentResourceChangeCounter();

  if (pType->GetLodCount() == 0)
  {
    ref_state.m_bBuilt = true;
    return;
  }

  bool bKnown = false;
  const bool bIncluded = pType->IsInRayTracingScene(bKnown);
  if (!bKnown)
    return;

  // A composite traces its merged mesh at rest pose (no bone data, no parts); which LOD that is shows once LOD 0 loaded.
  bool bCompositeKnown = false;
  pType->GetCompositeInfo(bCompositeKnown);
  if (!bCompositeKnown)
    return;

  if (cvar_VegetationRayTracingLogTypes)
  {
    LogRayTracingDecision(group.m_hType, *pType.GetPointer(), bIncluded);
  }

  if (!bIncluded)
  {
    ref_state.m_bBuilt = true;
    return;
  }

  const plMeshResourceHandle& hMesh = pType->GetLodMesh(pType->GetRayTracingLod());
  plResourceLock<plMeshResource> pMesh(hMesh, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final)
    return;

  struct Part
  {
    plUInt32 m_uiSubMesh = 0;
    plUInt32 m_uiMaterialIndex = 0;
    plMaterialResourceHandle m_hMaterial;
    plRenderData::Category m_Category;
  };
  plHybridArray<Part, 8> parts;

  plArrayPtr<const plMeshResourceDescriptor::SubMesh> subMeshes = pMesh->GetSubMeshes();
  plArrayPtr<const plMaterialResourceHandle> meshMaterials = pMesh->GetMaterials();
  for (plUInt32 uiSubMesh = 0; uiSubMesh < subMeshes.GetCount(); ++uiSubMesh)
  {
    const plUInt32 uiMaterialIndex = subMeshes[uiSubMesh].m_uiMaterialIndex;

    plMaterialResourceHandle hMaterial = pType->GetMaterialOverride(uiMaterialIndex);
    if (!hMaterial.IsValid() && uiMaterialIndex < meshMaterials.GetCount())
      hMaterial = meshMaterials[uiMaterialIndex];

    if (!hMaterial.IsValid())
      continue;

    // The category decides how the ray tracing scene treats the geometry (opaque, cut-out, translucent), so it has
    // to be the material's own.
    bool bFallback = false;
    const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bFallback);
    if (bFallback)
      return;

    Part& part = parts.ExpandAndGetRef();
    part.m_uiSubMesh = uiSubMesh;
    part.m_uiMaterialIndex = uiMaterialIndex;
    part.m_hMaterial = hMaterial;
    part.m_Category = category;
  }

  if (parts.IsEmpty())
  {
    ref_state.m_bBuilt = true;
    return;
  }

  plArrayPtr<plPerInstanceData> instanceData = pRenderDataManager->GetOrCreateInstanceData(this, false, ref_state.m_hBuffer, ref_state.m_Offset, uiCount);
  if (instanceData.GetCount() < uiCount)
  {
    if (!ref_state.m_Offset.IsInvalidated())
      pRenderDataManager->DeleteInstanceData(ref_state.m_Offset);
    return;
  }

  plColor seedColors[256];
  ResolveSeedColors(*pType.GetPointer(), seedColors);

  const float fRadius = pType->GetBoundingRadius();
  const plUInt32 uiOwnerId = GetUniqueIdForRendering();
  plBoundingBox bounds = plBoundingBox::MakeInvalid();

  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    const plVegetationCellInstance& inst = group.m_Instances[i];

    plTransform transform;
    transform.m_vPosition = inst.m_vPosition;
    transform.m_qRotation = inst.m_qRotation;
    transform.m_vScale.Set(inst.m_fScale);

    const float fInstanceRadius = fRadius * inst.m_fScale;
    bounds.ExpandToInclude(inst.m_vPosition - plVec3(fInstanceRadius));
    bounds.ExpandToInclude(inst.m_vPosition + plVec3(fInstanceRadius));

    // CustomData as the raster batches write it, with the LOD crossfade off.
    const plUInt32 uiSeed = plHashingUtils::xxHash32(&inst.m_vPosition, sizeof(plVec3)) ^ inst.m_uiColorSeed;
    const plVec4 vCustomData(0.0f, 1.0e9f, fInstanceRadius, 0.0f);
    plRenderDataManager::FillPerInstanceData(instanceData[i], nullptr, transform, uiOwnerId, seedColors[inst.m_uiColorSeed], vCustomData, fInstanceRadius, uiSeed);
  }

  plSharedPtr<plSceneRegistryRTChunk> pChunk = PL_DEFAULT_NEW(plSceneRegistryRTChunk);
  // Sized once: the ray tracing scene keeps pointers to the render data.
  pChunk->m_RenderData.SetCount(parts.GetCount());
  pChunk->m_Categories.SetCount(parts.GetCount());
  pChunk->m_Bounds = bounds;

  for (plUInt32 i = 0; i < parts.GetCount(); ++i)
  {
    const Part& part = parts[i];

    plMeshRenderData& renderData = pChunk->m_RenderData[i];
    renderData.m_hOwner = GetOwner()->GetHandle();
    renderData.m_GlobalTransform = plTransform::MakeIdentity(); // the instances carry their transforms
    renderData.Fill(ref_state.m_Offset, ref_state.m_hBuffer, part.m_hMaterial, hMesh, part.m_uiMaterialIndex, part.m_uiSubMesh, uiCount);
    renderData.m_vGlobalPosition = bounds.GetCenter();
    pChunk->m_Categories[i] = part.m_Category;
  }

  ref_state.m_pChunk = pChunk;
  ref_state.m_Bounds = bounds;
  ref_state.m_fRange = pType->GetRayTracingRange();
  ref_state.m_bBuilt = true;
}

void plVegetationCellComponent::RetireRTGroup(RTGroup& ref_state) const
{
  if (ref_state.m_Offset.IsInvalidated())
    return;

  RetiredRTData& retired = m_RetiredRTData.ExpandAndGetRef();
  retired.m_Offset = ref_state.m_Offset;
  retired.m_uiFrame = plRenderWorld::GetFrameCounter();
  ref_state.m_Offset = plInstanceDataOffset();
}

void plVegetationCellComponent::ReleaseRTGroups(const plRenderDataManager* pRenderDataManager, bool bAll) const
{
  if (bAll)
  {
    for (RTGroup& state : m_RTGroups)
      RetireRTGroup(state);

    m_RTGroups.Clear();
    m_uiRTSyncedVersion = 0xFFFFFFFF;
  }

  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  for (plUInt32 i = m_RetiredRTData.GetCount(); i-- > 0;)
  {
    if (!bAll && uiFrame < m_RetiredRTData[i].m_uiFrame + c_uiRTRetireFrames)
      continue;

    if (pRenderDataManager != nullptr)
      pRenderDataManager->DeleteInstanceData(m_RetiredRTData[i].m_Offset);

    m_RetiredRTData.RemoveAtAndSwap(i);
  }
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationCellComponent);
