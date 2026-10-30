#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Interfaces/PhysicsWorldModule.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/GameObjectDesc.h>
#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Types/TagRegistry.h>
#include <Foundation/Utilities/Stats.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Lights/Implementation/VirtualShadowPool.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <RendererCore/Wind/TouchMapManager.h>
#include <VegetationPlugin/Components/VegetationCellComponent.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/Components/VegetationTouchComponent.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/Grass/GrassRenderComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

#include <Shaders/Vegetation/Grass/GrassGpuData.h>
#include <Shaders/Vegetation/Grass/GrassConstants.h>

plCVarInt cvar_VegetationGrassUploadBudgetKB("Vegetation.Grass.UploadBudgetKB", 512, plCVarFlags::Default, "Painted grass coverage each world uploads per frame, nearest tiles first. At least one tile is grown per frame");
plCVarBool cvar_VegetationTouchPhysics("Vegetation.Touch.Physics", true, plCVarFlags::Default, "Dynamic physics bodies and characters inside the touch map bend vegetation and grass without a Vegetation Touch component");
plCVarInt cvar_VegetationTouchPhysicsCollisionLayer("Vegetation.Touch.PhysicsCollisionLayer", -1, plCVarFlags::Default, "Collision layer of the physics touch query: bodies whose layer collides with it bend vegetation. -1 queries with layer 0");
plCVarInt cvar_VegetationTouchMaxPhysicsSources("Vegetation.Touch.MaxPhysicsSources", 32, plCVarFlags::Default, "Most physics bodies that bend vegetation per frame, nearest to the camera first");

// clang-format off
PL_IMPLEMENT_WORLD_MODULE(plVegetationWorldModule);

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationWorldModule, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plUInt32 plVegetationCell::GetInstanceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& group : m_Groups)
  {
    uiCount += group.m_Instances.GetCount();
  }
  return uiCount;
}

plVegetationWorldModule::plVegetationWorldModule(plWorld* pWorld)
  : plVegetationWorldModuleInterface(pWorld)
{
}

plVegetationWorldModule::~plVegetationWorldModule() = default;

void plVegetationWorldModule::Initialize()
{
  SUPER::Initialize();

  auto desc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plVegetationWorldModule::BeginFrame, this);
  desc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PreAsync;
  desc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(desc);

  // After the touch components registered their spheres (PreAsync component updates).
  auto touchDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plVegetationWorldModule::PublishTouchMap, this);
  touchDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostTransform;
  touchDesc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(touchDesc);

  // After the layer components applied their coverage changes (PreAsync component updates).
  auto grassDesc = PL_CREATE_MODULE_UPDATE_FUNCTION_DESC(plVegetationWorldModule::UpdateGrass, this);
  grassDesc.m_Phase = plWorldModule::UpdateFunctionDesc::Phase::PostTransform;
  grassDesc.m_bOnlyUpdateWhenSimulating = false;
  RegisterUpdateFunction(grassDesc);
}

void plVegetationWorldModule::Deinitialize()
{
  WorldClear();
  plVegetationTouchMapManager::RemoveWorld(GetWorld()->GetIndex());

  SUPER::Deinitialize();
}

void plVegetationWorldModule::PublishTouchMap(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);

  plVegetationTouchInput input;
  input.m_DeltaTime = GetWorld()->GetClock().GetTimeDiff();
  input.m_fRecovery = 1.5f;

  if (const plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
  {
    if (const plCamera* pCamera = pView->GetCamera())
    {
      input.m_vFocusPosition = pCamera->GetCenterPosition();
      input.m_bHasFocus = true;
    }
  }

  // Component sources come first, so they survive the touch map's source cap.
  ++m_uiPhysicsTouchFrame;
  if (input.m_bHasFocus && cvar_VegetationTouchPhysics && (m_uiTotalInstances > 0 || !m_GrassSources.IsEmpty()))
  {
    GatherPhysicsTouchSources(input.m_vFocusPosition);
  }

  // Actors not seen this frame restart their velocity when they return.
  for (auto it = m_PhysicsTouchStates.GetIterator(); it.IsValid();)
  {
    if (it.Value().m_uiLastSeenFrame != m_uiPhysicsTouchFrame)
      it = m_PhysicsTouchStates.Remove(it);
    else
      ++it;
  }

  for (const TouchSource& source : m_TouchSources)
  {
    plVegetationTouchSource& dst = input.m_Sources.ExpandAndGetRef();
    dst.m_vPosition = source.m_vPosition;
    dst.m_fRadius = source.m_fRadius;
    dst.m_vVelocity = source.m_vVelocity;
    dst.m_fStrength = source.m_fStrength;
  }

  plVegetationTouchMapManager::SubmitWorldTouch(GetWorld()->GetIndex(), input);
}

void plVegetationWorldModule::GatherPhysicsTouchSources(const plVec3& vFocus)
{
  const plPhysicsWorldModuleInterface* pPhysics = GetWorld()->GetModuleReadOnly<plPhysicsWorldModuleInterface>();
  if (pPhysics == nullptr)
    return;

  const plUInt32 uiMaxSources = static_cast<plUInt32>(plMath::Max(cvar_VegetationTouchMaxPhysicsSources.GetValue(), 0));
  if (uiMaxSources == 0)
    return;

  PL_PROFILE_SCOPE("Vegetation Physics Touch");

  // The touch map covers a square of Vegetation.Touch.MapExtent metres centred on the camera.
  static plCVarFloat* s_pMapExtent = nullptr;
  if (s_pMapExtent == nullptr)
  {
    plCVar* pCVar = plCVar::FindCVarByName("Vegetation.Touch.MapExtent");
    if (pCVar != nullptr && pCVar->GetType() == plCVarType::Float)
      s_pMapExtent = static_cast<plCVarFloat*>(pCVar);
  }
  const float fExtent = plMath::Max(s_pMapExtent != nullptr ? s_pMapExtent->GetValue() : 64.0f, 4.0f);

  const plInt32 iLayer = cvar_VegetationTouchPhysicsCollisionLayer.GetValue();
  const plUInt32 uiLayer = iLayer < 0 ? 0u : static_cast<plUInt32>(plMath::Min(iLayer, 31));
  const plBitflags<plPhysicsShapeType> shapeTypes = plPhysicsShapeType::Dynamic | plPhysicsShapeType::Character;

  plPhysicsQueryParameters params(uiLayer, shapeTypes);

  // The box reaches well below and above the camera so ground contact is found from a raised view.
  plPhysicsOverlapResultArray results;
  plTransform boxTransform = plTransform::MakeIdentity();
  boxTransform.m_vPosition = vFocus;
  pPhysics->QueryShapesInBox(results, plVec3(fExtent, fExtent, plMath::Max(fExtent, 100.0f)), boxTransform, params);

  if (results.m_Results.IsEmpty())
    return;

  struct Candidate
  {
    plVec3 m_vPosition;
    plVec3 m_vVelocity;
    float m_fRadius;
    float m_fDistanceSqr;
  };

  plHybridArray<Candidate, 32> candidates;
  const float fDeltaTime = static_cast<float>(GetWorld()->GetClock().GetTimeDiff().GetSeconds());

  for (const plPhysicsOverlapResult& hit : results.m_Results)
  {
    const plGameObjectHandle hActor = !hit.m_hActorObject.IsInvalidated() ? hit.m_hActorObject : hit.m_hShapeObject;

    plGameObject* pActor = nullptr;
    if (!GetWorld()->TryGetObject(hActor, pActor))
      continue;

    // One body yields one result per touched sub-shape.
    // States of actors missing last frame were pruned, so an existing state holds last frame's position.
    PhysicsTouchState* pState = nullptr;
    const bool bHasLast = m_PhysicsTouchStates.TryGetValue(hActor, pState);
    if (bHasLast && pState->m_uiLastSeenFrame == m_uiPhysicsTouchFrame)
      continue;

    const plVegetationTouchComponent* pTouch = nullptr;
    if (pActor->TryGetComponentOfBaseType(pTouch) && pTouch->IsActiveAndInitialized())
      continue;

    // Character controllers have no actor bounds; their object bounds or a default sphere at the origin stand in.
    plBoundingBoxSphere bounds = pPhysics->GetWorldSpaceBounds(pActor, uiLayer, shapeTypes, false);
    if (!bounds.IsValid())
      bounds = pActor->GetGlobalBounds();

    Candidate candidate;
    if (bounds.IsValid())
    {
      const plBoundingBox box = bounds.GetBox();
      const plVec3 vHalf = box.GetHalfExtents();
      candidate.m_vPosition = plVec3(box.GetCenter().x, box.GetCenter().y, box.m_vMin.z);
      candidate.m_fRadius = plMath::Clamp(plMath::Max(vHalf.x, vHalf.y), 0.1f, 4.0f);
    }
    else
    {
      candidate.m_vPosition = pActor->GetGlobalPosition();
      candidate.m_fRadius = 0.5f;
    }

    PhysicsTouchState& state = m_PhysicsTouchStates[hActor];
    candidate.m_vVelocity = (bHasLast && fDeltaTime > 0.0001f) ? (candidate.m_vPosition - state.m_vLastPosition) / fDeltaTime : plVec3::MakeZero();
    state.m_vLastPosition = candidate.m_vPosition;
    state.m_uiLastSeenFrame = m_uiPhysicsTouchFrame;

    candidate.m_fDistanceSqr = (candidate.m_vPosition - vFocus).GetLengthSquared();
    candidates.PushBack(candidate);
  }

  if (candidates.GetCount() > uiMaxSources)
  {
    candidates.Sort([](const Candidate& a, const Candidate& b)
      { return a.m_fDistanceSqr < b.m_fDistanceSqr; });
    candidates.SetCount(uiMaxSources);
  }

  for (const Candidate& candidate : candidates)
  {
    RegisterTouchSource(candidate.m_vPosition, candidate.m_fRadius, 1.0f, candidate.m_vVelocity);
  }
}

