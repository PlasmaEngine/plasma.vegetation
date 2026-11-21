#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Camera.h>
#include <Foundation/Configuration/CVar.h>
#include <RendererCore/Lights/Implementation/VirtualShadowPool.h>
#include <RendererCore/Meshes/MeshComponentBase.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <RendererCore/Pipeline/RenderDataManager.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Scene/SceneRegistry.h>
#include <RendererCore/Wind/FoliageBonePool.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/DynamicBuffer.h>
#include <VegetationPlugin/Components/VegetationCellComponent.h>
#include <VegetationPlugin/World/VegetationCellShell.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

#include <Shaders/Vegetation/FoliageBonesData.h>
#include <Shaders/Vegetation/FoliageRepresentation.h>

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

// Far representations of foliage composite types, nearest to farthest: trees (parts, then the merged mesh through its
// cluster hierarchy) up to Vegetation.CellShell.Distance, the cell's merged voxel shell from there, and impostors beyond
// the type's impostor distance (times Vegetation.Impostor.DistanceScale). Every hand-off is a short screen dither.
plCVarBool cvar_VegetationImpostorEnabled("Vegetation.Impostor.Enabled", true, plCVarFlags::Default,
  "Foliage composite trees with a baked impostor draw it beyond their type's impostor distance; off keeps the shell or the merged mesh out there");
plCVarFloat cvar_VegetationImpostorDistanceScale("Vegetation.Impostor.DistanceScale", 1.0f, plCVarFlags::Default,
  "Scales every vegetation type's impostor distance (the hand-off to the impostors)");
plCVarFloat cvar_VegetationImpostorFadeDistance("Vegetation.Impostor.FadeDistance", 10.0f, plCVarFlags::Default,
  "Metres over which the impostors dither in against the shell or the merged mesh. 0 = switch at once");
plCVarBool cvar_VegetationImpostorShadows("Vegetation.Impostor.Shadows", true, plCVarFlags::Default,
  "Trees beyond the impostor distance cast their shadow with the impostor too; off casts it with the merged mesh");
plCVarBool cvar_VegetationImpostorFarCells("Vegetation.Impostor.FarCells", true, plCVarFlags::Default,
  "Vegetation cells entirely beyond the impostor hand-off draw their impostors from instance data written once, without per-tree work per frame");
plCVarBool cvar_VegetationCellShellEnabled("Vegetation.CellShell.Enabled", true, plCVarFlags::Default,
  "Foliage composite trees with a voxel proxy draw as their cell's merged voxel shell between Vegetation.CellShell.Distance and the impostors");
plCVarFloat cvar_VegetationCellShellDistance("Vegetation.CellShell.Distance", 90.0f, plCVarFlags::Default,
  "Metres from the main camera where the trees hand over to their cell's merged voxel shell (the middle of the hand-off)");
plCVarFloat cvar_VegetationCellShellFadeDistance("Vegetation.CellShell.FadeDistance", 10.0f, plCVarFlags::Default,
  "Metres over which the trees and the cell's voxel shell dither against each other");
plCVarFloat cvar_VegetationCellShellVoxelSize("Vegetation.CellShell.VoxelSize", 0.6f, plCVarFlags::Default,
  "Metres: the voxel grid of the cells' merged voxel shells. Cells rebuild when it changes");

/// Builds a cell's merged voxel shell on a task from a copy of what it needs.
class plVegetationCellShellTask final : public plTask
{
public:
  plVegetationCellShellTask() { ConfigureTask("Vegetation Cell Shell", plTaskNesting::Never); }

  plDynamicArray<plVegetationCellShellTree> m_Trees;
  plHybridArray<plSharedPtr<plVegetationVoxelProxy>, 8> m_Proxies; ///< keeps the trees' proxies alive
  plVec3 m_vOrigin = plVec3::MakeZero();
  plVegetationCellShellSettings m_Settings;

  plMeshResourceDescriptor m_Mesh;
  plVegetationCellShellStats m_Stats;
  bool m_bSucceeded = false;

private:
  virtual void Execute() override { m_bSucceeded = plVegetationCellShellBuilder::Build(m_Trees, m_vOrigin, m_Settings, m_Mesh, &m_Stats).Succeeded(); }
};

extern plCVarInt cvar_VegetationRayTracingLodOverride;
plCVarBool cvar_VegetationRayTracingLogTypes("Vegetation.RayTracing.LogTypes", false, plCVarFlags::Default, "Log once per vegetation type whether it is part of the ray tracing scene, with its size, traced LOD and range.");

namespace
{
  /// Marks a LOD batch entry whose instance dithers between two levels (the rest of the entry is the instance index).
  constexpr plUInt32 s_uiCrossfadeEntry = PL_BIT(31);

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

  /// The random seed of a drawn instance: a hash of its position and colour seed, with how it is drawn in the top bits
  /// for the Foliage Representation view mode.
  plUInt32 MakeInstanceSeed(const plVegetationCellInstance& inst, plUInt32 uiRepresentation)
  {
    const plUInt32 uiHash = plHashingUtils::xxHash32(&inst.m_vPosition, sizeof(plVec3)) ^ inst.m_uiColorSeed;
    return (uiHash & FOLIAGE_REPRESENTATION_SEED_MASK) | (uiRepresentation << FOLIAGE_REPRESENTATION_SHIFT);
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
    PL_LOCK(m_BatchMutex);
    ReleaseShell(GetWorld()->GetModuleReadOnly<plRenderDataManager>());
  }

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
    ReleaseCompositeBatch(pRenderDataManager, state.m_Composite.m_CellMain);
    ReleaseCompositeBatch(pRenderDataManager, state.m_Composite.m_CellShadow);
    state.m_Composite.m_bCellBuilt = false;
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
  ReleaseCompositeBatch(pRenderDataManager, ref_set.m_Impostor);

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

  // The shell is decided once per frame, before any group splits its trees around it.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (m_Shell.m_uiUpdateFrame != uiFrame)
  {
    m_Shell.m_uiUpdateFrame = uiFrame;
    UpdateShell(*pModule, msg);
  }

  for (plUInt32 g = 0; g < m_pCell->m_Groups.GetCount(); ++g)
  {
    BuildBatches(m_pCell->m_Groups[g], m_GroupStates[g], bShadow, vCameraPosition, fTanHalfFov, msg);
  }

  EmitShell(*pModule, bShadow, msg);
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

