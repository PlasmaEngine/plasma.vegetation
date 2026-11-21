#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Algorithm/HashingUtils.h>
#include <VegetationPlugin/World/VegetationCompositeWind.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

//////////////////////////////////////////////////////////////////////////
// plVegetationCompositeWindTiles

plUInt64 plVegetationCompositeWindTiles::MakeKey(plUInt64 uiTypeKey, plInt32 iX, plInt32 iY)
{
  const plUInt64 uiCoords = (static_cast<plUInt64>(static_cast<plUInt32>(iX)) << 32) | static_cast<plUInt32>(iY);
  return plHashingUtils::xxHash64(&uiCoords, sizeof(uiCoords), uiTypeKey);
}

void plVegetationCompositeWindTiles::TileCoords(const plVec3& vPosition, float fTileSize, plInt32& out_iX, plInt32& out_iY)
{
  const float fSize = plMath::Max(fTileSize, 1.0f);
  out_iX = static_cast<plInt32>(plMath::Floor(vPosition.x / fSize));
  out_iY = static_cast<plInt32>(plMath::Floor(vPosition.y / fSize));
}

plUInt32 plVegetationCompositeWindTiles::ChooseVariant(const plVec3& vPosition, plUInt8 uiSeed, plUInt32 uiVariantCount)
{
  if (uiVariantCount <= 1)
    return 0;

  // Centimetre grid, so the choice does not depend on float noise in the position.
  const plInt32 key[3] = {static_cast<plInt32>(plMath::Floor(vPosition.x * 100.0f)), static_cast<plInt32>(plMath::Floor(vPosition.y * 100.0f)), uiSeed};
  return plHashingUtils::xxHash32(key, sizeof(key), 0x5eedu) % uiVariantCount;
}

void plVegetationCompositeWindTiles::ComputeVariantParams(plUInt64 uiTileKey, plUInt32 uiVariant, plUInt32 uiVariantCount, float& out_fPhase, float& out_fStiffnessScale)
{
  const plUInt32 uiHash = plHashingUtils::xxHash32(&uiTileKey, sizeof(uiTileKey), uiVariant);
  const float fJitter = static_cast<float>(uiHash & 0xFFFF) / 65535.0f - 0.5f;
  const float fStiffness = static_cast<float>(uiHash >> 16) / 65535.0f;

  // Evenly spread phases, so no two variants of a tile flutter in step, each nudged by up to 20 % of the spacing.
  const float fCount = static_cast<float>(plMath::Max(uiVariantCount, 1u));
  out_fPhase = plMath::Fraction((static_cast<float>(uiVariant) + 0.5f + fJitter * 0.4f) / fCount) * 6.2831853f;
  out_fStiffnessScale = 0.8f + 0.4f * fStiffness;
}

void plVegetationCompositeWindTiles::BeginFrame()
{
  for (auto it = m_Tiles.GetIterator(); it.IsValid(); ++it)
    it.Value().m_bUsed = false;
}

void plVegetationCompositeWindTiles::MarkUsed(plUInt64 uiTypeKey, plUInt32 uiTypeChangeCounter, plInt32 iX, plInt32 iY, float fSampleHeight, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, plDynamicArray<Change>& out_changes)
{
  const plUInt64 uiKey = MakeKey(uiTypeKey, iX, iY);
  const plUInt32 uiVariants = plMath::Clamp(settings.m_uiVariants, 1u, plVegetationMaxWindVariants);

  Tile* pTile = nullptr;
  if (m_Tiles.TryGetValue(uiKey, pTile) && (pTile->m_uiTypeChangeCounter != uiTypeChangeCounter || pTile->m_uiVariantCount != uiVariants))
  {
    // A reloaded type may have another skeleton; a new variant count reassigns trees. Make the tile again.
    if (pTile->m_bReady)
      out_changes.PushBack({uiTypeKey, iX, iY});
    Release(*pTile, ref_backend);
    m_Tiles.Remove(uiKey);
    pTile = nullptr;
  }

  if (pTile == nullptr)
  {
    Tile tile;
    tile.m_uiTypeKey = uiTypeKey;
    tile.m_uiTypeChangeCounter = uiTypeChangeCounter;
    tile.m_iX = iX;
    tile.m_iY = iY;
    const float fSize = plMath::Max(settings.m_fTileSize, 1.0f);
    tile.m_vSample.Set((iX + 0.5f) * fSize, (iY + 0.5f) * fSize, fSampleHeight);
    tile.m_uiVariantCount = uiVariants;
    for (plUInt32 v = 0; v < plVegetationMaxWindVariants; ++v)
    {
      tile.m_Variants[v] = plVegetationCompositeWindBackend::InvalidInstance;
      tile.m_PaletteOffsets[v] = 0;
    }
    m_Tiles.Insert(uiKey, tile);
    m_Tiles.TryGetValue(uiKey, pTile);
  }

  pTile->m_bUsed = true;
  pTile->m_fUnusedTime = 0.0f;
}

