#include <Foundation/Basics.h>
#include <RendererCore/Meshes/FoliageSkeleton.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>
#include <VegetationPlugin/World/VegetationCompositeSplit.h>
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