void plVegetationWorldModule::WorldClear()
{
  for (GrassLayerSource* pSource : m_GrassSources)
  {
    DestroyGrassSource(*pSource);
    PL_DEFAULT_DELETE(pSource);
  }
  m_GrassSources.Clear();

  for (auto it = m_Cells.GetIterator(); it.IsValid(); ++it)
  {
    plVegetationCell& cell = *it.Value();

    plVegetationCellComponent* pComponent = nullptr;
    if (GetWorld()->TryGetComponent(cell.m_hComponent, pComponent))
    {
      pComponent->SetCell(nullptr);
    }

    if (!cell.m_hObject.IsInvalidated())
    {
      GetWorld()->DeleteObjectNow(cell.m_hObject, false);
    }

    PL_DEFAULT_DELETE(it.Value());
  }

  m_Cells.Clear();
  m_Tiles.Clear();
  m_TouchSources.Clear();
  m_PhysicsTouchStates.Clear();
  m_uiTotalInstances = 0;
}

void plVegetationWorldModule::BeginFrame(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);

  // Touch sources re-register every frame; the touch map consumed last frame's set during rendering.
  m_TouchSources.Clear();

  plStats::SetStat("Vegetation/Instances", m_uiTotalInstances);
  plStats::SetStat("Vegetation/Cells", m_Cells.GetCount());
  plStats::SetStat("Vegetation/Tiles", m_Tiles.GetCount());
}

plUInt64 plVegetationWorldModule::MakeCellKey(plInt32 iX, plInt32 iY)
{
  return (static_cast<plUInt64>(static_cast<plUInt32>(iX)) << 32) | static_cast<plUInt32>(iY);
}

void plVegetationWorldModule::CellCoordsForPosition(const plVec3& vPosition, plInt32& out_iX, plInt32& out_iY)
{
  out_iX = static_cast<plInt32>(plMath::Floor(vPosition.x / plVegetationCellSize));
  out_iY = static_cast<plInt32>(plMath::Floor(vPosition.y / plVegetationCellSize));
}