      // Crossfade bands around the thresholds this instance is next to (main views only; shadows pop). Entries of an
      // instance that crossfades carry s_uiCrossfadeEntry, in both of its levels.
      plUInt32 uiEntry = i;
      if (!bShadow && fCrossfade > 0.0f)
      {
        if (uiLod > 0)
        {
          const float fUpper = pType->GetLodScreenSize(uiLod - 1);
          if (fScreenSize >= fUpper * (1.0f - fCrossfade))
          {
            visible[uiLod - 1].PushBack(i | s_uiCrossfadeEntry);
            uiEntry |= s_uiCrossfadeEntry;
          }
        }
        if (uiLod + 1 < uiLodCount)
        {
          const float fLower = pType->GetLodScreenSize(uiLod);
          if (fScreenSize <= fLower * (1.0f + fCrossfade))
          {
            visible[uiLod + 1].PushBack(i | s_uiCrossfadeEntry);
            uiEntry |= s_uiCrossfadeEntry;
          }
        }
      }

      visible[uiLod].PushBack(uiEntry);
    }

    // Colour per seed, resolved once per refill.
    plColor seedColors[256];
    ResolveSeedColors(*pType.GetPointer(), seedColors);

    const plUInt32 uiOwnerId = GetUniqueIdForRendering();

    for (plUInt32 lod = 0; lod < uiLodCount; ++lod)
    {
      LodBatch& batch = pBatches[lod];
      batch.m_uiVisible = visible[lod].GetCount();
      batch.m_uiCrossfading = 0;

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
      // Steady instances first, crossfading ones last: they are drawn as a second render data flagged LodCrossfade, the
      // only draws whose opaque materials compile the dither discard. Steady instances get a zero band.
      plHybridArray<plUInt32, 256>& entries = visible[lod];
      plUInt32 uiSteady = 0;
      for (plUInt32 k = 0; k < entries.GetCount(); ++k)
      {
        if ((entries[k] & s_uiCrossfadeEntry) == 0)
          plMath::Swap(entries[uiSteady++], entries[k]);
      }
      batch.m_uiCrossfading = batch.m_uiVisible - uiSteady;

      for (plUInt32 k = 0; k < batch.m_uiVisible; ++k)
      {
        const bool bCrossfading = (entries[k] & s_uiCrossfadeEntry) != 0;
        const float fBand = bCrossfading ? fCrossfade : 0.0f;
        const plVegetationCellInstance& inst = group.m_Instances[entries[k] & ~s_uiCrossfadeEntry];

        plTransform transform;
        transform.m_vPosition = inst.m_vPosition;
        transform.m_qRotation = inst.m_qRotation;
        transform.m_vScale.Set(inst.m_fScale);

        const plColor color = seedColors[inst.m_uiColorSeed];
        const plUInt32 uiSeed = MakeInstanceSeed(inst, FOLIAGE_REPRESENTATION_LOD);
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

      bool bDontCacheYet = false;
      const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);

      // The steady instances, then the crossfading ones at the end of the same allocation.
      const plUInt32 uiSteady = batch.m_uiVisible - batch.m_uiCrossfading;
      for (plUInt32 uiSplit = 0; uiSplit < 2; ++uiSplit)
      {
        const plUInt32 uiCount = uiSplit == 0 ? uiSteady : batch.m_uiCrossfading;
        if (uiCount == 0)
          continue;

        plInstanceDataOffset offset = batch.m_Offset;
        offset.m_uiOffset += uiSplit == 0 ? 0 : uiSteady;

        plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
        pRenderData->m_Flags.Add(plRenderData::Flags::Dynamic);
        // These batches follow the view. The ray tracing scene gets the cell's snapshots instead (GetRTChunks).
        pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
        // Editor clicks go through vegetation to what lies beneath: a pick would redraw the whole batch.
        pRenderData->m_Flags.Add(plRenderData::Flags::NotPickable);
        if (uiSplit == 1)
          pRenderData->m_Flags.Add(plRenderData::Flags::LodCrossfade);
        pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
        pRenderData->SetFallbackGlobalBounds(cellBounds);
        pRenderData->Fill(offset, batch.m_hBuffer, hMaterial, hMesh, uiMaterialIndex, uiPart, uiCount);

        msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
      }
    }
  }
}

