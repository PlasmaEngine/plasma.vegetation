#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Camera.h>
#include <Foundation/Configuration/CVar.h>
#include <RendererCore/Lights/Implementation/VirtualShadowPool.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/DynamicBuffer.h>
#include <VegetationPlugin/Components/VegetationCellComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

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
plCVarFloat cvar_VegetationDensityScale("Vegetation.DensityScale", 1.0f, plCVarFlags::Save, "Fraction of instances kept by distance thinning, 0..1");

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
  }

  m_GroupStates.Clear();
  m_uiSyncedVersion = 0xFFFFFFFF;
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
    fTanHalfFov = plMath::Tan(pCamera->GetFovY(1.0f) * 0.5f);

    if (bMain)
    {
      pModule->ReportMainCamera(vCameraPosition, fTanHalfFov);
    }
  }

  // Main views and multiple shadow union views extract this component concurrently.
  // Keep batch allocation, refill and emitted offsets together so no view observes a
  // partially rebuilt set or races the version synchronization.
  PL_LOCK(m_BatchMutex);
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

  // The instance data allocations persist, so the cull and LOD pass only reruns when something that feeds it moved:
  // the camera by a few percent of the cull distance, the cvars, or the cell's content (a version change clears
  // the states before we get here). A static camera costs nothing per frame.
  const float fRefillDistance = plMath::Max(0.5f, fCullDistance * 0.04f);
  const bool bRefill = set.m_uiFrame == 0 || set.m_fFillDistanceScale != fDistanceScale || set.m_fFillDensity != fDensity ||
                       (vCameraPosition - set.m_vFillCamera).GetLengthSquared() > fRefillDistance * fRefillDistance;

  if (bRefill && set.m_uiFrame != uiCurrentFrame)
  {
    set.m_uiFrame = uiCurrentFrame;
    set.m_vFillCamera = vCameraPosition;
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

    // Colour per seed, resolved once per refill; white without a gradient (locking an invalid handle asserts).
    plColor seedColors[256];
    for (plUInt32 s = 0; s < 256; ++s)
      seedColors[s] = plColor::White;

    if (const plColorGradientResourceHandle hGradient = pType->GetColorGradient(); hGradient.IsValid())
    {
      plResourceLock<plColorGradientResource> pGradient(hGradient, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
      if (pGradient.GetAcquireResult() == plResourceAcquireResult::Final)
      {
        for (plUInt32 s = 0; s < 256; ++s)
          seedColors[s] = pGradient->Evaluate(s / 255.0);
      }
    }

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
      pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
      pRenderData->SetFallbackGlobalBounds(cellBounds);
      pRenderData->Fill(batch.m_Offset, batch.m_hBuffer, hMaterial, hMesh, uiMaterialIndex, uiPart, batch.m_uiVisible);

      bool bDontCacheYet = false;
      const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);
      msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
    }
  }
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationCellComponent);