plVegetationCell& plVegetationWorldModule::GetOrCreateCell(plInt32 iX, plInt32 iY)
{
  const plUInt64 uiKey = MakeCellKey(iX, iY);

  auto it = m_Cells.Find(uiKey);
  if (it.IsValid())
    return *it.Value();

  plVegetationCell* pCell = PL_DEFAULT_NEW(plVegetationCell);
  pCell->m_iX = iX;
  pCell->m_iY = iY;
  pCell->m_vOrigin = plVec3(iX * plVegetationCellSize, iY * plVegetationCellSize, 0.0f);

  // The cell object is internal: never serialised (it is regenerated from its sources), never picked by name.
  plGameObjectDesc desc;
  desc.m_sName.Assign("VegetationCell");
  desc.m_bDynamic = false;
  desc.m_LocalPosition = pCell->m_vOrigin;

  plGameObject* pObject = nullptr;
  pCell->m_hObject = GetWorld()->CreateObject(desc, pObject);

  // Shadow views only extract objects carrying this tag; per-instance and per-type shadow settings are applied by
  // the cell component when it fills the shadow batches.
  pObject->SetTag(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
  // Cells are static spatial objects, but wind and touch deform their shadow casters every frame.
  // Tag the generated render owner, not the layer that supplied its instances.
  pObject->SetTag(plTagRegistry::GetGlobalRegistry().RegisterTag("VSMContinuousShadow"));

  plVegetationCellComponent* pComponent = nullptr;
  pCell->m_hComponent = plVegetationCellComponent::CreateComponent(pObject, pComponent);
  pComponent->SetCell(pCell);

  m_Cells.Insert(uiKey, pCell);
  return *pCell;
}

void plVegetationWorldModule::DestroyCell(plUInt64 uiKey)
{
  auto it = m_Cells.Find(uiKey);
  if (!it.IsValid())
    return;

  plVegetationCell& cell = *it.Value();

  plVegetationCellComponent* pComponent = nullptr;
  if (GetWorld()->TryGetComponent(cell.m_hComponent, pComponent))
  {
    pComponent->SetCell(nullptr);
  }

  if (!cell.m_hObject.IsInvalidated())
  {
    GetWorld()->DeleteObjectDelayed(cell.m_hObject, false);
  }

  PL_DEFAULT_DELETE(it.Value());
  m_Cells.Remove(it);
}

void plVegetationWorldModule::NotifyCellChanged(plVegetationCell& cell)
{
  ++cell.m_uiVersion;

  plVegetationCellComponent* pComponent = nullptr;
  if (GetWorld()->TryGetComponent(cell.m_hComponent, pComponent))
  {
    pComponent->OnCellChanged();
  }
}

plVegetationTileHandle plVegetationWorldModule::AddInstances(plStringView sTypeResource, plArrayPtr<const plVegetationInstanceDesc> instances, plVegetationSource::Enum source)
{
  if (sTypeResource.IsEmpty())
    return plVegetationTileHandle();

  plVegetationTypeResourceHandle hType = plResourceManager::LoadResource<plVegetationTypeResource>(sTypeResource);
  return AddInstances(hType, instances, source);
}

plVegetationTileHandle plVegetationWorldModule::AddInstances(const plVegetationTypeResourceHandle& hType, plArrayPtr<const plVegetationInstanceDesc> instances, plVegetationSource::Enum source)
{
  if (!hType.IsValid() || instances.IsEmpty())
    return plVegetationTileHandle();

  Tile tile;
  tile.m_hType = hType;
  tile.m_Source = source;
  tile.m_uiInstanceCount = instances.GetCount();

  const plVegetationTileId tileId = m_Tiles.Insert(std::move(tile));
  Tile& storedTile = m_Tiles[tileId];

  for (plUInt32 i = 0; i < instances.GetCount(); ++i)
  {
    const plVegetationInstanceDesc& desc = instances[i];

    plInt32 iX, iY;
    CellCoordsForPosition(desc.m_vPosition, iX, iY);
    plVegetationCell& cell = GetOrCreateCell(iX, iY);

    plVegetationCellGroup* pGroup = nullptr;
    for (auto& group : cell.m_Groups)
    {
      if (group.m_hType == hType)
      {
        pGroup = &group;
        break;
      }
    }

    if (pGroup == nullptr)
    {
      pGroup = &cell.m_Groups.ExpandAndGetRef();
      pGroup->m_hType = hType;
    }

    plVegetationCellInstance& inst = pGroup->m_Instances.ExpandAndGetRef();
    inst.m_vPosition = desc.m_vPosition;
    inst.m_qRotation = desc.m_qRotation;
    inst.m_fScale = plMath::Max(desc.m_fScale, 0.001f);
    inst.m_uiColorSeed = desc.m_uiColorSeed;
    inst.m_Flags = desc.m_Flags;
    inst.m_TileId = tileId;
    inst.m_uiIndexInTile = i;

    const plUInt64 uiKey = MakeCellKey(iX, iY);
    if (!storedTile.m_Cells.Contains(uiKey))
    {
      storedTile.m_Cells.PushBack(uiKey);
    }
  }

  for (plUInt64 uiKey : storedTile.m_Cells)
  {
    NotifyCellChanged(*m_Cells[uiKey]);
  }

  m_uiTotalInstances += instances.GetCount();

  return plVegetationTileHandle(tileId);
}

void plVegetationWorldModule::RemoveTileFromCell(plVegetationCell& cell, plVegetationTileId tileId)
{
  for (plUInt32 g = cell.m_Groups.GetCount(); g > 0; --g)
  {
    auto& group = cell.m_Groups[g - 1];

    for (plUInt32 i = group.m_Instances.GetCount(); i > 0; --i)
    {
      if (group.m_Instances[i - 1].m_TileId == tileId)
      {
        group.m_Instances.RemoveAtAndSwap(i - 1);
      }
    }

    if (group.m_Instances.IsEmpty())
    {
      cell.m_Groups.RemoveAtAndCopy(g - 1);
    }
  }
}

void plVegetationWorldModule::RemoveTile(plVegetationTileHandle hTile)
{
  Tile* pTile = nullptr;
  if (!m_Tiles.TryGetValue(hTile.GetInternalID(), pTile))
    return;

  const plVegetationTileId tileId = hTile.GetInternalID();
  plHybridArray<plUInt64, 8> cells = pTile->m_Cells;
  m_uiTotalInstances -= plMath::Min(m_uiTotalInstances, pTile->m_uiInstanceCount);
  m_Tiles.Remove(tileId);

  for (plUInt64 uiKey : cells)
  {
    auto it = m_Cells.Find(uiKey);
    if (!it.IsValid())
      continue;

    plVegetationCell& cell = *it.Value();
    RemoveTileFromCell(cell, tileId);

    if (cell.m_Groups.IsEmpty())
      DestroyCell(uiKey);
    else
      NotifyCellChanged(cell);
  }
}

void plVegetationWorldModule::QueryInstances(const plBoundingSphere& sphere, plDynamicArray<plVegetationInstanceRef>& out_instances) const
{
  plInt32 iMinX, iMinY, iMaxX, iMaxY;
  CellCoordsForPosition(sphere.m_vCenter - plVec3(sphere.m_fRadius), iMinX, iMinY);
  CellCoordsForPosition(sphere.m_vCenter + plVec3(sphere.m_fRadius), iMaxX, iMaxY);

  const float fRadiusSq = sphere.m_fRadius * sphere.m_fRadius;

  for (plInt32 y = iMinY; y <= iMaxY; ++y)
  {
    for (plInt32 x = iMinX; x <= iMaxX; ++x)
    {
      auto it = m_Cells.Find(MakeCellKey(x, y));
      if (!it.IsValid())
        continue;

      for (const auto& group : it.Value()->m_Groups)
      {
        for (const auto& inst : group.m_Instances)
        {
          if ((inst.m_vPosition - sphere.m_vCenter).GetLengthSquared() > fRadiusSq)
            continue;

          plVegetationInstanceRef& ref = out_instances.ExpandAndGetRef();
          ref.m_hTile = plVegetationTileHandle(inst.m_TileId);
          ref.m_uiIndexInTile = inst.m_uiIndexInTile;
          ref.m_vPosition = inst.m_vPosition;
          ref.m_fScale = inst.m_fScale;
        }
      }
    }
  }
}

void plVegetationWorldModule::RemoveInstances(plArrayPtr<const plVegetationInstanceRef> instances)
{
  for (const plVegetationInstanceRef& ref : instances)
  {
    plInt32 iX, iY;
    CellCoordsForPosition(ref.m_vPosition, iX, iY);
    const plUInt64 uiKey = MakeCellKey(iX, iY);

    auto it = m_Cells.Find(uiKey);
    if (!it.IsValid())
      continue;

    plVegetationCell& cell = *it.Value();
    bool bRemoved = false;

    for (plUInt32 g = cell.m_Groups.GetCount(); g > 0 && !bRemoved; --g)
    {
      auto& group = cell.m_Groups[g - 1];
      for (plUInt32 i = 0; i < group.m_Instances.GetCount(); ++i)
      {
        const auto& inst = group.m_Instances[i];
        if (inst.m_TileId == ref.m_hTile.GetInternalID() && inst.m_uiIndexInTile == ref.m_uiIndexInTile)
        {
          group.m_Instances.RemoveAtAndSwap(i);
          bRemoved = true;
          break;
        }
      }

      if (group.m_Instances.IsEmpty())
      {
        cell.m_Groups.RemoveAtAndCopy(g - 1);
      }
    }

    if (!bRemoved)
      continue;

    if (m_uiTotalInstances > 0)
      --m_uiTotalInstances;

    Tile* pTile = nullptr;
    if (m_Tiles.TryGetValue(ref.m_hTile.GetInternalID(), pTile) && pTile->m_uiInstanceCount > 0)
    {
      --pTile->m_uiInstanceCount;
    }

    if (cell.m_Groups.IsEmpty())
      DestroyCell(uiKey);
    else
      NotifyCellChanged(cell);
  }
}

void plVegetationWorldModule::RegisterTouchSource(const plVec3& vPosition, float fRadius, float fStrength, const plVec3& vVelocity)
{
  TouchSource& source = m_TouchSources.ExpandAndGetRef();
  source.m_vPosition = vPosition;
  source.m_fRadius = plMath::Max(fRadius, 0.01f);
  source.m_fStrength = plMath::Clamp(fStrength, 0.0f, 1.0f);
  source.m_vVelocity = vVelocity;
}

//////////////////////////////////////////////////////////////////////////
// Painted grass

namespace
{
  // Materials/Grass/GrassBase.plMaterialAsset, used by grass types without a material.
  constexpr const char* s_szGrassLayerDefaultMaterial = "{ f4f1bf46-9929-d239-90ab-037fadeb9ecd }";

  plTagSet GrassRenderObjectTags(const plGameObject& owner, bool bCastShadows)
  {
    // Shadow views extract only tagged objects; wind and touch move the casters every frame. Without shadows the tags
    // stay off, so no shadow view ever reaches the render component.
    plTagSet tags = owner.GetTags();
    // Exclude the generated grass from GI captures, while retaining GI reception and normal shadows.
    tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("skipGI"));
    if (bCastShadows)
    {
      tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
      tags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("VSMContinuousShadow"));
    }
    else
    {
      tags.Remove(plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow"));
      tags.Remove(plTagRegistry::GetGlobalRegistry().RegisterTag("VSMContinuousShadow"));
    }
    return tags;
  }

  constexpr plUInt32 s_uiGrassTileUploadBytes = GRASS_TILE_PAYLOAD_UINTS * sizeof(plUInt32);
  constexpr float s_fGrassMinDensity = 0.25f;
  constexpr plUInt32 s_uiGrassMaxEvictionsPerTile = 16; ///< freed ranges only return pages once a page is empty

  plUInt64 GrassTileContentHash(const plGrassCoverageTile& tile)
  {
    plUInt32 uiBaseHeightBits = 0;
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&uiBaseHeightBits), reinterpret_cast<const plUInt8*>(&tile.m_fBaseHeight), sizeof(float));
    return plHashingUtils::xxHash64(tile.m_Texels, sizeof(tile.m_Texels), uiBaseHeightBits);
  }

  plBoundingBox GrassToWorld(const plBoundingBox& localBox, const plMat4& transform)
  {
    plBoundingBox box = localBox;
    if (box.IsValid())
      box.TransformFromOrigin(transform);
    return box;
  }

  void GrassExpand(plBoundingBox& inout_box, const plBoundingBox& add)
  {
    if (!add.IsValid())
      return;

    if (inout_box.IsValid())
      inout_box.ExpandToInclude(add);
    else
      inout_box = add;
  }
} // namespace

