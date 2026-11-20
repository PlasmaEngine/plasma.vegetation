#include <Foundation/Basics.h>
#include <RendererCore/Meshes/FoliageSkeleton.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>
#include <VegetationPlugin/World/VegetationCompositeSplit.h>
#include <VegetationPlugin/World/VegetationCompositeWind.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// CPU checks of how vegetation cells draw foliage composites: the per-tree parts/merged split and when it reports a
// change, the switch distance, and the sway bound of part instances. Needs no renderer.

PL_TESTFRAMEWORK_ENTRY_POINT("ForestCellsTest", "Vegetation cell foliage composite tests")
PL_CREATE_SIMPLE_TEST_GROUP(ForestCells);

namespace
{
  plVegetationCellInstance MakeTree(const plVec3& vPosition, float fScale = 1.0f, plUInt8 uiSeed = 0)
  {
    plVegetationCellInstance inst;
    plMemoryUtils::ZeroFill(&inst, 1);
    inst.m_vPosition = vPosition;
    inst.m_qRotation = plQuat::MakeIdentity();
    inst.m_fScale = fScale;
    inst.m_uiColorSeed = uiSeed;
    inst.m_uiBoneInstance = 0xFFFFFFFF;
    return inst;
  }

  plVegetationCompositeThresholds MakeThresholds()
  {
    plVegetationCompositeThresholds t;
    t.m_fSwitchDistance = 50.0f;
    t.m_fCullDistance = 400.0f;
    t.m_fShadowCullDistance = 150.0f;
    t.m_fDensity = 1.0f;
    return t;
  }

  /// One 32 m cell's worth of trees on a 4 x 4 grid starting at vOrigin.
  void MakeCell(const plVec3& vOrigin, plDynamicArray<plVegetationCellInstance>& out_trees)
  {
    out_trees.Clear();
    for (plUInt32 y = 0; y < 4; ++y)
      for (plUInt32 x = 0; x < 4; ++x)
        out_trees.PushBack(MakeTree(vOrigin + plVec3(x * 8.0f, y * 8.0f, 0.0f), 1.0f, static_cast<plUInt8>(x + y * 4)));
  }
} // namespace

PL_CREATE_SIMPLE_TEST(ForestCells, ClassifyTree)
{
  using S = plVegetationCompositeTreeState;
  const plVegetationCompositeThresholds t = MakeThresholds();

  plVegetationCellInstance tree = MakeTree(plVec3::MakeZero());
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 10.0f, t), S::Parts | S::CastsShadow);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 60.0f, t), S::Merged | S::CastsShadow);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 200.0f, t), S::Merged);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 401.0f, t), S::Hidden);

  // The switch distance scales with the tree: a tree twice as large keeps its parts twice as far.
  tree.m_fScale = 2.0f;
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 60.0f, t), S::Parts | S::CastsShadow);

  tree.m_Flags.Add(plVegetationInstanceFlags::NoShadow);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(tree, 60.0f, t), S::Parts);

  // Density drops trees by colour seed; hero trees stay.
  plVegetationCompositeThresholds thin = t;
  thin.m_fDensity = 0.5f;
  plVegetationCellInstance high = MakeTree(plVec3::MakeZero(), 1.0f, 200);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(high, 10.0f, thin), S::Hidden);
  high.m_Flags.Add(plVegetationInstanceFlags::Hero);
  PL_TEST_INT(plVegetationCompositeSplit::ClassifyTree(high, 10.0f, thin), S::Parts | S::CastsShadow);
}