void plVegetationCompositeWindTiles::Update(float fDeltaTime, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, plDynamicArray<Change>& out_changes)
{
  for (auto it = m_Tiles.GetIterator(); it.IsValid();)
  {
    Tile& tile = it.Value();

    if (!tile.m_bUsed)
    {
      tile.m_fUnusedTime += fDeltaTime;
      if (tile.m_fUnusedTime >= settings.m_fTileReleaseDelay)
      {
        if (tile.m_bReady)
          out_changes.PushBack({tile.m_uiTypeKey, tile.m_iX, tile.m_iY});
        Release(tile, ref_backend);
        it = m_Tiles.Remove(it);
        continue;
      }
    }

    if (!tile.m_bReady)
    {
      bool bAllPosed = true;
      for (plUInt32 v = 0; v < tile.m_uiVariantCount; ++v)
      {
        if (tile.m_Variants[v] == plVegetationCompositeWindBackend::InvalidInstance)
        {
          float fPhase = 0.0f, fStiffness = 1.0f;
          ComputeVariantParams(it.Key(), v, tile.m_uiVariantCount, fPhase, fStiffness);
          tile.m_Variants[v] = ref_backend.RegisterVariant(tile.m_uiTypeKey, tile.m_vSample, fPhase, fStiffness);
          if (tile.m_Variants[v] == plVegetationCompositeWindBackend::InvalidInstance)
          {
            // The merged mesh is not ready: the other variants would fail too.
            bAllPosed = false;
            break;
          }
          ++m_uiRegisteredVariants;
        }

        plUInt32 uiBoneCount = 0;
        if (!ref_backend.GetPose(tile.m_Variants[v], tile.m_PaletteOffsets[v], uiBoneCount))
          bAllPosed = false;
        else
          tile.m_uiBoneCount = uiBoneCount;
      }

      if (bAllPosed)
      {
        tile.m_bReady = true;
        out_changes.PushBack({tile.m_uiTypeKey, tile.m_iX, tile.m_iY});
      }
    }

    ++it;
  }
}

void plVegetationCompositeWindTiles::ReleaseAll(plVegetationCompositeWindBackend& ref_backend)
{
  for (auto it = m_Tiles.GetIterator(); it.IsValid(); ++it)
    Release(it.Value(), ref_backend);
  m_Tiles.Clear();
}

void plVegetationCompositeWindTiles::Release(Tile& ref_tile, plVegetationCompositeWindBackend& ref_backend)
{
  for (plUInt32 v = 0; v < ref_tile.m_uiVariantCount; ++v)
  {
    if (ref_tile.m_Variants[v] == plVegetationCompositeWindBackend::InvalidInstance)
      continue;
    ref_backend.Unregister(ref_tile.m_Variants[v]);
    ref_tile.m_Variants[v] = plVegetationCompositeWindBackend::InvalidInstance;
    m_uiRegisteredVariants -= plMath::Min(m_uiRegisteredVariants, 1u);
  }
  ref_tile.m_bReady = false;
}

const plVegetationCompositeWindTiles::Tile* plVegetationCompositeWindTiles::Find(plUInt64 uiTypeKey, plInt32 iX, plInt32 iY) const
{
  const Tile* pTile = nullptr;
  m_Tiles.TryGetValue(MakeKey(uiTypeKey, iX, iY), pTile);
  return pTile;
}