void plVegetationWorldModule::UpdateGrass(const plWorldModule::UpdateContext& context)
{
  PL_IGNORE_UNUSED(context);
  PL_PROFILE_SCOPE("Vegetation.UpdateGrass");

  for (GrassLayerSource* pSource : m_GrassSources)
  {
    pSource->m_bSeen = false;
    pSource->m_pEntry = nullptr;
  }

  const plUInt32 uiWorldIndex = GetWorld()->GetIndex();

  if (plVegetationLayerComponentManager* pManager = GetWorld()->GetComponentManager<plVegetationLayerComponentManager>())
  {
    for (auto it = pManager->GetComponents(); it.IsValid(); ++it)
    {
      const plVegetationLayerComponent& layer = *it;
      if (!layer.IsActiveAndInitialized())
        continue;

      const plGrassCoverageData& coverage = layer.GetGrassCoverage();
      const plUInt32 uiCounter = layer.GetGrassCoverageChangeCounter();
      const plComponentHandle hLayer = layer.GetHandle();

      for (const plGrassCoverageEntry& entry : coverage.m_Entries)
      {
        if (entry.m_sType.IsEmpty() || entry.m_Tiles.IsEmpty())
          continue;

        const plUInt32 uiSeed = plGrassCoverageData::GetTypeSeed(entry.m_sType);

        GrassLayerSource* pSource = nullptr;
        for (GrassLayerSource* pCandidate : m_GrassSources)
        {
          if (pCandidate->m_hLayer == hLayer && pCandidate->m_uiTypeSeed == uiSeed)
          {
            pSource = pCandidate;
            break;
          }
        }

        if (pSource == nullptr)
        {
          pSource = PL_DEFAULT_NEW(GrassLayerSource);
          pSource->m_hLayer = hLayer;
          pSource->m_sType = entry.m_sType;
          pSource->m_uiTypeSeed = uiSeed;
          m_GrassSources.PushBack(pSource);
        }

        GrassLayerSource& source = *pSource;
        source.m_bSeen = true;

        if (!PrepareGrassSource(source, layer))
          continue;

        source.m_pEntry = &entry;
        source.m_fTexelSize = coverage.m_fTexelSize;
        source.m_LayerTransform = layer.GetOwner()->GetGlobalTransform();
        IndexGrassTiles(source, coverage, entry, uiCounter);
        plGrassGpuManager::SetSourceTransform(uiWorldIndex, source.m_hSource, source.m_LayerTransform);
      }
    }
  }

  for (plUInt32 i = m_GrassSources.GetCount(); i > 0; --i)
  {
    GrassLayerSource* pSource = m_GrassSources[i - 1];
    if (pSource->m_bSeen)
      continue;

    DestroyGrassSource(*pSource, false);
    PL_DEFAULT_DELETE(pSource);
    m_GrassSources.RemoveAtAndSwap(i - 1);
  }

  m_uiGrassResidentTiles = 0;
  m_uiGrassPendingTiles = 0;

  if (!m_GrassSources.IsEmpty())
  {
    UpdateGrassCameras();
    StreamGrassTiles();
    UpdateGrassBudgetThinning();

    for (GrassLayerSource* pSource : m_GrassSources)
    {
      plVegetationLayerComponent* pLayer = nullptr;
      if (pSource->m_pEntry == nullptr || !GetWorld()->TryGetComponent(pSource->m_hLayer, pLayer))
        continue;

      UpdateGrassRenderObject(*pSource, *pLayer);

      // Growing and freeing tiles changes the casters inside an unchanged footprint too.
      if (pSource->m_ChangedCasterBox.IsValid())
      {
        if (pSource->m_bCastsShadows)
          plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), pSource->m_ChangedCasterBox);
        pSource->m_ChangedCasterBox = plBoundingBox::MakeInvalid();
      }
    }

    plVec3 vCamera = plVec3::MakeZero();
    bool bHasCamera = false;
    if (const plView* pView = plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()))
    {
      if (const plCamera* pCamera = pView->GetCamera())
      {
        vCamera = pCamera->GetCenterPosition();
        bHasCamera = true;
      }
    }
    plGrassGpuManager::SubmitWorldFrame(uiWorldIndex, vCamera, bHasCamera, GetWorld()->GetClock().GetTimeDiff(), GetWorld()->GetWorldSimulationEnabled());
  }
  else
  {
    m_GrassCameras.Clear();
    m_bGrassRefusing = false;
    m_bGrassPressure = false;
  }

  plStats::SetStat("Vegetation/GrassSources", m_GrassSources.GetCount());
  plStats::SetStat("Vegetation/Grass/ResidentTiles", m_uiGrassResidentTiles);
  plStats::SetStat("Vegetation/Grass/PendingTiles", m_uiGrassPendingTiles);
  plStats::SetStat("Vegetation/Grass/BudgetThinning", m_bGrassPressure);
}