void plVegetationCellComponent::BuildCompositeBatches(const plVegetationCellGroup& group, CompositeState& ref_state, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bShadow, const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const
{
  const auto& render = type.GetDescriptor().m_Render;
  if (bShadow && !render.m_bCastShadows)
    return;

  // A reloaded type may name other part meshes, or carry another impostor.
  if (ref_state.m_uiTypeChangeCounter != type.GetCurrentResourceChangeCounter())
  {
    ReleaseCompositeSet(msg.m_pRenderDataManager, ref_state.m_Main);
    ReleaseCompositeSet(msg.m_pRenderDataManager, ref_state.m_Shadow);
    ReleaseCompositeBatch(msg.m_pRenderDataManager, ref_state.m_CellMain);
    ReleaseCompositeBatch(msg.m_pRenderDataManager, ref_state.m_CellShadow);
    ref_state.m_bCellBuilt = false;
    ref_state.m_Split.Reset();
    ref_state.m_uiTypeChangeCounter = type.GetCurrentResourceChangeCounter();
  }

  const plVegetationImpostorGpu* pImpostor = nullptr;
  if (cvar_VegetationImpostorEnabled && type.GetImpostorDistance() > 0.0f)
  {
    bool bKnown = false;
    pImpostor = type.GetImpostor(bKnown);
  }

  // A cell far enough away draws the whole group from instance data it wrote once.
  if (pImpostor != nullptr && IsMergedCell(module) && BuildMergedCell(group, ref_state, type, *pImpostor, bShadow, msg))
    return;

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

    const float fTanHalfFov = plMath::Max(module.GetMainCameraTanHalfFov(), 0.0001f);

    // The hand-off to the cell's voxel shell, when the cell has one for this type, never before the parts are gone. The
    // trees fade by screen size (ApplyLodCrossfade, with CustomData x as the screen size per unit of radius), the shell
    // per pixel by distance over the same metres.
    ref_state.m_bShell = m_Shell.m_bReady && pImpostor != nullptr && type.GetVoxelProxy() != nullptr;
    ref_state.m_fCellLower = 0.0f;
    ref_state.m_fCellBand = 0.0f;
    float fCellDistance = 0.0f;
    if (ref_state.m_bShell)
    {
      const float fFade = plMath::Max(cvar_VegetationCellShellFadeDistance.GetValue(), 0.0f);
      fCellDistance = plMath::Max(cvar_VegetationCellShellDistance.GetValue(), thresholds.m_fSwitchDistance + fFade * 0.5f + 1.0f);
      const float fBand = plMath::Clamp(fFade / (2.0f * fCellDistance), 0.0f, 0.4f);
      thresholds.m_fCellNear = fCellDistance / (1.0f + fBand) * 0.98f;
      thresholds.m_fCellFar = fCellDistance / (1.0f - fBand) * 1.02f;
      ref_state.m_fCellLower = 1.0f / (fCellDistance * fTanHalfFov);
      ref_state.m_fCellBand = fBand;
    }

    // The hand-off to the impostor. Against the merged mesh it fades like the shell hand-off; against the shell both
    // fade by distance over [start, start + length].
    ref_state.m_fImpostorLower = 0.0f;
    ref_state.m_fImpostorBand = 0.0f;
    ref_state.m_fImpostorFadeStart = 0.0f;
    ref_state.m_fImpostorFadeLength = 0.0f;
    if (pImpostor != nullptr)
    {
      const float fFade = plMath::Max(cvar_VegetationImpostorFadeDistance.GetValue(), 0.0f);
      float fDistance = ref_state.m_bShell ? m_Shell.m_fImpostorDistance : type.GetImpostorDistance() * plMath::Max(cvar_VegetationImpostorDistanceScale.GetValue(), 0.0f);
      fDistance = plMath::Max(fDistance, plMath::Max(thresholds.m_fSwitchDistance, thresholds.m_fCellFar) + fFade * 0.5f + 1.0f);

      if (ref_state.m_bShell)
      {
        ref_state.m_fImpostorFadeStart = fDistance - fFade * 0.5f;
        ref_state.m_fImpostorFadeLength = fFade;
        thresholds.m_fImpostorNear = ref_state.m_fImpostorFadeStart - 0.5f;
        thresholds.m_fImpostorFar = ref_state.m_fImpostorFadeStart + fFade + 0.5f;
      }
      else
      {
        const float fBand = plMath::Clamp(fFade / (2.0f * fDistance), 0.0f, 0.4f);
        thresholds.m_fImpostorNear = fDistance / (1.0f + fBand) * 0.98f;
        thresholds.m_fImpostorFar = fDistance / (1.0f - fBand) * 1.02f;
        ref_state.m_fImpostorLower = 1.0f / (fDistance * fTanHalfFov);
        ref_state.m_fImpostorBand = fBand;
      }
      thresholds.m_fShadowImpostorDistance = cvar_VegetationImpostorShadows ? fDistance : 0.0f;
    }

    if (ref_state.m_Split.Update(group.m_Instances, module.GetMainCameraPosition(), thresholds))
    {
      ref_state.m_Main.m_bDirty = true;
      ref_state.m_Shadow.m_bDirty = true;
    }
  }

  CompositeSet& set = bShadow ? ref_state.m_Shadow : ref_state.m_Main;

  // A zoom moves the hand-off screen sizes: the fading instances are written again.
  if (set.m_fFillImpostorLower != ref_state.m_fImpostorLower || set.m_fFillCellLower != ref_state.m_fCellLower)
    set.m_bDirty = true;

  bool bBonesStale = set.m_bBonesPending || set.m_uiBonesGeneration != group.m_uiBonesGeneration;
  if (set.m_bDirty)
  {
    FillCompositeSet(group, set, ref_state, type, info, pImpostor != nullptr, bShadow, msg);
    bBonesStale = true;
  }

  // Shared trees' bone data is static: written when the split or a generation changes. Only promoted trees that blend
  // are rewritten per frame.
  if (bBonesStale)
    WriteCompositeBones(group, set, module, msg);
  else if (group.m_bBonesBlending)
    WriteActiveCompositeBones(group, set, module, msg);

  const plBoundingBoxSphere cellBounds = GetOwner()->GetGlobalBounds();
  const plMeshResourceHandle& hMergedMesh = type.GetLodMesh(0);

  for (plUInt32 p = 0; p < set.m_Parts.GetCount() && p < info.m_PartMeshes.GetCount(); ++p)
    EmitCompositeBatch(set.m_Parts[p], info.m_PartMeshes[p], type, hMergedMesh, cellBounds, msg);

  EmitCompositeBatch(set.m_Merged, hMergedMesh, type, hMergedMesh, cellBounds, msg);

  if (pImpostor != nullptr)
    EmitImpostorBatch(set.m_Impostor, *pImpostor, cellBounds, msg);
}