bool plVegetationCompositeWindTiles::GetTreePalette(plUInt64 uiTypeKey, const plVegetationCellInstance& tree, float fTileSize, plUInt32& out_uiPaletteOffset, plUInt32& out_uiBoneCount) const
{
  plInt32 iX, iY;
  TileCoords(tree.m_vPosition, fTileSize, iX, iY);
  const Tile* pTile = Find(uiTypeKey, iX, iY);
  if (pTile == nullptr || !pTile->m_bReady)
    return false;

  out_uiPaletteOffset = pTile->m_PaletteOffsets[ChooseVariant(tree.m_vPosition, tree.m_uiColorSeed, pTile->m_uiVariantCount)];
  out_uiBoneCount = pTile->m_uiBoneCount;
  return true;
}

plUInt32 plVegetationCompositeWindTiles::GetTreeVariant(plUInt64 uiTypeKey, const plVegetationCellInstance& tree, float fTileSize) const
{
  plInt32 iX, iY;
  TileCoords(tree.m_vPosition, fTileSize, iX, iY);
  const Tile* pTile = Find(uiTypeKey, iX, iY);
  if (pTile == nullptr || !pTile->m_bReady)
    return plVegetationCompositeWindBackend::InvalidInstance;

  return pTile->m_Variants[ChooseVariant(tree.m_vPosition, tree.m_uiColorSeed, pTile->m_uiVariantCount)];
}

//////////////////////////////////////////////////////////////////////////
// plVegetationCompositeTreeWind

plVegetationCompositeTreeWind::Event plVegetationCompositeTreeWind::Step(plVegetationCellInstance& ref_tree, bool bWanted, bool bOwnPoseValid, float fDeltaTime, const plVegetationCompositeWindSettings& settings)
{
  if (ref_tree.m_uiBoneInstance == plVegetationCompositeWindBackend::InvalidInstance)
  {
    ref_tree.m_fStandalone = 0.0f;
    ref_tree.m_fWindIdle = 0.0f;
    return bWanted ? Event::Promote : Event::None;
  }

  ref_tree.m_fWindIdle = bWanted ? 0.0f : ref_tree.m_fWindIdle + fDeltaTime;
  const bool bKeep = ref_tree.m_fWindIdle < settings.m_fDemoteDelay;
  const float fStep = settings.m_fBlendTime > 0.0f ? fDeltaTime / settings.m_fBlendTime : 1.0f;
  const float fBefore = ref_tree.m_fStandalone;

  if (bKeep)
  {
    // Blend in only once the own palette holds a pose; until then the shared one shows.
    // Snapped at the ends, so float steps settle exactly and the tree stops needing writes.
    if (bOwnPoseValid)
    {
      ref_tree.m_fStandalone += fStep;
      if (ref_tree.m_fStandalone > 0.999f)
        ref_tree.m_fStandalone = 1.0f;
    }
  }
  else
  {
    ref_tree.m_fStandalone -= fStep;
    if (ref_tree.m_fStandalone < 0.001f)
    {
      ref_tree.m_fStandalone = 0.0f;
      return Event::Demote;
    }
  }

  return (ref_tree.m_fStandalone != fBefore || !bOwnPoseValid) ? Event::Blend : Event::None;
}

float plVegetationCompositeTreeWind::GetSharedWeight(const plVegetationCellInstance& tree)
{
  return tree.m_uiBoneInstance == plVegetationCompositeWindBackend::InvalidInstance ? 1.0f : plMath::Clamp(1.0f - tree.m_fStandalone, 0.0f, 1.0f);
}