void plVegetationWorldModule::UpdateGrassCameras()
{
  const plTime now = plTime::Now();

  auto addView = [&](const plView* pView)
  {
    if (pView == nullptr || pView->GetWorld() != GetWorld() || pView->GetCamera() == nullptr)
      return;

    if (pView->GetCameraUsageHint() != plCameraUsageHint::MainView && pView->GetCameraUsageHint() != plCameraUsageHint::EditorView)
      return;

    const plViewHandle hView = pView->GetHandle();
    const plVec3 vPosition = pView->GetCamera()->GetCenterPosition();
    for (GrassCamera& camera : m_GrassCameras)
    {
      if (camera.m_hView == hView)
      {
        camera.m_vPosition = vPosition;
        camera.m_LastSeen = now;
        return;
      }
    }

    GrassCamera& camera = m_GrassCameras.ExpandAndGetRef();
    camera.m_hView = hView;
    camera.m_vPosition = vPosition;
    camera.m_LastSeen = now;
  };

  for (const plViewHandle& hView : plRenderWorld::GetMainViews())
  {
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(hView, pView))
      addView(pView);
  }
  addView(plRenderWorld::GetViewByUsageHint(plCameraUsageHint::MainView, plCameraUsageHint::EditorView, GetWorld()));

  // Editor viewports redraw on demand, so a view keeps its tiles for a while after it last rendered.
  for (plUInt32 i = m_GrassCameras.GetCount(); i > 0; --i)
  {
    if (now - m_GrassCameras[i - 1].m_LastSeen > plTime::MakeFromSeconds(5.0))
      m_GrassCameras.RemoveAtAndSwap(i - 1);
  }
}

void plVegetationWorldModule::IndexGrassTiles(GrassLayerSource& ref_source, const plGrassCoverageData& coverage, const plGrassCoverageEntry& entry, plUInt32 uiCoverageCounter)
{
  if (ref_source.m_bIndexed && ref_source.m_fIndexedTexelSize != coverage.m_fTexelSize)
  {
    // Every tile covers a different area after a regrid.
    for (auto it = ref_source.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_bResident)
        FreeGrassTile(ref_source, it.Key(), it.Value());
    }
    ref_source.m_Tiles.Clear();
    ref_source.m_bIndexed = false;
  }

  if (ref_source.m_bIndexed && uiCoverageCounter == ref_source.m_uiCoverageCounter)
    return;

  if (ref_source.m_bIndexed && uiCoverageCounter == ref_source.m_uiCoverageCounter + 1 && !coverage.GetLastChangeWasFull())
  {
    // One brush stroke: only the tiles it touched.
    for (const plGrassCoverageTileId& id : coverage.GetLastChangedTiles())
    {
      if (id.m_uiTypeSeed == ref_source.m_uiTypeSeed)
        IndexGrassTile(ref_source, entry, id.m_uiTileKey);
    }
  }
  else
  {
    // Unknown changes: the content hash finds the tiles that differ.
    for (auto it = ref_source.m_Tiles.GetIterator(); it.IsValid(); ++it)
      it.Value().m_bSeen = false;

    for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
      IndexGrassTile(ref_source, entry, it.Key());

    for (auto it = ref_source.m_Tiles.GetIterator(); it.IsValid();)
    {
      if (it.Value().m_bSeen)
      {
        ++it;
        continue;
      }

      if (it.Value().m_bResident)
        FreeGrassTile(ref_source, it.Key(), it.Value());
      it = ref_source.m_Tiles.Remove(it);
    }
  }

  ref_source.m_bIndexed = true;
  ref_source.m_fIndexedTexelSize = coverage.m_fTexelSize;
  ref_source.m_uiCoverageCounter = uiCoverageCounter;
}

void plVegetationWorldModule::IndexGrassTile(GrassLayerSource& ref_source, const plGrassCoverageEntry& entry, plUInt32 uiTileKey)
{
  const plGrassCoverageTile* pTile = entry.m_Tiles.GetValue(uiTileKey);
  if (pTile == nullptr || pTile->IsEmpty())
  {
    if (GrassTileState* pState = ref_source.m_Tiles.GetValue(uiTileKey))
    {
      if (pState->m_bResident)
        FreeGrassTile(ref_source, uiTileKey, *pState);
      ref_source.m_Tiles.Remove(uiTileKey);
    }
    return;
  }

  bool bExisted = false;
  GrassTileState& state = ref_source.m_Tiles.FindOrAdd(uiTileKey, &bExisted);
  state.m_bSeen = true;

  const plUInt64 uiHash = GrassTileContentHash(*pTile);
  if (bExisted && state.m_uiContentHash == uiHash)
    return;

  state.m_uiContentHash = uiHash;
  state.m_bDirty = true;

  float fMinHeight = plMath::MaxValue<float>();
  float fMaxHeight = -plMath::MaxValue<float>();
  for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
  {
    if (pTile->m_Texels[i].m_uiDensity == 0)
      continue;
    fMinHeight = plMath::Min(fMinHeight, pTile->GetTexelHeight(i));
    fMaxHeight = plMath::Max(fMaxHeight, pTile->GetTexelHeight(i));
  }

  const plGrassSettings& s = ref_source.m_Settings;
  float fAtlasAspect = 1.0f;
  float fAtlasSize = 1.0f;
  for (const plGrassAtlasRect& rect : s.m_AtlasRects)
  {
    fAtlasSize = plMath::Max(fAtlasSize, plMath::Clamp(rect.m_fSize, 0.0f, 10.0f));
    if (plMath::Abs(rect.m_vTexMulAdd.y) > 0.00001f)
      fAtlasAspect = plMath::Max(fAtlasAspect, plMath::Abs(rect.m_vTexMulAdd.x / rect.m_vTexMulAdd.y));
  }

  // Covers every constrained pose, card width and camera bend of the tile's strands.
  const float fLength = s.m_fLength * fAtlasSize * (1.0f + s.m_fRandomness);
  const float fRadius = fLength * (s.m_bProceduralBlades ? (3.0f + s.m_fWidth * s.m_fBladeWidth * 3.0f) : (1.8f + s.m_fWidth * fAtlasAspect * ref_source.m_fTextureAspect * 0.5f)) + (s.m_bProceduralBlades ? s.m_fGroundCoverMaxSize : 0.0f);
  const float fTileSize = ref_source.m_fTexelSize * plGrassCoverageTile::TexelsPerSide;
  state.m_Bounds = plBoundingBox::MakeFromMinMax(plVec3(pTile->m_iX * fTileSize, pTile->m_iY * fTileSize, fMinHeight), plVec3((pTile->m_iX + 1) * fTileSize, (pTile->m_iY + 1) * fTileSize, fMaxHeight));
  state.m_Bounds.Grow(plVec3(fRadius));

  if (state.m_bResident)
    ref_source.m_bBoundsDirty = true;
}

void plVegetationWorldModule::FreeGrassTile(GrassLayerSource& ref_source, plUInt32 uiTileKey, GrassTileState& ref_tile)
{
  plGrassGpuManager::RemoveCoverageTile(GetWorld()->GetIndex(), ref_source.m_hSource, uiTileKey);
  AddGrassCasterChange(ref_source, ref_tile);
  ref_tile.m_bResident = false;
  ref_tile.m_fDensity = 1.0f;
  ref_source.m_bBoundsDirty = true;

  // The freed strands may make room for a refused tile.
  m_bGrassRefusing = false;
}