void plVegetationCellComponent::FillCompositeSet(const plVegetationCellGroup& group, CompositeSet& ref_set, const CompositeState& state, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bHasImpostor, bool bShadow, plMsgExtractRenderData& msg) const
{
  const plArrayPtr<const plUInt8> states = state.m_Split.GetStates();
  ref_set.m_bDirty = false;
  ref_set.m_fFillImpostorLower = state.m_fImpostorLower;
  ref_set.m_fFillCellLower = state.m_fCellLower;

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

  // What a tree draws in this set: shadows keep parts only for the trees near enough (ShadowParts) and switch to the
  // impostor at one distance (ShadowImpostor); without an impostor the merged mesh stands in for it.
  auto DrawnState = [bShadow, bHasImpostor](plUInt8 uiState) -> plUInt8
  {
    plUInt8 uiMain = uiState & plVegetationCompositeTreeState::MainMask;
    if (bShadow)
    {
      if (uiMain == plVegetationCompositeTreeState::Hidden)
        return uiMain;
      if (bHasImpostor && (uiState & plVegetationCompositeTreeState::ShadowImpostor) != 0)
        return plVegetationCompositeTreeState::Impostor;
      if (uiMain == plVegetationCompositeTreeState::Impostor)
        return plVegetationCompositeTreeState::Merged;
      if (uiMain == plVegetationCompositeTreeState::Parts && (uiState & plVegetationCompositeTreeState::ShadowParts) == 0)
        return plVegetationCompositeTreeState::Merged;
      return uiMain;
    }

    if (uiMain == plVegetationCompositeTreeState::Impostor && !bHasImpostor)
      return plVegetationCompositeTreeState::Merged;
    return uiMain;
  };

  // Main views draw a fading tree twice: its merged mesh fades out while its impostor fades in.
  auto AlsoImpostor = [bShadow, bHasImpostor](plUInt8 uiState) -> bool
  { return !bShadow && bHasImpostor && (uiState & plVegetationCompositeTreeState::ImpostorFade) != 0; };

  // A merged mesh in a hand-off band dithers out against the shell or the impostor: such instances go last in the merged
  // batch and draw as a second render data flagged LodCrossfade, the only draws whose opaque materials compile the discard.
  auto MergedFades = [&](plUInt8 uiState) -> bool
  {
    if (bShadow)
      return false;
    if (state.m_bShell && (uiState & plVegetationCompositeTreeState::CellFade) != 0)
      return true;
    return !state.m_bShell && bHasImpostor && (uiState & plVegetationCompositeTreeState::ImpostorFade) != 0;
  };

  // Counts first, so every batch is allocated once.
  plUInt32 uiPartTrees = 0;
  plUInt32 uiMergedTrees = 0;
  plUInt32 uiMergedFading = 0;
  plUInt32 uiImpostorTrees = 0;
  for (plUInt32 i = 0; i < states.GetCount(); ++i)
  {
    const plUInt8 uiState = states[i];
    if (bShadow && (uiState & plVegetationCompositeTreeState::CastsShadow) == 0)
      continue;

    const plUInt8 uiMain = DrawnState(uiState);
    if (uiMain == plVegetationCompositeTreeState::Parts && bPartsReady)
      ++uiPartTrees;
    else if (uiMain == plVegetationCompositeTreeState::Impostor)
      ++uiImpostorTrees;
    else if (uiMain != plVegetationCompositeTreeState::Hidden)
    {
      ++uiMergedTrees;
      if (uiMain == plVegetationCompositeTreeState::Merged && MergedFades(uiState))
        ++uiMergedFading;
    }

    if (AlsoImpostor(uiState))
      ++uiImpostorTrees;
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
  plArrayPtr<plPerInstanceData> impostorData = Allocate(ref_set.m_Impostor, uiImpostorTrees);
  plUInt32 uiImpostorCursor = 0;

  plColor seedColors[256];
  ResolveSeedColors(type, seedColors);

  const float fMergedRadius = type.GetBoundingRadius();
  const plUInt32 uiOwnerId = GetUniqueIdForRendering();
  plHybridArray<plUInt32, 8> partCursor;
  partCursor.SetCount(uiPartCount);
  for (plUInt32 p = 0; p < uiPartCount; ++p)
    partCursor[p] = 0;
  plUInt32 uiMergedCursor = 0;
  plUInt32 uiMergedFadeCursor = 0;

  for (plUInt32 i = 0; i < states.GetCount(); ++i)
  {
    const plUInt8 uiState = states[i];
    if (bShadow && (uiState & plVegetationCompositeTreeState::CastsShadow) == 0)
      continue;

    plUInt8 uiMain = DrawnState(uiState);
    // A tree the shell holds still draws its impostor while that fades in.
    if (uiMain == plVegetationCompositeTreeState::Hidden && !AlsoImpostor(uiState))
      continue;
    if (!bPartsReady && uiMain == plVegetationCompositeTreeState::Parts)
      uiMain = plVegetationCompositeTreeState::Merged;

    const plVegetationCellInstance& inst = group.m_Instances[i];

    plTransform tree;
    tree.m_vPosition = inst.m_vPosition;
    tree.m_qRotation = inst.m_qRotation;
    tree.m_vScale.Set(inst.m_fScale);

    const plColor color = seedColors[inst.m_uiColorSeed];
    const float fTreeRadius = fMergedRadius * inst.m_fScale;

    // Fading trees carry their hand-off in CustomData. Merged mesh: x = screen size where it is half gone, w = band
    // (Vegetation.plShader ApplyLodCrossfade); the shell or the impostor draws the complementary pixels. Impostor against
    // the shell: x = start, y = length in metres, w = -1 (VegetationImpostor.plShader).
    plVec4 vFadeData(0.0f, 1.0e9f, fTreeRadius, 0.0f);
    plVec4 vImpostorData = vFadeData;
    if (!bShadow)
    {
      if ((uiState & plVegetationCompositeTreeState::CellFade) != 0 && state.m_bShell)
        vFadeData = plVec4(state.m_fCellLower * fTreeRadius, 1.0e9f, fTreeRadius, state.m_fCellBand);

      if (bHasImpostor && (uiState & plVegetationCompositeTreeState::ImpostorFade) != 0)
      {
        if (state.m_bShell)
        {
          vImpostorData = plVec4(state.m_fImpostorFadeStart, state.m_fImpostorFadeLength, fTreeRadius, -1.0f);
        }
        else
        {
          vFadeData = plVec4(state.m_fImpostorLower * fTreeRadius, 1.0e9f, fTreeRadius, state.m_fImpostorBand);
          vImpostorData = vFadeData;
        }
      }
    }

    if (uiMain == plVegetationCompositeTreeState::Impostor || AlsoImpostor(uiState))
    {
      if (uiImpostorCursor < ref_set.m_Impostor.m_uiCount)
      {
        plRenderDataManager::FillPerInstanceData(impostorData[uiImpostorCursor], nullptr, tree, uiOwnerId, color, vImpostorData, fTreeRadius, MakeInstanceSeed(inst, FOLIAGE_REPRESENTATION_IMPOSTOR));
        ref_set.m_Impostor.m_Trees[uiImpostorCursor] = i;
        ++uiImpostorCursor;
      }

      if (uiMain == plVegetationCompositeTreeState::Impostor || uiMain == plVegetationCompositeTreeState::Hidden)
        continue;
    }

    if (uiMain == plVegetationCompositeTreeState::Merged)
    {
      // Steady trees from the front, fading ones after them.
      const bool bFades = MergedFades(uiState);
      const plUInt32 uiSlot = bFades ? (uiMergedTrees - uiMergedFading) + uiMergedFadeCursor : uiMergedCursor;
      if (uiSlot >= ref_set.m_Merged.m_uiCount || (!bFades && uiMergedCursor >= uiMergedTrees - uiMergedFading) || (bFades && uiMergedFadeCursor >= uiMergedFading))
        continue;

      // CustomData as the other types write it, the LOD crossfade only in a hand-off. The merged mesh's bones come from
      // its vertices (TexCoord1), so Wind stays zero.
      plRenderDataManager::FillPerInstanceData(mergedData[uiSlot], nullptr, tree, uiOwnerId, color, bFades ? vFadeData : plVec4(0.0f, 1.0e9f, fTreeRadius, 0.0f), fTreeRadius, MakeInstanceSeed(inst, FOLIAGE_REPRESENTATION_MERGED));
      ref_set.m_Merged.m_Trees[uiSlot] = i;
      if (bFades)
        ++uiMergedFadeCursor;
      else
        ++uiMergedCursor;
      continue;
    }

    const plUInt32 uiSeed = MakeInstanceSeed(inst, FOLIAGE_REPRESENTATION_PARTS);
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
  if (uiMergedCursor == uiMergedTrees - uiMergedFading && uiMergedFadeCursor == uiMergedFading)
  {
    ref_set.m_Merged.m_uiCount = uiMergedTrees;
    ref_set.m_Merged.m_uiCrossfading = uiMergedFading;
  }
  else
  {
    ref_set.m_Merged.m_uiCount = uiMergedCursor;
    ref_set.m_Merged.m_uiCrossfading = 0;
  }
  ref_set.m_Impostor.m_uiCount = uiImpostorCursor;
  for (plUInt32 p = 0; p < uiPartCount; ++p)
    ref_set.m_Parts[p].m_uiCount = partCursor[p];
}

void plVegetationCellComponent::WriteCompositeBones(const plVegetationCellGroup& group, CompositeSet& ref_set, const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const
{
  ref_set.m_uiBonesGeneration = group.m_uiBonesGeneration;
  ref_set.m_bBonesPending = false;
  ref_set.m_ActiveSlots.Clear();
  ref_set.m_uiActiveWriteFrame = plRenderWorld::GetFrameCounter();

  if (m_uiFoliageBonesDataIndex == 0xFFFFFFFF)
  {
    ref_set.m_bBonesPending = true;
    return;
  }

  // Each tree's bone data, made once however many of its instances are drawn.
  const plUInt64 uiTypeKey = group.m_hType.GetResourceIDHash();
  plDynamicArray<plFoliageInstanceBones> treeBones;
  plDynamicArray<plUInt8> treeRead;
  treeBones.SetCount(group.m_Instances.GetCount());
  treeRead.SetCount(group.m_Instances.GetCount());

  auto Write = [&](CompositeBatch& ref_batch, plUInt32 uiBatch, plUInt32 uiFlags)
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
        module.FillTreeBones(uiTypeKey, group.m_Instances[t], treeBones[t]);
      }

      slots[k] = treeBones[t];
      slots[k].Flags |= uiFlags;

      // A promoted tree's own pose and blend move while it blends: its slots are rewritten then (the pool's own pose
      // becomes valid, or the blend moves, without a generation change).
      if (group.m_Instances[t].m_uiBoneInstance != plFoliageBonePool::InvalidInstance)
        ref_set.m_ActiveSlots.PushBack({uiBatch, k});
    }
  };

  for (plUInt32 p = 0; p < ref_set.m_Parts.GetCount(); ++p)
    Write(ref_set.m_Parts[p], p, 0);

  // The merged mesh gives each vertex the two heaviest bones of its part instance (TexCoord1.x and .y).
  Write(ref_set.m_Merged, MergedBatch, FOLIAGE_BONES_FLAG_VERTEX_WEIGHTS);
}