PL_CREATE_SIMPLE_TEST(ForestCells, SplitOnlyChangesWhenATreeCrosses)
{
  plDynamicArray<plVegetationCellInstance> trees;
  MakeCell(plVec3(1000.0f, 0.0f, 0.0f), trees);
  const plVegetationCompositeThresholds t = MakeThresholds();

  plVegetationCompositeSplit split;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Far cell: no per-tree work while the camera moves")
  {
    // First update evaluates everything.
    PL_TEST_BOOL(split.Update(trees, plVec3(0, 0, 0), t));
    PL_TEST_BOOL(split.WasEvaluatedPerTree());
    PL_TEST_INT(split.GetCount(plVegetationCompositeTreeState::Hidden), 16);

    // 1 km away, the camera moves 100 m: the cell stays beyond every threshold.
    for (plUInt32 i = 1; i <= 10; ++i)
    {
      PL_TEST_BOOL(!split.Update(trees, plVec3(i * 10.0f, 0, 0), t));
      PL_TEST_BOOL(!split.WasEvaluatedPerTree());
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Approaching: merged, then parts")
  {
    // 300 m: all within cull, beyond shadow and switch distance.
    PL_TEST_BOOL(split.Update(trees, plVec3(700.0f, 0, 0), t));
    PL_TEST_INT(split.GetCount(plVegetationCompositeTreeState::Merged), 16);

    // 290 m: same band for the whole cell, nothing evaluated per tree.
    PL_TEST_BOOL(!split.Update(trees, plVec3(710.0f, 0, 0), t));
    PL_TEST_BOOL(!split.WasEvaluatedPerTree());

    // Inside the cell: some trees within 50 m show their parts, the cell straddles the switch.
    PL_TEST_BOOL(split.Update(trees, plVec3(1000.0f, -30.0f, 0), t));
    PL_TEST_BOOL(split.WasEvaluatedPerTree());
    const plUInt32 uiParts = split.GetCount(plVegetationCompositeTreeState::Parts);
    PL_TEST_BOOL(uiParts > 0 && uiParts < 16);
    PL_TEST_INT(uiParts + split.GetCount(plVegetationCompositeTreeState::Merged), 16);

    // A tiny move that crosses no tree's threshold: evaluated per tree (the cell straddles), but no change.
    PL_TEST_BOOL(!split.Update(trees, plVec3(1000.0f, -30.001f, 0), t));
    PL_TEST_BOOL(split.WasEvaluatedPerTree());

    // Standing still: nothing at all.
    PL_TEST_BOOL(!split.Update(trees, plVec3(1000.0f, -30.001f, 0), t));
    PL_TEST_BOOL(!split.WasEvaluatedPerTree());

    // Cell centre: every tree within 50 m.
    PL_TEST_BOOL(split.Update(trees, plVec3(1012.0f, 12.0f, 0), t));
    PL_TEST_INT(split.GetCount(plVegetationCompositeTreeState::Parts), 16);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Inputs and instances")
  {
    // A changed threshold is a change even with the camera still.
    plVegetationCompositeThresholds smaller = t;
    smaller.m_fSwitchDistance = 1.0f;
    PL_TEST_BOOL(split.Update(trees, plVec3(1012.0f, 12.0f, 0), smaller));
    PL_TEST_INT(split.GetCount(plVegetationCompositeTreeState::Parts), 0);

    // A different instance count (the cell changed) starts over.
    trees.PopBack();
    PL_TEST_BOOL(split.Update(trees, plVec3(1012.0f, 12.0f, 0), smaller));
    PL_TEST_INT(split.GetStates().GetCount(), 15);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Per tree matches the rule")
  {
    plVegetationCompositeSplit fresh;
    const plVec3 vCamera(1005.0f, -20.0f, 3.0f);
    fresh.Update(trees, vCamera, t);
    for (plUInt32 i = 0; i < trees.GetCount(); ++i)
      PL_TEST_INT(fresh.GetStates()[i], plVegetationCompositeSplit::ClassifyTree(trees[i], (trees[i].m_vPosition - vCamera).GetLength(), t));
  }
}

PL_CREATE_SIMPLE_TEST(ForestCells, SwitchDistance)
{
  // 1 cm of error, 1080 lines, 90 degree vertical field of view, 1 pixel: 0.01 * 1080 / 2 = 5.4 m.
  PL_TEST_FLOAT(plVegetationCompositeInfo::ComputeSwitchDistance(0.01f, 1.0f, 1080.0f, 1.0f, 1.0f), 5.4f, 1e-4f);
  PL_TEST_FLOAT(plVegetationCompositeInfo::ComputeSwitchDistance(0.01f, 2.0f, 1080.0f, 1.0f, 1.0f), 10.8f, 1e-4f);
  PL_TEST_FLOAT(plVegetationCompositeInfo::ComputeSwitchDistance(0.01f, 1.0f, 1080.0f, 1.0f, 2.0f), 2.7f, 1e-4f);
  PL_TEST_FLOAT(plVegetationCompositeInfo::ComputeSwitchDistance(0.0f, 1.0f, 1080.0f, 1.0f, 1.0f), 0.0f, 0.0f);
}

PL_CREATE_SIMPLE_TEST(ForestCells, PartSwayBound)
{
  plFoliageSkeleton skeleton;
  {
    plFoliageBone& trunk = skeleton.m_Bones.ExpandAndGetRef();
    trunk.m_vPivot = plVec3::MakeZero();
    trunk.m_vDirection = plVec3(0, 0, 1);
    trunk.m_fLength = 5.0f;
    trunk.m_fMaxBend = 0.1f;

    plFoliageBone& branch = skeleton.m_Bones.ExpandAndGetRef();
    branch.m_vPivot = plVec3(0, 0, 5);
    branch.m_vDirection = plVec3(1, 0, 0);
    branch.m_fLength = 3.0f;
    branch.m_uiParent = 0;
    branch.m_fMaxBend = 0.4f;

    plFoliageBone& twig = skeleton.m_Bones.ExpandAndGetRef();
    twig.m_vPivot = plVec3(3, 0, 5);
    twig.m_vDirection = plVec3(0, 1, 0);
    twig.m_fLength = 1.0f;
    twig.m_uiParent = 1;
    twig.m_fMaxBend = 0.8f;
  }
  PL_TEST_BOOL(skeleton.Finalize().Succeeded());

  plVegetationCompositeInfo info;
  {
    plFoliageCompositeInstance& onTwig = info.m_Instances.ExpandAndGetRef();
    onTwig.m_Transform.m_vPosition = plVec3(3, 0.5f, 5);
    onTwig.SetBone(2);

    plFoliageCompositeInstance& blended = info.m_Instances.ExpandAndGetRef();
    blended.m_Transform.m_vPosition = plVec3(1.5f, 0, 5);
    const plUInt16 bones[] = {0, 1};
    const float weights[] = {0.3f, 0.7f};
    blended.SetBones(bones, weights);

    plFoliageCompositeInstance& vertexBones = info.m_Instances.ExpandAndGetRef();
    vertexBones.m_Transform.m_vPosition = plVec3(0, 0, 2);
    vertexBones.SetBone(plFoliageCompositeInstance::InvalidBone);
  }
  info.ComputeSway(&skeleton);
  PL_TEST_INT(info.m_InstanceSway.GetCount(), 3);

  // Every point within r of a placement moves at most sway + r * per-metre, for every bone the placement can follow.
  const float fRadius = 1.5f;
  for (plUInt32 i = 0; i < info.m_Instances.GetCount(); ++i)
  {
    const plFoliageCompositeInstance& inst = info.m_Instances[i];
    const float fBound = info.m_InstanceSway[i] + fRadius * info.m_InstanceSwayPerMetre[i];
    PL_TEST_BOOL(info.m_InstanceSway[i] > 0.0f);

    for (plUInt32 s = 0; s < 64; ++s)
    {
      // Points on the sphere of radius fRadius and at its centre.
      const float fTheta = s * 0.61803398875f * 6.2831853f;
      const float fZ = 1.0f - 2.0f * (s + 0.5f) / 64.0f;
      const float fR = plMath::Sqrt(1.0f - fZ * fZ);
      const plVec3 vPoint = inst.m_Transform.m_vPosition + fRadius * plVec3(fR * plMath::Cos(plAngle::MakeFromRadian(fTheta)), fR * plMath::Sin(plAngle::MakeFromRadian(fTheta)), fZ);

      for (plUInt32 b = 0; b < skeleton.m_Bones.GetCount(); ++b)
      {
        const bool bFollows = inst.GetDominantBone() == plFoliageCompositeInstance::InvalidBone || inst.m_Bones[0] == b || inst.m_Bones[1] == b || inst.m_Bones[2] == b;
        if (!bFollows)
          continue;
        PL_TEST_BOOL(skeleton.ComputeDisplacementBound(vPoint, b) <= fBound + 1e-4f);
        PL_TEST_BOOL(skeleton.ComputeDisplacementBound(inst.m_Transform.m_vPosition, b) <= info.m_InstanceSway[i] + 1e-4f);
      }
    }
  }

  // Without a skeleton nothing sways.
  info.ComputeSway(nullptr);
  for (plUInt32 i = 0; i < info.m_Instances.GetCount(); ++i)
  {
    PL_TEST_FLOAT(info.m_InstanceSway[i], 0.0f, 0.0f);
    PL_TEST_FLOAT(info.m_InstanceSwayPerMetre[i], 0.0f, 0.0f);
  }
}

PL_CREATE_SIMPLE_TEST(ForestCells, SplitCost)
{
  // Logs what the per-frame split costs per cell while the camera moves: a cell beyond every threshold, and one that
  // straddles the switch distance (64 trees). Informational, nothing is asserted on time.
  plDynamicArray<plVegetationCellInstance> trees;
  for (plUInt32 i = 0; i < 64; ++i)
    trees.PushBack(MakeTree(plVec3((i % 8) * 4.0f, (i / 8) * 4.0f, 0.0f), 0.8f + (i % 5) * 0.1f, static_cast<plUInt8>(i * 3)));

  const plVegetationCompositeThresholds t = MakeThresholds();
  constexpr plUInt32 uiFrames = 20000;

  auto Measure = [&](const plVec3& vStart, plUInt32& out_uiChanges) {
    plVegetationCompositeSplit split;
    split.Update(trees, vStart, t);
    out_uiChanges = 0;
    const plTime start = plTime::Now();
    for (plUInt32 f = 0; f < uiFrames; ++f)
      out_uiChanges += split.Update(trees, vStart + plVec3(f * 0.001f, 0, 0), t) ? 1 : 0;
    return (plTime::Now() - start).GetNanoseconds() / uiFrames;
  };

  plUInt32 uiFarChanges = 0, uiNearChanges = 0;
  const double fFar = Measure(plVec3(-1000.0f, 0, 0), uiFarChanges);
  const double fNear = Measure(plVec3(-30.0f, 10.0f, 0), uiNearChanges);
  plStringBuilder sResult;
  sResult.SetFormat("Split per cell and frame, camera moving 1 mm per frame: far cell {} ns ({} rebuilds), straddling cell of 64 trees {} ns ({} rebuilds in {} frames)", plArgF(fFar, 1), uiFarChanges, plArgF(fNear, 1), uiNearChanges, uiFrames);
  plTestFramework::Output(plTestOutput::Message, "%s", sResult.GetData());
  PL_TEST_INT(uiFarChanges, 0);
  PL_TEST_BOOL(uiNearChanges < uiFrames / 10);
}

PL_CREATE_SIMPLE_TEST(ForestCells, BonePoolIdTravelsWithSwap)
{
  // Cells remove instances with RemoveAtAndSwap. The pool id lives in the instance, so the tree moved into the freed
  // slot keeps its registration and only the removed tree's id is released.
  plDynamicArray<plVegetationCellInstance> trees;
  for (plUInt32 i = 0; i < 4; ++i)
  {
    trees.PushBack(MakeTree(plVec3(i * 1.0f, 0, 0)));
    trees.PeekBack().m_uiBoneInstance = 100 + i;
  }

  trees.RemoveAtAndSwap(1);
  PL_TEST_INT(trees.GetCount(), 3);
  PL_TEST_INT(trees[1].m_uiBoneInstance, 103);
  PL_TEST_FLOAT(trees[1].m_vPosition.x, 3.0f, 0.0f);
}

namespace
{
  /// Stands in for plFoliageBonePool: ids in registration order, a pose after m_uiPoseFrames ticks.
  class FakeWindBackend : public plVegetationCompositeWindBackend
  {
  public:
    struct Entry
    {
      bool m_bAlive = false;
      bool m_bShared = false;
      plVec3 m_vSample = plVec3::MakeZero();
      float m_fPhase = 0.0f;
      float m_fStiffness = 1.0f;
      plUInt32 m_uiAge = 0;
    };

    virtual plUInt32 RegisterVariant(plUInt64 uiTypeKey, const plVec3& vWindSample, float fPhase, float fStiffnessScale) override
    {
      PL_IGNORE_UNUSED(uiTypeKey);
      if (!m_bMeshReady)
        return InvalidInstance;
      Entry& e = m_Entries.ExpandAndGetRef();
      e.m_bAlive = true;
      e.m_bShared = true;
      e.m_vSample = vWindSample;
      e.m_fPhase = fPhase;
      e.m_fStiffness = fStiffnessScale;
      ++m_uiRegistrations;
      return m_Entries.GetCount() - 1;
    }

    virtual plUInt32 RegisterTree(plUInt64 uiTypeKey, const plVegetationCellInstance& tree) override
    {
      PL_IGNORE_UNUSED(uiTypeKey);
      PL_IGNORE_UNUSED(tree);
      if (!m_bMeshReady)
        return InvalidInstance;
      m_Entries.ExpandAndGetRef().m_bAlive = true;
      ++m_uiRegistrations;
      return m_Entries.GetCount() - 1;
    }

    virtual void UpdateTree(plUInt32 uiInstance, const plVegetationCellInstance& tree) override
    {
      PL_IGNORE_UNUSED(uiInstance);
      PL_IGNORE_UNUSED(tree);
      ++m_uiTreeUpdates;
    }

    virtual void Unregister(plUInt32& inout_uiInstance) override
    {
      if (inout_uiInstance < m_Entries.GetCount())
        m_Entries[inout_uiInstance].m_bAlive = false;
      inout_uiInstance = InvalidInstance;
      ++m_uiUnregistrations;
    }

    virtual bool GetPose(plUInt32 uiInstance, plUInt32& out_uiPaletteOffset, plUInt32& out_uiBoneCount) override
    {
      if (uiInstance >= m_Entries.GetCount() || !m_Entries[uiInstance].m_bAlive)
        return false;
      out_uiPaletteOffset = uiInstance * 100;
      out_uiBoneCount = 50;
      return m_Entries[uiInstance].m_uiAge >= m_uiPoseFrames;
    }

    /// The pool's dispatch at the next BeginRender.
    void Tick()
    {
      for (Entry& e : m_Entries)
        e.m_uiAge += e.m_bAlive ? 1 : 0;
    }

    plUInt32 GetAlive(bool bShared) const
    {
      plUInt32 uiCount = 0;
      for (const Entry& e : m_Entries)
        uiCount += (e.m_bAlive && e.m_bShared == bShared) ? 1 : 0;
      return uiCount;
    }

    plDynamicArray<Entry> m_Entries;
    bool m_bMeshReady = true;
    plUInt32 m_uiPoseFrames = 1;
    plUInt32 m_uiRegistrations = 0;
    plUInt32 m_uiUnregistrations = 0;
    plUInt32 m_uiTreeUpdates = 0;
  };

  plVegetationCompositeTreeWind::GroupInputs MakeGroupInputs(const plVec3& vCamera)
  {
    plVegetationCompositeTreeWind::GroupInputs inputs;
    inputs.m_uiTypeKey = 42;
    inputs.m_vCamera = vCamera;
    inputs.m_vCellMin = plVec2(0, 0);
    inputs.m_fCellSize = 32.0f;
    inputs.m_fDrawRange = 400.0f;
    inputs.m_fTypeRadius = 3.0f;
    return inputs;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(ForestCells, WindVariantAssignment)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Same tree, same variant; every variant used")
  {
    plUInt32 uiUsed[8] = {};
    for (plUInt32 i = 0; i < 1000; ++i)
    {
      const plVec3 vPosition(i * 1.37f, (i % 31) * 2.11f, 5.0f);
      const plUInt32 v = plVegetationCompositeWindTiles::ChooseVariant(vPosition, static_cast<plUInt8>(i), 8);
      PL_TEST_BOOL(v < 8);
      PL_TEST_INT(plVegetationCompositeWindTiles::ChooseVariant(vPosition, static_cast<plUInt8>(i), 8), v);
      ++uiUsed[v];
    }
    for (plUInt32 v = 0; v < 8; ++v)
      PL_TEST_BOOL(uiUsed[v] > 60); // ~125 each

    PL_TEST_INT(plVegetationCompositeWindTiles::ChooseVariant(plVec3(1, 2, 3), 7, 1), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Variant parameters are deterministic and spread")
  {
    const plUInt64 uiTile = plVegetationCompositeWindTiles::MakeKey(42, -3, 5);
    PL_TEST_BOOL(uiTile != plVegetationCompositeWindTiles::MakeKey(42, 5, -3));
    PL_TEST_BOOL(uiTile != plVegetationCompositeWindTiles::MakeKey(43, -3, 5));

    float phases[8];
    for (plUInt32 v = 0; v < 8; ++v)
    {
      float fPhase = 0.0f, fStiffness = 0.0f, fPhase2 = 0.0f, fStiffness2 = 0.0f;
      plVegetationCompositeWindTiles::ComputeVariantParams(uiTile, v, 8, fPhase, fStiffness);
      plVegetationCompositeWindTiles::ComputeVariantParams(uiTile, v, 8, fPhase2, fStiffness2);
      PL_TEST_FLOAT(fPhase, fPhase2, 0.0f);
      PL_TEST_FLOAT(fStiffness, fStiffness2, 0.0f);
      PL_TEST_BOOL(fStiffness >= 0.8f && fStiffness <= 1.2f);
      PL_TEST_BOOL(fPhase >= 0.0f && fPhase < 6.2832f);
      phases[v] = fPhase;
    }
    // Spaced a cycle / 8 apart, give or take 20 % of that each.
    for (plUInt32 v = 1; v < 8; ++v)
      PL_TEST_BOOL(phases[v] - phases[v - 1] > 6.2831853f / 8.0f * 0.5f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Tile coordinates")
  {
    plInt32 x, y;
    plVegetationCompositeWindTiles::TileCoords(plVec3(-0.5f, 63.9f, 0), 64.0f, x, y);
    PL_TEST_INT(x, -1);
    PL_TEST_INT(y, 0);
    plVegetationCompositeWindTiles::TileCoords(plVec3(64.0f, 128.5f, 0), 64.0f, x, y);
    PL_TEST_INT(x, 1);
    PL_TEST_INT(y, 2);
  }
}

PL_CREATE_SIMPLE_TEST(ForestCells, WindTileLifetime)
{
  FakeWindBackend backend;
  plVegetationCompositeWindTiles tiles;
  plVegetationCompositeWindSettings settings;
  plDynamicArray<plVegetationCompositeWindTiles::Change> changes;
  const float dt = 0.1f;

  auto Frame = [&](bool bUsed) {
    changes.Clear();
    tiles.BeginFrame();
    if (bUsed)
      tiles.MarkUsed(42, 1, 2, -1, 10.0f, settings, backend, changes);
    tiles.Update(dt, settings, backend, changes);
    backend.Tick();
  };

  plVegetationCellInstance tree = MakeTree(plVec3(150.0f, -20.0f, 10.0f), 1.0f, 9);
  plUInt32 uiOffset = 0, uiBones = 0;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Registered on first use, ready once posed")
  {
    Frame(true);
    PL_TEST_INT(backend.GetAlive(true), 8);
    PL_TEST_INT(tiles.GetVariantCount(), 8);
    PL_TEST_INT(changes.GetCount(), 0); // no pose yet
    PL_TEST_BOOL(!tiles.GetTreePalette(42, tree, settings.m_fTileSize, uiOffset, uiBones));

    // Canonical sample position: the tile centre at the given height.
    PL_TEST_VEC3(backend.m_Entries[0].m_vSample, plVec3(160.0f, -32.0f, 10.0f), 0.0f);

    Frame(true);
    PL_TEST_INT(changes.GetCount(), 1);
    PL_TEST_BOOL(tiles.GetTreePalette(42, tree, settings.m_fTileSize, uiOffset, uiBones));
    PL_TEST_INT(uiOffset, plVegetationCompositeWindTiles::ChooseVariant(tree.m_vPosition, tree.m_uiColorSeed, 8) * 100);
    PL_TEST_INT(uiBones, 50);

    // Ready and used: nothing more to do.
    const plUInt32 uiRegistrations = backend.m_uiRegistrations;
    for (plUInt32 f = 0; f < 50; ++f)
    {
      Frame(true);
      PL_TEST_INT(changes.GetCount(), 0);
    }
    PL_TEST_INT(backend.m_uiRegistrations, uiRegistrations);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Released after the delay, kept through short gaps")
  {
    // A few unused frames (a camera wobbling at the edge of the draw range) keep it.
    for (plUInt32 f = 0; f < 10; ++f)
      Frame(false);
    Frame(true);
    PL_TEST_INT(backend.GetAlive(true), 8);
    PL_TEST_INT(changes.GetCount(), 0);

    // m_fTileReleaseDelay = 2 s of 0.1 s frames.
    plUInt32 uiReleasedAt = 0;
    for (plUInt32 f = 1; f <= 30 && uiReleasedAt == 0; ++f)
    {
      Frame(false);
      if (backend.GetAlive(true) == 0)
        uiReleasedAt = f;
    }
    PL_TEST_BOOL(uiReleasedAt >= 20 && uiReleasedAt <= 21); // float accumulation may take one more frame
    PL_TEST_INT(changes.GetCount(), 1);
    PL_TEST_INT(tiles.GetVariantCount(), 0);
    PL_TEST_INT(tiles.GetTileCount(), 0);
    PL_TEST_BOOL(!tiles.GetTreePalette(42, tree, settings.m_fTileSize, uiOffset, uiBones));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mesh not ready: retried; a new variant count or type remakes the tile")
  {
    backend.m_bMeshReady = false;
    Frame(true);
    PL_TEST_INT(backend.GetAlive(true), 0);
    backend.m_bMeshReady = true;
    Frame(true);
    Frame(true);
    PL_TEST_INT(backend.GetAlive(true), 8);
    PL_TEST_INT(changes.GetCount(), 1);

    settings.m_uiVariants = 4;
    Frame(true);
    PL_TEST_INT(backend.GetAlive(true), 4);
    PL_TEST_INT(tiles.GetVariantCount(), 4);
    PL_TEST_INT(changes.GetCount(), 1); // the ready tile went away

    tiles.ReleaseAll(backend);
    PL_TEST_INT(backend.GetAlive(true), 0);
    PL_TEST_INT(tiles.GetVariantCount(), 0);
  }
}

PL_CREATE_SIMPLE_TEST(ForestCells, TreePromotionTiming)
{
  FakeWindBackend backend;
  plVegetationCompositeWindSettings settings;
  const float dt = 0.1f; // fake clock: settings are 0.5 s blend, 2 s demote delay

  plVegetationCellGroup group;
  group.m_Instances.PushBack(MakeTree(plVec3(10, 10, 0)));
  group.m_Instances.PushBack(MakeTree(plVec3(20, 20, 0)));
  plVegetationCellInstance& tree = group.m_Instances[0];

  // The camera far away (but within the 400 m draw range): only the touch can promote.
  plVegetationCompositeTreeWind::TouchSphere touch = {plVec3(11, 10, 0), 0.5f};

  plVegetationCompositeTreeWind::GroupResult result;
  auto Frame = [&](bool bTouch, const plVec3& vCamera = plVec3(200, 200, 0)) {
    plVegetationCompositeTreeWind::GroupInputs inputs = MakeGroupInputs(vCamera);
    if (bTouch)
      inputs.m_Touches = plArrayPtr<const plVegetationCompositeTreeWind::TouchSphere>(&touch, 1);
    plVegetationCompositeTreeWind::UpdateGroup(group, inputs, dt, settings, backend, result);
    backend.Tick();
  };

  PL_TEST_BLOCK(plTestBlock::Enabled, "Touch promotes, blends in over BlendTime once posed")
  {
    const plUInt32 uiGeneration = group.m_uiBonesGeneration;
    Frame(true);
    PL_TEST_INT(result.m_uiPromoted, 1);
    PL_TEST_BOOL(tree.m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance);
    PL_TEST_INT(group.m_Instances[1].m_uiBoneInstance, plVegetationCompositeWindBackend::InvalidInstance);
    PL_TEST_INT(group.m_uiBoneInstances, 1);
    PL_TEST_INT(group.m_uiBonesGeneration, uiGeneration + 1);
    PL_TEST_BOOL(group.m_bBonesBlending);
    PL_TEST_FLOAT(plVegetationCompositeTreeWind::GetSharedWeight(tree), 1.0f, 0.0f);

    // Posed from the next frame; 0.5 s / 0.1 s = five steps to fully standalone.
    for (plUInt32 f = 1; f <= 5; ++f)
    {
      Frame(true);
      PL_TEST_BOOL(group.m_bBonesBlending);
      PL_TEST_FLOAT(plVegetationCompositeTreeWind::GetSharedWeight(tree), 1.0f - f * 0.2f, 1e-4f);
    }
    Frame(true);
    PL_TEST_BOOL(!group.m_bBonesBlending); // fully standalone: no bone writes
    PL_TEST_INT(result.m_uiStandalone, 1);
    PL_TEST_INT(group.m_uiBonesGeneration, uiGeneration + 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Demoted DemoteDelay after the last touch, blended out, then unregistered")
  {
    const plUInt32 uiGeneration = group.m_uiBonesGeneration;
    plUInt32 uiBlendOutAt = 0, uiDemotedAt = 0;
    for (plUInt32 f = 1; f <= 40 && uiDemotedAt == 0; ++f)
    {
      Frame(false);
      if (uiBlendOutAt == 0 && plVegetationCompositeTreeWind::GetSharedWeight(tree) > 0.0f)
        uiBlendOutAt = f;
      if (result.m_uiDemoted > 0)
        uiDemotedAt = f;
    }
    // Idle reaches 2 s at frame 20 (float accumulation may take one more), then five steps back to shared.
    PL_TEST_BOOL(uiBlendOutAt >= 20 && uiBlendOutAt <= 21);
    PL_TEST_INT(uiDemotedAt, uiBlendOutAt + 4);
    PL_TEST_INT(tree.m_uiBoneInstance, plVegetationCompositeWindBackend::InvalidInstance);
    PL_TEST_INT(group.m_uiBoneInstances, 0);
    PL_TEST_INT(group.m_uiBonesGeneration, uiGeneration + 1);
    PL_TEST_INT(backend.GetAlive(false), 0);
    PL_TEST_FLOAT(plVegetationCompositeTreeWind::GetSharedWeight(tree), 1.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A touch during the blend out brings it back without re-registering")
  {
    Frame(true);
    for (plUInt32 f = 0; f < 6; ++f)
      Frame(true);
    for (plUInt32 f = 0; f < 22; ++f)
      Frame(false);
    PL_TEST_BOOL(plVegetationCompositeTreeWind::GetSharedWeight(tree) > 0.0f);
    PL_TEST_BOOL(tree.m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance);
    const plUInt32 uiRegistrations = backend.m_uiRegistrations;
    Frame(true);
    Frame(true);
    PL_TEST_INT(backend.m_uiRegistrations, uiRegistrations);
    for (plUInt32 f = 0; f < 6; ++f)
      Frame(true);
    PL_TEST_FLOAT(plVegetationCompositeTreeWind::GetSharedWeight(tree), 0.0f, 0.0f);
    for (plUInt32 f = 0; f < 40; ++f)
      Frame(false);
    PL_TEST_INT(group.m_uiBoneInstances, 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Camera within PromoteRadius, and pins")
  {
    Frame(false, plVec3(10, 30, 0)); // 20 m from tree 0, 10 m from tree 1
    PL_TEST_INT(result.m_uiPromoted, 2);
    for (plUInt32 f = 0; f < 40; ++f)
      Frame(false);
    PL_TEST_INT(group.m_uiBoneInstances, 0);

    group.m_Instances[1].m_uiWindPins = 1;
    group.m_uiPinnedTrees = 1;
    Frame(false);
    PL_TEST_INT(result.m_uiPromoted, 1);
    PL_TEST_BOOL(group.m_Instances[1].m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance);
    for (plUInt32 f = 0; f < 60; ++f)
      Frame(false);
    PL_TEST_BOOL(group.m_Instances[1].m_uiBoneInstance != plVegetationCompositeWindBackend::InvalidInstance); // pinned: stays

    group.m_Instances[1].m_uiWindPins = 0;
    group.m_uiPinnedTrees = 0;
    for (plUInt32 f = 0; f < 40; ++f)
      Frame(false);
    PL_TEST_INT(group.m_uiBoneInstances, 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Not drawn: never promoted")
  {
    plVegetationCompositeTreeWind::GroupInputs inputs = MakeGroupInputs(plVec3(10, 30, 0));
    inputs.m_fDrawRange = 5.0f;
    inputs.m_Touches = plArrayPtr<const plVegetationCompositeTreeWind::TouchSphere>(&touch, 1);
    plVegetationCompositeTreeWind::UpdateGroup(group, inputs, dt, settings, backend, result);
    PL_TEST_INT(result.m_uiPromoted, 0);
  }
}

PL_CREATE_SIMPLE_TEST(ForestCells, SharedTreesCostNothingPerFrame)
{
  // A cell of shared trees, away from the camera and every touch: no per-tree work, no pool calls, no bone data to
  // rewrite (the generation stays, nothing blends) however many frames pass.
  FakeWindBackend backend;
  plVegetationCompositeWindSettings settings;

  plVegetationCellGroup group;
  MakeCell(plVec3(0, 0, 0), group.m_Instances);
  const plUInt32 uiGeneration = group.m_uiBonesGeneration;

  const plVegetationCompositeTreeWind::TouchSphere farTouch = {plVec3(200, 0, 0), 1.0f};
  plVegetationCompositeTreeWind::GroupInputs inputs = MakeGroupInputs(plVec3(-100, 16, 0));
  inputs.m_Touches = plArrayPtr<const plVegetationCompositeTreeWind::TouchSphere>(&farTouch, 1);

  plVegetationCompositeTreeWind::GroupResult result;
  for (plUInt32 f = 0; f < 1000; ++f)
  {
    plVegetationCompositeTreeWind::UpdateGroup(group, inputs, 1.0f / 60.0f, settings, backend, result);
    PL_TEST_BOOL(!result.m_bEvaluatedPerTree);
    PL_TEST_BOOL(!group.m_bBonesBlending);
  }
  PL_TEST_INT(group.m_uiBonesGeneration, uiGeneration);
  PL_TEST_INT(backend.m_uiRegistrations, 0);
  PL_TEST_INT(backend.m_uiTreeUpdates, 0);
  PL_TEST_INT(group.m_uiBoneInstances, 0);

  // A camera inside the cell's promote reach looks at its trees, and promotes only the near ones.
  inputs.m_vCamera = plVec3(-5, 0, 0);
  plVegetationCompositeTreeWind::UpdateGroup(group, inputs, 1.0f / 60.0f, settings, backend, result);
  PL_TEST_BOOL(result.m_bEvaluatedPerTree);
  PL_TEST_BOOL(result.m_uiPromoted > 0 && result.m_uiPromoted < group.m_Instances.GetCount());
}