bool plVegetationWorldModule::EvictFarthestGrassTile(float fFartherThan)
{
  GrassLayerSource* pVictimSource = nullptr;
  plUInt32 uiVictimKey = 0;
  GrassTileState* pVictim = nullptr;
  float fVictimDistance = fFartherThan;

  for (GrassLayerSource* pSource : m_GrassSources)
  {
    if (pSource->m_hSource.IsInvalidated())
      continue;

    for (auto it = pSource->m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      GrassTileState& tile = it.Value();
      if (tile.m_bResident && tile.m_fDistance > fVictimDistance)
      {
        pVictimSource = pSource;
        uiVictimKey = it.Key();
        pVictim = &tile;
        fVictimDistance = tile.m_fDistance;
      }
    }
  }

  if (pVictim == nullptr)
    return false;

  FreeGrassTile(*pVictimSource, uiVictimKey, *pVictim);
  return true;
}

void plVegetationWorldModule::StreamGrassTiles()
{
  PL_PROFILE_SCOPE("Vegetation.StreamGrass");

  const plUInt32 uiWorldIndex = GetWorld()->GetIndex();

  plUInt32 uiUsedPages = 0;
  plUInt32 uiBudgetPages = 0;
  plGrassGpuManager::GetPoolUsage(uiWorldIndex, uiUsedPages, uiBudgetPages);
  if (uiBudgetPages != m_uiGrassBudgetPages)
  {
    m_uiGrassBudgetPages = uiBudgetPages;
    m_bGrassRefusing = false;
  }
  if (m_bGrassPressure && uiUsedPages * 10 <= uiBudgetPages * 9)
    m_bGrassPressure = false;

  struct Candidate
  {
    PL_DECLARE_POD_TYPE();
    GrassLayerSource* m_pSource;
    plUInt32 m_uiTileKey;
    float m_fDistance;
    plUInt32 m_uiFullCount = 0;
  };
  plDynamicArray<Candidate> candidates;

  // Without a camera residency stays as it is and only resident tiles regrow.
  const bool bHasCamera = !m_GrassCameras.IsEmpty();

  for (GrassLayerSource* pSource : m_GrassSources)
  {
    GrassLayerSource& source = *pSource;
    if (source.m_pEntry == nullptr || source.m_hSource.IsInvalidated())
      continue;

    const plMat4 transform = source.m_LayerTransform.GetAsMat4();
    const float fTileSize = source.m_fTexelSize * plGrassCoverageTile::TexelsPerSide;
    const float fMargin = plMath::Max(8.0f, fTileSize);
    const float fStreamIn = source.m_Settings.m_fViewDistance + fMargin;
    const float fStreamOut = source.m_Settings.m_fViewDistance + 2.0f * fMargin;

    for (auto it = source.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      GrassTileState& tile = it.Value();
      if (bHasCamera)
      {
        const plBoundingBox worldBox = GrassToWorld(tile.m_Bounds, transform);
        float fDistance = plMath::MaxValue<float>();
        for (const GrassCamera& camera : m_GrassCameras)
          fDistance = plMath::Min(fDistance, worldBox.GetDistanceTo(camera.m_vPosition));
        tile.m_fDistance = fDistance;
      }

      // Keep more roots than rendering needs, using the nearest point of the tile and a camera-motion margin.
      // Power-of-two subsets are nested. Hysteresis delays downsampling but never delays restoring detail.
      plUInt32 uiStride = 1;
      if (source.m_Settings.m_bProceduralBlades && source.m_Settings.m_bAdaptiveDensity)
      {
        const float fStart = source.m_Settings.m_fLodStartDistance;
        const float fDistance = plMath::Max(fStart, tile.m_fDistance - fMargin);
        const float fKeep = plMath::Max(source.m_Settings.m_fFarDensity, (fStart * fStart) / (fDistance * fDistance));
        while (uiStride < 256 && float(uiStride * 2) * fKeep <= 1.0f)
          uiStride *= 2;
        if (tile.m_bResident && uiStride > tile.m_uiSampleStride && float(tile.m_uiSampleStride * 2) * fKeep > 0.8f)
          uiStride = tile.m_uiSampleStride;
      }
      tile.m_uiRequestedStride = uiStride;

      if (tile.m_bResident)
      {
        if (bHasCamera && tile.m_fDistance > fStreamOut)
          FreeGrassTile(source, it.Key(), tile);
        else
          candidates.PushBack({pSource, it.Key(), tile.m_fDistance});
      }
      else if (bHasCamera && tile.m_fDistance <= fStreamIn)
      {
        candidates.PushBack({pSource, it.Key(), tile.m_fDistance});
      }
    }
  }

  // Plan all in-range tiles together. Near-detail upgrades must leave space for distant coverage, not consume the
  // pool and evict the roots that the far-cover shader needs. Keep headroom for replacing an existing tile's range.
  for (Candidate& candidate : candidates)
  {
    const auto& source = *candidate.m_pSource;
    const auto& coverage = *source.m_pEntry->m_Tiles.GetValue(candidate.m_uiTileKey);
    const plUInt32 perTexel = GrassStrandsPerTexelQ8(source.m_Settings.m_fStrandsPerSquareMeter, source.m_fTexelSize);
    for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      candidate.m_uiFullCount += GrassStrandCountForTexelQ8(coverage.m_Texels[i].m_uiDensity, perTexel, GrassHash(coverage.m_iX, coverage.m_iY, i, source.m_uiTypeSeed));
  }
  const plUInt64 targetRoots = plUInt64(uiBudgetPages) * GRASS_PAGE_SIZE * 4 / 5;
  for (;;)
  {
    plUInt64 requestedRoots = 0;
    for (const Candidate& candidate : candidates)
    {
      const auto& tile = candidate.m_pSource->m_Tiles[candidate.m_uiTileKey];
      requestedRoots += (plUInt64(candidate.m_uiFullCount) + tile.m_uiRequestedStride - 1) / tile.m_uiRequestedStride;
    }
    if (requestedRoots <= targetRoots)
      break;
    bool reduced = false;
    for (const Candidate& candidate : candidates)
    {
      auto& source = *candidate.m_pSource;
      auto& tile = source.m_Tiles[candidate.m_uiTileKey];
      if (source.m_Settings.m_bProceduralBlades && source.m_Settings.m_bAdaptiveDensity && tile.m_uiRequestedStride < 256)
      {
        tile.m_uiRequestedStride *= 2;
        reduced = true;
      }
    }
    if (!reduced)
      break;
  }
  for (const Candidate& candidate : candidates)
  {
    auto& tile = candidate.m_pSource->m_Tiles[candidate.m_uiTileKey];
    // Only the final budgeted stride determines whether existing roots need regeneration.
    if (tile.m_uiRequestedStride != tile.m_uiSampleStride)
      tile.m_bDirty = true;
  }
  candidates.Sort([](const Candidate& a, const Candidate& b)
    {
      const auto& ta = a.m_pSource->m_Tiles[a.m_uiTileKey];
      const auto& tb = b.m_pSource->m_Tiles[b.m_uiTileKey];
      const bool shrinkA = ta.m_bResident && ta.m_uiRequestedStride > ta.m_uiSampleStride;
      const bool shrinkB = tb.m_bResident && tb.m_uiRequestedStride > tb.m_uiSampleStride;
      return shrinkA != shrinkB ? shrinkA : a.m_fDistance < b.m_fDistance;
    });

  const plUInt32 uiBudgetBytes = static_cast<plUInt32>(plMath::Max(cvar_VegetationGrassUploadBudgetKB.GetValue(), 0)) * 1024u;
  plUInt32 uiSpentBytes = 0;
  plUInt32 uiProcessed = 0;

  for (const Candidate& candidate : candidates)
  {
    if (uiSpentBytes > 0 && uiSpentBytes + s_uiGrassTileUploadBytes > uiBudgetBytes)
      break;

    ++uiProcessed;

    GrassLayerSource& source = *candidate.m_pSource;
    GrassTileState* pTile = source.m_Tiles.GetValue(candidate.m_uiTileKey);
    const plGrassCoverageTile* pCoverage = source.m_pEntry->m_Tiles.GetValue(candidate.m_uiTileKey);
    if (pTile == nullptr || pCoverage == nullptr)
      continue;
    if (pTile->m_bResident && !pTile->m_bDirty)
      continue;

    // Pool pressure must not pin yesterday's resident set as the camera moves. A missing nearby tile may replace a
    // farther resident even while the pool is full. Only refuse it when no lower-priority resident can make room.
    if (!pTile->m_bResident && m_bGrassRefusing && !EvictFarthestGrassTile(candidate.m_fDistance))
      continue;

    AddGrassCasterChange(source, *pTile);
    source.m_bBoundsDirty = true;

    bool bGrown = plGrassGpuManager::UploadCoverageTile(uiWorldIndex, source.m_hSource, *pCoverage, source.m_uiTypeSeed, source.m_fTexelSize, pTile->m_uiRequestedStride).Succeeded();

    // A full pool gives up the farthest resident tiles that lie beyond this one, until it fits.
    for (plUInt32 uiEvictions = 0; !bGrown && bHasCamera && uiEvictions < s_uiGrassMaxEvictionsPerTile && EvictFarthestGrassTile(candidate.m_fDistance); ++uiEvictions)
    {
      bGrown = plGrassGpuManager::UploadCoverageTile(uiWorldIndex, source.m_hSource, *pCoverage, source.m_uiTypeSeed, source.m_fTexelSize, pTile->m_uiRequestedStride).Succeeded();
    }

    if (bGrown)
    {
      pTile->m_bResident = true;
      pTile->m_bDirty = false;
      pTile->m_uiSampleStride = pTile->m_uiRequestedStride;
      // Downsampling releases root ranges even though this tile stays resident.
      m_bGrassRefusing = false;
      uiSpentBytes += s_uiGrassTileUploadBytes;
    }
    else
    {
      // Still full: a resident tile keeps its previous strands and stays dirty, so it regrows once room frees up.
      m_bGrassRefusing = true;
      m_bGrassPressure = true;
    }
  }

  // Pending: candidates past the upload budget, and processed ones that are still missing or outdated.
  m_uiGrassPendingTiles = candidates.GetCount() - uiProcessed;
  for (plUInt32 i = 0; i < uiProcessed; ++i)
  {
    const GrassTileState* pTile = candidates[i].m_pSource->m_Tiles.GetValue(candidates[i].m_uiTileKey);
    if (pTile != nullptr && (!pTile->m_bResident || pTile->m_bDirty))
      ++m_uiGrassPendingTiles;
  }

  for (const GrassLayerSource* pSource : m_GrassSources)
  {
    for (auto it = pSource->m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_bResident)
        ++m_uiGrassResidentTiles;
    }
  }
}