void plVegetationCellComponent::WriteActiveCompositeBones(const plVegetationCellGroup& group, CompositeSet& ref_set, const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const
{
  // Every view of a frame extracts the same set; once per frame is enough.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (ref_set.m_uiActiveWriteFrame == uiFrame || ref_set.m_ActiveSlots.IsEmpty() || m_uiFoliageBonesDataIndex == 0xFFFFFFFF)
    return;
  ref_set.m_uiActiveWriteFrame = uiFrame;

  const plUInt64 uiTypeKey = group.m_hType.GetResourceIDHash();
  plUInt32 uiMappedBatch = 0xFFFFFFFE;
  plArrayPtr<plFoliageInstanceBones> slots;

  // Grouped by batch as the full write recorded them, so each batch is mapped once.
  for (const CompositeSet::ActiveSlot& active : ref_set.m_ActiveSlots)
  {
    CompositeBatch& batch = active.m_uiBatch == MergedBatch ? ref_set.m_Merged : ref_set.m_Parts[active.m_uiBatch];
    if (active.m_uiBatch != uiMappedBatch)
    {
      uiMappedBatch = active.m_uiBatch;
      slots = {};
      if (!batch.m_BonesOffset.IsInvalidated())
        slots = msg.m_pRenderDataManager->GetOrCreateCustomInstanceData<plFoliageInstanceBones>(m_uiFoliageBonesDataIndex, this, batch.m_hBonesBuffer, batch.m_BonesOffset, batch.m_uiBonesCapacity);
    }
    if (active.m_uiSlot >= slots.GetCount() || active.m_uiSlot >= batch.m_Trees.GetCount() || batch.m_Trees[active.m_uiSlot] >= group.m_Instances.GetCount())
      continue;

    const plVegetationCellInstance& tree = group.m_Instances[batch.m_Trees[active.m_uiSlot]];
    module.FillTreeBones(uiTypeKey, tree, slots[active.m_uiSlot]);
    slots[active.m_uiSlot].Flags |= active.m_uiBatch == MergedBatch ? FOLIAGE_BONES_FLAG_VERTEX_WEIGHTS : 0u;
  }
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

    bool bDontCacheYet = false;
    const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(hMaterial, &bDontCacheYet);

    // The steady instances, then the ones dithering in a hand-off at the end of the same allocation.
    const plUInt32 uiSteady = batch.m_uiCount - plMath::Min(batch.m_uiCrossfading, batch.m_uiCount);
    for (plUInt32 uiSplit = 0; uiSplit < 2; ++uiSplit)
    {
      const plUInt32 uiStart = uiSplit == 0 ? 0 : uiSteady;
      const plUInt32 uiCount = uiSplit == 0 ? uiSteady : batch.m_uiCount - uiSteady;
      if (uiCount == 0)
        continue;

      plInstanceDataOffset offset = batch.m_Offset;
      offset.m_uiOffset += uiStart;

      plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
      // Not Dynamic: the trees stand still and only the bone shader moves them, so they keep the static categories and
      // the virtual shadow map caches their shadows instead of redrawing every clipmap level each frame.
      // The ray tracing scene gets the cell's rest-pose snapshot of the merged mesh instead (GetRTChunks).
      pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
      // Editor clicks go through vegetation to what lies beneath: a pick would redraw every part instance of the batch.
      pRenderData->m_Flags.Add(plRenderData::Flags::NotPickable);
      if (uiSplit == 1)
        pRenderData->m_Flags.Add(plRenderData::Flags::LodCrossfade);
      pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
      pRenderData->SetFallbackGlobalBounds(cellBounds);
      pRenderData->Fill(offset, batch.m_hBuffer, hMaterial, hMesh, uiMaterialIndex, uiSubMesh, uiCount);

      // Instance k reads its bones at the batch's slot + k.
      if (bHasBones)
      {
        pRenderData->m_DataOffsets.m_uiCustomInstance = batch.m_BonesOffset.m_uiOffset + uiStart;
        pRenderData->m_hCustomInstanceDataBuffer = batch.m_hBonesBuffer;
      }

      msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
    }
  }
}