void plVegetationCompositeTreeWind::UpdateGroup(plVegetationCellGroup& ref_group, const GroupInputs& inputs, float fDeltaTime, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, GroupResult& out_result)
{
  out_result = GroupResult();
  ref_group.m_bBonesBlending = false;

  const float fMaxTreeRadius = inputs.m_fTypeRadius * inputs.m_fMaxScale;
  auto CellDistanceSq = [&](const plVec3& vPoint)
  {
    const plVec2 vNearest(plMath::Clamp(vPoint.x, inputs.m_vCellMin.x, inputs.m_vCellMin.x + inputs.m_fCellSize), plMath::Clamp(vPoint.y, inputs.m_vCellMin.y, inputs.m_vCellMin.y + inputs.m_fCellSize));
    return (vNearest - vPoint.GetAsVec2()).GetLengthSquared();
  };

  // Which touch sources can reach a tree of the cell; usually none.
  plHybridArray<TouchSphere, 8> touches;
  for (const TouchSphere& touch : inputs.m_Touches)
  {
    const float fReach = touch.m_fRadius + fMaxTreeRadius + settings.m_fTouchMargin;
    if (CellDistanceSq(touch.m_vPosition) <= fReach * fReach)
      touches.PushBack(touch);
  }

  const float fCameraReach = settings.m_fPromoteRadius + fMaxTreeRadius;
  const bool bCameraNear = settings.m_fPromoteRadius > 0.0f && CellDistanceSq(inputs.m_vCamera) <= fCameraReach * fCameraReach;

  // Nothing near, nothing promoted or pinned: every tree is shared and stays so. No per-tree work.
  if (ref_group.m_uiBoneInstances == 0 && ref_group.m_uiPinnedTrees == 0 && !bCameraNear && touches.IsEmpty())
    return;

  out_result.m_bEvaluatedPerTree = true;
  bool bRegisterFailed = false;

  for (plVegetationCellInstance& tree : ref_group.m_Instances)
  {
    const float fTreeRadius = inputs.m_fTypeRadius * tree.m_fScale;
    const float fDistanceSq = (tree.m_vPosition - inputs.m_vCamera).GetLengthSquared();
    const float fDrawReach = inputs.m_fDrawRange + fTreeRadius;

    bool bWanted = false;
    if (fDistanceSq <= fDrawReach * fDrawReach)
    {
      bWanted = tree.m_uiWindPins > 0 || (settings.m_fPromoteRadius > 0.0f && fDistanceSq <= settings.m_fPromoteRadius * settings.m_fPromoteRadius);
      for (plUInt32 t = 0; t < touches.GetCount() && !bWanted; ++t)
      {
        const float fReach = touches[t].m_fRadius + fTreeRadius + settings.m_fTouchMargin;
        bWanted = (touches[t].m_vPosition - tree.m_vPosition).GetLengthSquared() <= fReach * fReach;
      }
    }

    bool bOwnPoseValid = false;
    if (tree.m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance)
    {
      plUInt32 uiOffset = 0, uiBones = 0;
      bOwnPoseValid = ref_backend.GetPose(tree.m_uiBoneInstance, uiOffset, uiBones);
      ref_backend.SetTreeWanted(tree.m_uiBoneInstance, bWanted);
    }

    switch (Step(tree, bWanted, bOwnPoseValid, fDeltaTime, settings))
    {
      case Event::None:
        if (tree.m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance)
          ++out_result.m_uiStandalone;
        break;

      case Event::Promote:
        // A failure means the merged mesh is not ready; the other trees would fail too. Retried next frame.
        if (bRegisterFailed)
          break;
        tree.m_uiBoneInstance = ref_backend.RegisterTree(inputs.m_uiTypeKey, tree);
        if (tree.m_uiBoneInstance == plVegetationCompositeWindBackend::InvalidInstance)
        {
          bRegisterFailed = true;
          break;
        }
        // Starts fully shared, its own springs bent as its shared variant's are; it blends in once the pool posed it.
        tree.m_fStandalone = 0.0f;
        tree.m_fWindIdle = 0.0f;
        ++ref_group.m_uiBoneInstances;
        ++ref_group.m_uiBonesGeneration;
        ++out_result.m_uiPromoted;
        ++out_result.m_uiBlending;
        ref_group.m_bBonesBlending = true;
        break;

      case Event::Blend:
        ++out_result.m_uiBlending;
        ref_group.m_bBonesBlending = true;
        break;

      case Event::Demote:
        ReleaseTree(ref_group, tree, ref_backend);
        ++out_result.m_uiDemoted;
        break;
    }

    if (tree.m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance)
      ref_backend.UpdateTree(tree.m_uiBoneInstance, tree);
  }
}

void plVegetationCompositeTreeWind::ReleaseTree(plVegetationCellGroup& ref_group, plVegetationCellInstance& ref_tree, plVegetationCompositeWindBackend& ref_backend)
{
  ref_tree.m_fStandalone = 0.0f;
  ref_tree.m_fWindIdle = 0.0f;
  if (ref_tree.m_uiBoneInstance == plVegetationCompositeWindBackend::InvalidInstance)
    return;

  ref_backend.Unregister(ref_tree.m_uiBoneInstance);
  ref_tree.m_uiBoneInstance = plVegetationCompositeWindBackend::InvalidInstance;
  ref_group.m_uiBoneInstances -= plMath::Min(ref_group.m_uiBoneInstances, 1u);
  ++ref_group.m_uiBonesGeneration;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_World_VegetationCompositeWind);