void plVegetationWorldModule::UpdateGrassBudgetThinning()
{
  const plUInt32 uiWorldIndex = GetWorld()->GetIndex();

  struct ResidentTile
  {
    PL_DECLARE_POD_TYPE();
    GrassLayerSource* m_pSource;
    plUInt32 m_uiTileKey;
    float m_fRelativeDistance;
  };
  plDynamicArray<ResidentTile> tiles;

  for (GrassLayerSource* pSource : m_GrassSources)
  {
    if (pSource->m_pEntry == nullptr || pSource->m_hSource.IsInvalidated())
      continue;

    const float fViewDistance = plMath::Max(pSource->m_Settings.m_fViewDistance, 1.0f);
    for (auto it = pSource->m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const GrassTileState& tile = it.Value();
      if (tile.m_bResident && (m_bGrassPressure || tile.m_fDensity < 1.0f))
        tiles.PushBack({pSource, it.Key(), tile.m_fDistance / fViewDistance});
    }
  }

  // Under pressure the farthest quarter of the resident tiles (relative to each type's view distance) thins from full
  // density down to s_fGrassMinDensity at the farthest tile. Every other tile draws at full density.
  plUInt32 uiThinned = 0;
  if (m_bGrassPressure)
  {
    tiles.Sort([](const ResidentTile& a, const ResidentTile& b) { return a.m_fRelativeDistance > b.m_fRelativeDistance; });
    uiThinned = (tiles.GetCount() + 3) / 4;
  }

  for (plUInt32 i = 0; i < tiles.GetCount(); ++i)
  {
    const float fDensity = i < uiThinned ? plMath::Lerp(s_fGrassMinDensity, 1.0f, float(i) / float(uiThinned)) : 1.0f;

    // Compared in the manager's unorm8 steps.
    GrassTileState* pTile = tiles[i].m_pSource->m_Tiles.GetValue(tiles[i].m_uiTileKey);
    if (pTile == nullptr || plMath::Round(pTile->m_fDensity * 255.0f) == plMath::Round(fDensity * 255.0f))
      continue;

    pTile->m_fDensity = fDensity;
    plGrassGpuManager::SetCoverageTileDensity(uiWorldIndex, tiles[i].m_pSource->m_hSource, tiles[i].m_uiTileKey, fDensity);
  }
}