void plVegetationCellComponent::ReleaseShell(const plRenderDataManager* pRenderDataManager) const
{
  if (m_Shell.m_pTask != nullptr)
    plTaskSystem::WaitForGroup(m_Shell.m_TaskGroup);

  if (pRenderDataManager != nullptr && !m_Shell.m_Offset.IsInvalidated())
    pRenderDataManager->DeleteInstanceData(m_Shell.m_Offset);

  m_Shell = Shell();
}

void plVegetationCellComponent::UpdateShell(const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const
{
  PL_IGNORE_UNUSED(msg);

  if (!cvar_VegetationCellShellEnabled || !cvar_VegetationImpostorEnabled || m_pCell == nullptr)
  {
    m_Shell.m_bReady = false;
    return;
  }

  // A finished build replaces the shell; until then the previous one (if any) keeps drawing.
  if (m_Shell.m_pTask != nullptr && plTaskSystem::IsTaskGroupFinished(m_Shell.m_TaskGroup))
  {
    plVegetationCellShellTask& task = *m_Shell.m_pTask;
    m_Shell.m_hMesh.Invalidate();
    if (task.m_bSucceeded)
    {
      plStringBuilder sId;
      sId.SetFormat("VegetationCellShell/{}_{}/{}", m_pCell->m_iX, m_pCell->m_iY, task.m_Stats.m_uiFaces);
      static plAtomicInteger32 s_iShells = 0;
      sId.AppendFormat("/{}", s_iShells.Increment());
      m_Shell.m_hMesh = plResourceManager::CreateResource<plMeshResource>(sId, std::move(task.m_Mesh), sId);
    }
    m_Shell.m_uiVersion = m_Shell.m_uiTaskVersion;
    m_Shell.m_uiTypeCounters = m_Shell.m_uiTaskTypeCounters;
    m_Shell.m_pTask.Clear();
  }

  // Only cells that reach into the shell's band keep one.
  const plBoundingBoxSphere bounds = GetOwner()->GetGlobalBounds();
  if (!bounds.IsValid())
  {
    m_Shell.m_bReady = false;
    return;
  }

  const plVec3 vCamera = module.GetMainCameraPosition();
  plVec3 corners[8];
  bounds.GetBox().GetCorners(corners);
  float fFarthestSq = 0.0f;
  for (const plVec3& vCorner : corners)
    fFarthestSq = plMath::Max(fFarthestSq, (vCorner - vCamera).GetLengthSquared());

  const float fShellStart = cvar_VegetationCellShellDistance.GetValue() - cvar_VegetationCellShellFadeDistance.GetValue();
  const bool bInReach = plMath::Sqrt(fFarthestSq) > fShellStart - 16.0f;

  // What the shell is built from: every composite type with a voxel proxy and an impostor.
  const float fDensity = plMath::Clamp(cvar_VegetationDensityScale.GetValue(), 0.0f, 1.0f);
  const float fVoxelSize = plMath::Max(cvar_VegetationCellShellVoxelSize.GetValue(), 0.1f);
  plUInt32 uiTypeCounters = plHashingUtils::xxHash32(&fDensity, sizeof(float), plHashingUtils::xxHash32(&fVoxelSize, sizeof(float)));
  for (const plVegetationCellGroup& group : m_pCell->m_Groups)
  {
    plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::PointerOnly);
    if (pType)
      uiTypeCounters = uiTypeCounters * 31 + pType->GetCurrentResourceChangeCounter();
  }

  const bool bCurrent = m_Shell.m_uiVersion == m_pCell->m_uiVersion && m_Shell.m_uiTypeCounters == uiTypeCounters;
  const bool bBuilding = m_Shell.m_pTask != nullptr;
  const bool bBuildingCurrent = bBuilding && m_Shell.m_uiTaskVersion == m_pCell->m_uiVersion && m_Shell.m_uiTaskTypeCounters == uiTypeCounters;

  if (bInReach && !bCurrent && !bBuildingCurrent && !bBuilding)
  {
    plSharedPtr<plVegetationCellShellTask> pTask = PL_DEFAULT_NEW(plVegetationCellShellTask);
    pTask->m_vOrigin = m_pCell->m_vOrigin;
    pTask->m_Settings.m_fVoxelSize = fVoxelSize;

    float fImpostorDistance = plMath::MaxValue<float>();
    float fShadowCull = 0.0f;
    plUInt32 uiMostTrees = 0;
    plMaterialResourceHandle hMaterial;
    bool bTypesReady = true;

    for (const plVegetationCellGroup& group : m_pCell->m_Groups)
    {
      plResourceLock<plVegetationTypeResource> pType(group.m_hType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
      if (pType.GetAcquireResult() != plResourceAcquireResult::Final)
      {
        bTypesReady = false;
        continue;
      }

      plSharedPtr<plVegetationVoxelProxy> pProxy = pType->GetVoxelProxy();
      if (pProxy == nullptr || pType->GetImpostorDistance() <= 0.0f)
        continue;

      bool bKnown = false;
      const plVegetationImpostorGpu* pImpostor = pType->GetImpostor(bKnown);
      if (pImpostor == nullptr)
      {
        bTypesReady &= bKnown;
        continue;
      }

      plColor seedColors[256];
      ResolveSeedColors(*pType.GetPointer(), seedColors);

      plUInt32 uiTrees = 0;
      for (const plVegetationCellInstance& inst : group.m_Instances)
      {
        if (!inst.m_Flags.IsSet(plVegetationInstanceFlags::Hero) && (inst.m_uiColorSeed + 0.5f) / 256.0f > fDensity)
          continue;

        plVegetationCellShellTree& tree = pTask->m_Trees.ExpandAndGetRef();
        tree.m_Transform.m_vPosition = inst.m_vPosition;
        tree.m_Transform.m_qRotation = inst.m_qRotation;
        tree.m_Transform.m_vScale.Set(inst.m_fScale);
        tree.m_Color = seedColors[inst.m_uiColorSeed];
        tree.m_pProxy = pProxy.Borrow();
        ++uiTrees;
      }

      pTask->m_Proxies.PushBack(pProxy);
      fImpostorDistance = plMath::Min(fImpostorDistance, pType->GetImpostorDistance() * plMath::Max(cvar_VegetationImpostorDistanceScale.GetValue(), 0.0f));
      const auto& render = pType->GetDescriptor().m_Render;
      if (render.m_bCastShadows)
        fShadowCull = plMath::Max(fShadowCull, plMath::Min(render.m_fShadowCullDistance, render.m_fCullDistance));
      if (uiTrees > uiMostTrees)
      {
        uiMostTrees = uiTrees;
        hMaterial = pImpostor->m_hCellMaterial;
      }
    }

    // A type still loading: try again next frame rather than building a shell without its trees.
    if (bTypesReady)
    {
      m_Shell.m_uiTaskVersion = m_pCell->m_uiVersion;
      m_Shell.m_uiTaskTypeCounters = uiTypeCounters;

      if (pTask->m_Trees.IsEmpty())
      {
        m_Shell.m_hMesh.Invalidate();
        m_Shell.m_uiVersion = m_pCell->m_uiVersion;
        m_Shell.m_uiTypeCounters = uiTypeCounters;
      }
      else
      {
        m_Shell.m_hMaterial = hMaterial;
        m_Shell.m_fImpostorDistance = fImpostorDistance;
        m_Shell.m_fShadowCullDistance = fShadowCull * plMath::Max(cvar_VegetationDrawDistanceScale.GetValue(), 0.0f);
        m_Shell.m_pTask = pTask;
        m_Shell.m_TaskGroup = plTaskSystem::StartSingleTask(pTask, plTaskPriority::LongRunning);
      }
    }
  }

  // Draw the shell that matches the cell. While a rebuild after an edit runs, the previous one stands in.
  m_Shell.m_bReady = m_Shell.m_hMesh.IsValid() && m_Shell.m_hMaterial.IsValid() && (bCurrent || m_Shell.m_pTask != nullptr);
}

void plVegetationCellComponent::EmitShell(const plVegetationWorldModule& module, bool bShadow, plMsgExtractRenderData& msg) const
{
  if (!m_Shell.m_bReady)
    return;

  const plBoundingBoxSphere cellBounds = GetOwner()->GetGlobalBounds();
  const plVec3 vCamera = module.GetMainCameraPosition();
  if (bShadow && cellBounds.IsValid() && cellBounds.GetBox().GetDistanceTo(vCamera) > m_Shell.m_fShadowCullDistance)
    return;

  // One instance at the cell origin, written once per frame: it carries the main camera position and the bands, which
  // every view (shadow views included) fades by.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (m_Shell.m_uiInstanceFrame != uiFrame || m_Shell.m_Offset.IsInvalidated())
  {
    plArrayPtr<plPerInstanceData> data = msg.m_pRenderDataManager->GetOrCreateInstanceData(this, true, m_Shell.m_hBuffer, m_Shell.m_Offset, 1);
    if (data.IsEmpty())
      return;
    m_Shell.m_uiInstanceFrame = uiFrame;

    const float fCellFade = plMath::Max(cvar_VegetationCellShellFadeDistance.GetValue(), 0.001f);
    const float fImpostorFade = plMath::Max(cvar_VegetationImpostorFadeDistance.GetValue(), 0.001f);
    const float fCellStart = cvar_VegetationCellShellDistance.GetValue() - fCellFade * 0.5f;
    const float fImpostorStart = m_Shell.m_fImpostorDistance - fImpostorFade * 0.5f;

    plTransform transform = plTransform::MakeIdentity();
    transform.m_vPosition = m_pCell->m_vOrigin;
    const float fRadius = cellBounds.IsValid() ? cellBounds.m_fSphereRadius : plVegetationCellSize;

    plPerInstanceData& instance = data[0];
    const plUInt32 uiSeed = (plHashingUtils::xxHash32(&m_pCell->m_vOrigin, sizeof(plVec3)) & FOLIAGE_REPRESENTATION_SEED_MASK) | (FOLIAGE_REPRESENTATION_MERGED_CELL << FOLIAGE_REPRESENTATION_SHIFT);
    plRenderDataManager::FillPerInstanceData(instance, nullptr, transform, GetUniqueIdForRendering(), plColor(m_Shell.m_fShadowCullDistance, 0.0f, 0.0f, 1.0f), plVec4(fCellStart, fCellFade, fImpostorStart, fImpostorFade), fRadius, uiSeed);
    instance.Wind = vCamera;
  }

  plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
  pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
  pRenderData->m_Flags.Add(plRenderData::Flags::NotPickable);
  pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
  pRenderData->SetFallbackGlobalBounds(cellBounds);
  pRenderData->Fill(m_Shell.m_Offset, m_Shell.m_hBuffer, m_Shell.m_hMaterial, m_Shell.m_hMesh, 0, 0, 1);

  bool bDontCacheYet = false;
  const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(m_Shell.m_hMaterial, &bDontCacheYet, plDefaultRenderDataCategories::LitMasked);
  msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
}

void plVegetationCellComponent::EmitImpostorBatch(const CompositeBatch& batch, const plVegetationImpostorGpu& impostor, const plBoundingBoxSphere& cellBounds, plMsgExtractRenderData& msg) const
{
  if (batch.m_uiCount == 0 || batch.m_Offset.IsInvalidated() || !impostor.m_hMesh.IsValid() || !impostor.m_hMaterial.IsValid())
    return;

  plMeshRenderData* pRenderData = msg.m_pRenderDataManager->CreateRenderDataForThisFrame<plMeshRenderData>(GetOwner());
  // Static like the merged mesh: the quads only turn to the view in the shader, so cached shadow pages stay valid.
  pRenderData->m_Flags.Add(plRenderData::Flags::SkipGI);
  pRenderData->m_Flags.Add(plRenderData::Flags::NotPickable);
  pRenderData->m_vGlobalPosition = cellBounds.IsValid() ? cellBounds.m_vCenter : m_pCell->m_vOrigin;
  pRenderData->SetFallbackGlobalBounds(cellBounds);
  pRenderData->Fill(batch.m_Offset, batch.m_hBuffer, impostor.m_hMaterial, impostor.m_hMesh, 0, 0, batch.m_uiCount);

  bool bDontCacheYet = false;
  const plRenderData::Category category = plMaterialResource::GetRenderDataCategory(impostor.m_hMaterial, &bDontCacheYet, plDefaultRenderDataCategories::LitMasked);
  msg.AddRenderData(pRenderData, category, plRenderData::Caching::Never);
}

bool plVegetationCellComponent::IsMergedCell(const plVegetationWorldModule& module) const
{
  PL_IGNORE_UNUSED(module);
  if (!cvar_VegetationImpostorFarCells)
  {
    m_bMergedCell = false;
    return false;
  }

  // Once per frame, so every view of a frame draws the cell the same way: whether the cell lies far enough beyond the
  // hand-off is decided per type by BuildMergedCell.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (m_uiMergedCellFrame != uiFrame)
  {
    m_uiMergedCellFrame = uiFrame;
    m_bMergedCell = GetOwner()->GetGlobalBounds().IsValid();
  }

  return m_bMergedCell;
}

bool plVegetationCellComponent::BuildMergedCell(const plVegetationCellGroup& group, CompositeState& ref_state, const plVegetationTypeResource& type, const plVegetationImpostorGpu& impostor, bool bShadow, plMsgExtractRenderData& msg) const
{
  const plVegetationWorldModule* pModule = GetWorld()->GetModuleReadOnly<plVegetationWorldModule>();
  const plBoundingBoxSphere cellBounds = GetOwner()->GetGlobalBounds();
  if (pModule == nullptr || !cellBounds.IsValid())
    return false;

  const auto& render = type.GetDescriptor().m_Render;
  const plBoundingBox box = cellBounds.GetBox();
  const plVec3 vCamera = pModule->GetMainCameraPosition();
  const float fDistanceScale = plMath::Max(cvar_VegetationDrawDistanceScale.GetValue(), 0.0f);

  // Every tree must be an impostor and drawn: the whole cell beyond the type's hand-off (with room for large trees) and
  // within its cull distance. Otherwise the group draws tree by tree.
  const float fNearest = box.GetDistanceTo(vCamera);
  plVec3 corners[8];
  box.GetCorners(corners);
  float fFarthestSq = 0.0f;
  for (const plVec3& vCorner : corners)
    fFarthestSq = plMath::Max(fFarthestSq, (vCorner - vCamera).GetLengthSquared());

  const float fFade = plMath::Max(cvar_VegetationImpostorFadeDistance.GetValue(), 0.0f);
  const float fImpostorDistance = m_Shell.m_bReady ? m_Shell.m_fImpostorDistance : type.GetImpostorDistance() * plMath::Max(cvar_VegetationImpostorDistanceScale.GetValue(), 0.0f);
  const float fHandOff = fImpostorDistance + fFade;
  if (fNearest < fHandOff + 8.0f || plMath::Sqrt(fFarthestSq) > render.m_fCullDistance * fDistanceScale)
    return false;

  if (bShadow)
  {
    // Shadows as the trees beyond the hand-off cast them: with the impostor, or (Impostor.Shadows off) tree by tree.
    if (!render.m_bCastShadows)
      return true;
    if (!cvar_VegetationImpostorShadows)
      return false;
    if (fNearest > plMath::Min(render.m_fCullDistance, render.m_fShadowCullDistance) * fDistanceScale)
      return true;
  }

  // Written once per cell version (SyncGroupStates drops the state on any change), type and density.
  const float fDensity = plMath::Clamp(cvar_VegetationDensityScale.GetValue(), 0.0f, 1.0f);
  if (!ref_state.m_bCellBuilt || ref_state.m_fCellDensity != fDensity)
  {
    ref_state.m_bCellBuilt = true;
    ref_state.m_fCellDensity = fDensity;

    plColor seedColors[256];
    ResolveSeedColors(type, seedColors);
    const float fRadius = type.GetBoundingRadius();
    const plUInt32 uiOwnerId = GetUniqueIdForRendering();

    auto Fill = [&](CompositeBatch& ref_batch, bool bShadowSet)
    {
      plUInt32 uiCount = 0;
      for (const plVegetationCellInstance& inst : group.m_Instances)
      {
        const bool bKept = inst.m_Flags.IsSet(plVegetationInstanceFlags::Hero) || (inst.m_uiColorSeed + 0.5f) / 256.0f <= fDensity;
        if (bKept && !(bShadowSet && inst.m_Flags.IsSet(plVegetationInstanceFlags::NoShadow)))
          ++uiCount;
      }

      ref_batch.m_uiCount = 0;
      if (uiCount == 0)
        return;

      if (!ref_batch.m_Offset.IsInvalidated() && ref_batch.m_uiCapacity < uiCount)
      {
        msg.m_pRenderDataManager->DeleteInstanceData(ref_batch.m_Offset);
        ref_batch.m_Offset = plInstanceDataOffset();
      }

      const plUInt32 uiCapacity = plMath::Max(uiCount, ref_batch.m_uiCapacity);
      plArrayPtr<plPerInstanceData> data = msg.m_pRenderDataManager->GetOrCreateInstanceData(this, true, ref_batch.m_hBuffer, ref_batch.m_Offset, uiCapacity);
      if (data.GetCount() < uiCount)
        return;
      ref_batch.m_uiCapacity = uiCapacity;

      plUInt32 k = 0;
      for (const plVegetationCellInstance& inst : group.m_Instances)
      {
        const bool bKept = inst.m_Flags.IsSet(plVegetationInstanceFlags::Hero) || (inst.m_uiColorSeed + 0.5f) / 256.0f <= fDensity;
        if (!bKept || (bShadowSet && inst.m_Flags.IsSet(plVegetationInstanceFlags::NoShadow)))
          continue;

        plTransform tree;
        tree.m_vPosition = inst.m_vPosition;
        tree.m_qRotation = inst.m_qRotation;
        tree.m_vScale.Set(inst.m_fScale);

        const float fTreeRadius = fRadius * inst.m_fScale;
        plRenderDataManager::FillPerInstanceData(data[k], nullptr, tree, uiOwnerId, seedColors[inst.m_uiColorSeed], plVec4(0.0f, 1.0e9f, fTreeRadius, 0.0f), fTreeRadius, MakeInstanceSeed(inst, FOLIAGE_REPRESENTATION_IMPOSTOR));
        ++k;
      }
      ref_batch.m_uiCount = k;
    };

    Fill(ref_state.m_CellMain, false);
    Fill(ref_state.m_CellShadow, true);
  }

  EmitImpostorBatch(bShadow ? ref_state.m_CellShadow : ref_state.m_CellMain, impostor, cellBounds, msg);
  return true;
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