bool plVegetationWorldModule::PrepareGrassSource(GrassLayerSource& ref_source, const plVegetationLayerComponent& layer)
{
  if (!ref_source.m_hType.IsValid())
    ref_source.m_hType = plResourceManager::LoadResource<plGrassTypeResource>(ref_source.m_sType);

  plResourceLock<plGrassTypeResource> pType(ref_source.m_hType, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
  if (pType.GetAcquireResult() != plResourceAcquireResult::Final)
    return !ref_source.m_hSource.IsInvalidated();

  plMaterialResourceHandle hMaterial = pType->GetMaterial();
  if (!hMaterial.IsValid())
    hMaterial = plResourceManager::LoadResource<plMaterialResource>(s_szGrassLayerDefaultMaterial);

  // Card width follows the base texture's aspect; it is only known once the texture has loaded.
  float fAspect = ref_source.m_fTextureAspect;
  {
    plResourceLock<plMaterialResource> pMaterial(hMaterial, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
    if (pMaterial.GetAcquireResult() == plResourceAcquireResult::Final)
    {
      const plTexture2DResourceHandle hTexture = pMaterial->GetTexture2DBinding("BaseTexture");
      if (hTexture.IsValid())
      {
        plResourceLock<plTexture2DResource> pTexture(hTexture, plResourceAcquireMode::AllowLoadingFallback_NeverFail);
        if (pTexture.GetAcquireResult() == plResourceAcquireResult::Final)
          fAspect = float(pTexture->GetWidth()) / plMath::Max(1u, pTexture->GetHeight());
      }
    }
  }

  const plUInt32 uiTypeCounter = pType->GetCurrentResourceChangeCounter();
  if (ref_source.m_hSource.IsInvalidated() || uiTypeCounter != ref_source.m_uiTypeChangeCounter || fAspect != ref_source.m_fTextureAspect || hMaterial != ref_source.m_hMaterial)
  {
    const plUInt32 uiWorldIndex = GetWorld()->GetIndex();

    // Growth values and the card aspect shape the strands and their bounds; everything else is read from the source
    // table every frame, so a type edit that leaves them alone keeps the grown strands (and the shadow caches).
    plGrassSettings newSettings = pType->GetSettings();
    newSettings.Sanitize();
    plGrassSettings oldSettings = ref_source.m_Settings;
    oldSettings.Sanitize();
    const bool bRegrow = ref_source.m_hSource.IsInvalidated() || fAspect != ref_source.m_fTextureAspect || newSettings.RequiresRegrow(oldSettings) ||
                         plGrassGpuManager::UpdateSourceSettings(uiWorldIndex, ref_source.m_hSource, newSettings, fAspect).Failed();

    ref_source.m_Settings = pType->GetSettings();
    ref_source.m_hMaterial = hMaterial;
    ref_source.m_uiTypeChangeCounter = uiTypeCounter;
    ref_source.m_fTextureAspect = fAspect;

    if (bRegrow)
    {
      plGrassGpuManager::Free(uiWorldIndex, ref_source.m_hSource);
      ref_source.m_hSource = plGrassGpuManager::AllocateSource(uiWorldIndex, ref_source.m_Settings, layer.GetOwner()->GetGlobalTransform(), fAspect);
      // The new source owns no strands: every tile is indexed again and streams in anew.
      ref_source.m_bIndexed = false;
      ref_source.m_Tiles.Clear();
      ref_source.m_bBoundsDirty = true;
      m_bGrassRefusing = false;
    }
  }

  return !ref_source.m_hSource.IsInvalidated();
}

void plVegetationWorldModule::UpdateGrassRenderObject(GrassLayerSource& ref_source, const plVegetationLayerComponent& layer)
{
  if (ref_source.m_bBoundsDirty)
  {
    ref_source.m_RenderBounds = plBoundingBox::MakeInvalid();
    for (auto it = ref_source.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_bResident)
        GrassExpand(ref_source.m_RenderBounds, it.Value().m_Bounds);
    }
    ref_source.m_bBoundsDirty = false;
  }

  const plBoundingBox& bounds = ref_source.m_RenderBounds;
  const plGameObject* pOwner = layer.GetOwner();
  const bool bCastsShadows = ref_source.m_Settings.m_bCastShadows && plGrassGpuManager::AreShadowsEnabled();

  plGameObject* pObject = nullptr;
  plGrassRenderComponent* pComponent = nullptr;
  if (!GetWorld()->TryGetObject(ref_source.m_hRenderObject, pObject) || !GetWorld()->TryGetComponent(ref_source.m_hRenderComponent, pComponent))
  {
    // Internal child of the layer: follows its transform, never serialized (it is regenerated from the coverage).
    plGameObjectDesc desc;
    desc.m_sName.Assign("Grass Render");
    desc.m_hParent = pOwner->GetHandle();
    desc.m_bDynamic = pOwner->IsDynamic();
    desc.m_Tags = GrassRenderObjectTags(*pOwner, bCastsShadows);
    desc.m_uiTeamID = pOwner->GetTeamID();
    ref_source.m_hRenderObject = GetWorld()->CreateObject(desc, pObject);
    pObject->SetCreatedByPrefab();

    ref_source.m_hRenderComponent = plGrassRenderComponent::CreateComponent(pObject, pComponent);
    pComponent->SetCreatedByPrefab();
  }

  const plTagSet tags = GrassRenderObjectTags(*pOwner, bCastsShadows);
  if (pObject->GetTags() != tags)
    pObject->SetTags(tags);

  pComponent->SetSource(ref_source.m_hSource, ref_source.m_hMaterial, plRenderComponent::GetUniqueIdForRendering(layer), ref_source.m_Settings.m_fViewDistance, ref_source.m_Settings.m_fShadowDistance, ref_source.m_Settings.m_bCastShadows);
  pComponent->SetLocalBounds(bounds);

  // Grown and freed tiles invalidate their own boxes; a moved layer moves every caster. A shadow setting change
  // changes what the source contributes over its whole footprint. Without shadows nothing of it is cached.
  const plTransform transform = pOwner->GetGlobalTransform();
  const plBoundingBox worldBox = GrassToWorld(bounds, transform.GetAsMat4());
  if (bCastsShadows != ref_source.m_bCastsShadows)
  {
    plBoundingBox invalidate = ref_source.m_LastCasterBox;
    GrassExpand(invalidate, worldBox);
    if (invalidate.IsValid())
      plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), invalidate);
    ref_source.m_bCastsShadows = bCastsShadows;
  }
  else if (bCastsShadows && ref_source.m_LastCasterBox.IsValid() && !transform.IsEqual(ref_source.m_LastCasterTransform, 0.0001f))
  {
    plBoundingBox invalidate = ref_source.m_LastCasterBox;
    GrassExpand(invalidate, worldBox);
    plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), invalidate);
  }
  ref_source.m_LastCasterBox = worldBox;
  ref_source.m_LastCasterTransform = transform;
}

void plVegetationWorldModule::AddGrassCasterChange(GrassLayerSource& ref_source, const GrassTileState& tile)
{
  // Strands beyond Shadow Distance cast nothing, so far tiles streaming in and out leave the shadow caches alone.
  const float fMargin = plMath::Max(8.0f, ref_source.m_fTexelSize * plGrassCoverageTile::TexelsPerSide);
  if (tile.m_fDistance > ref_source.m_Settings.m_fShadowDistance + fMargin)
    return;

  GrassExpand(ref_source.m_ChangedCasterBox, GrassToWorld(tile.m_Bounds, ref_source.m_LayerTransform.GetAsMat4()));
}

void plVegetationWorldModule::DestroyGrassSource(GrassLayerSource& ref_source, bool bDeleteObjectNow)
{
  plGrassGpuManager::Free(GetWorld()->GetIndex(), ref_source.m_hSource);

  if (ref_source.m_LastCasterBox.IsValid())
  {
    plVirtualShadowPool::InvalidateStaticCasters(GetWorld(), ref_source.m_LastCasterBox);
    ref_source.m_LastCasterBox = plBoundingBox::MakeInvalid();
  }

  plGameObject* pObject = nullptr;
  if (GetWorld()->TryGetObject(ref_source.m_hRenderObject, pObject))
  {
    if (bDeleteObjectNow)
    {
      GetWorld()->DeleteObjectNow(ref_source.m_hRenderObject, false);
    }
    else
    {
      pObject->SetActiveFlag(false);
      GetWorld()->DeleteObjectDelayed(ref_source.m_hRenderObject, false);
    }
  }
  ref_source.m_hRenderObject.Invalidate();
  ref_source.m_hRenderComponent.Invalidate();
}

void plVegetationWorldModule::ReportMainCamera(const plVec3& vPosition, float fTanHalfFovY) const
{
  m_vMainCameraPosition = vPosition;
  m_fMainCameraTanHalfFov = plMath::Max(fTanHalfFovY, 0.01f);
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_World_VegetationWorldModule);
