#include <Foundation/Basics.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Time/Time.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestLogInterface.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Placement/VegetationScatter.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

PL_TESTFRAMEWORK_ENTRY_POINT("VegetationTests", "Vegetation CPU tests")
PL_CREATE_SIMPLE_TEST_GROUP(Vegetation);

namespace
{
  /// Smallest distance between two points relative to the spacing the pair must keep (>= 1 when spacing holds).
  float MinSpacingRatio(plArrayPtr<const plVegetationScatterPoint> points, plArrayPtr<const plVegetationScatterSpecies> species)
  {
    // bucket grid, so 15000 points stay quick
    float fMaxSpacing = 0.01f;
    for (const auto& s : species)
      fMaxSpacing = plMath::Max(fMaxSpacing, s.m_fMinSpacing);

    plHashTable<plUInt64, plDynamicArray<plUInt32>> cells;
    auto Key = [&](plInt32 x, plInt32 y) { return (static_cast<plUInt64>(static_cast<plUInt32>(x)) << 32) | static_cast<plUInt32>(y); };
    for (plUInt32 i = 0; i < points.GetCount(); ++i)
    {
      const plInt32 x = static_cast<plInt32>(plMath::Floor(points[i].m_vPosition.x / fMaxSpacing));
      const plInt32 y = static_cast<plInt32>(plMath::Floor(points[i].m_vPosition.y / fMaxSpacing));
      cells[Key(x, y)].PushBack(i);
    }

    float fMinRatio = plMath::MaxValue<float>();
    for (plUInt32 i = 0; i < points.GetCount(); ++i)
    {
      const plInt32 cx = static_cast<plInt32>(plMath::Floor(points[i].m_vPosition.x / fMaxSpacing));
      const plInt32 cy = static_cast<plInt32>(plMath::Floor(points[i].m_vPosition.y / fMaxSpacing));
      for (plInt32 y = cy - 1; y <= cy + 1; ++y)
      {
        for (plInt32 x = cx - 1; x <= cx + 1; ++x)
        {
          const plDynamicArray<plUInt32>* pCell = cells.GetValue(Key(x, y));
          if (pCell == nullptr)
            continue;

          for (plUInt32 j : *pCell)
          {
            if (j <= i)
              continue;

            const float fPair = plVegetationScatter::GetPairSpacing(species[points[i].m_uiSpecies].m_fMinSpacing, species[points[j].m_uiSpecies].m_fMinSpacing);
            if (fPair <= 0.0f)
              continue;

            fMinRatio = plMath::Min(fMinRatio, (points[i].m_vPosition - points[j].m_vPosition).GetLength() / fPair);
          }
        }
      }
    }

    return fMinRatio;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Vegetation, ScatterSpacing)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "One type keeps its spacing")
  {
    plVegetationScatterSpecies species;
    species.m_fMinSpacing = 3.0f;

    plVegetationScatterDesc desc;
    desc.m_vMin = plVec2(-100.0f);
    desc.m_vMax = plVec2(100.0f);
    desc.m_fDensity = 0.06f; // 600 per hectare
    desc.m_uiSeed = 7;
    desc.m_Species = plMakeArrayPtr(&species, 1);

    plDynamicArray<plVegetationScatterPoint> points;
    plVegetationScatter::Scatter(desc, points);

    PL_TEST_BOOL(points.GetCount() > 2000);
    PL_TEST_BOOL(MinSpacingRatio(points, desc.m_Species) >= 0.9999f);

    for (const auto& p : points)
    {
      PL_TEST_BOOL(p.m_vPosition.x >= desc.m_vMin.x && p.m_vPosition.y >= desc.m_vMin.y && p.m_vPosition.x <= desc.m_vMax.x && p.m_vPosition.y <= desc.m_vMax.y);
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mixed types keep the mean of their spacings")
  {
    plVegetationScatterSpecies species[3];
    species[0].m_fMinSpacing = 6.0f;
    species[0].m_fWeight = 1.0f;
    species[1].m_fMinSpacing = 1.0f;
    species[1].m_fWeight = 3.0f;
    species[2].m_fMinSpacing = 2.5f;
    species[2].m_fWeight = 0.0f; // never picked

    plVegetationScatterDesc desc;
    desc.m_vMin = plVec2(0.0f);
    desc.m_vMax = plVec2(150.0f, 120.0f);
    desc.m_fDensity = 0.1f;
    desc.m_uiSeed = 99;
    desc.m_Species = plMakeArrayPtr(species, 3);

    plDynamicArray<plVegetationScatterPoint> points;
    plVegetationScatter::Scatter(desc, points);

    PL_TEST_BOOL(!points.IsEmpty());
    PL_TEST_BOOL(MinSpacingRatio(points, desc.m_Species) >= 0.9999f);

    plUInt32 counts[3] = {};
    for (const auto& p : points)
      ++counts[p.m_uiSpecies];

    PL_TEST_INT(counts[2], 0);
    PL_TEST_BOOL(counts[0] > 0 && counts[1] > counts[0]);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Existing plants keep new ones away")
  {
    plVegetationScatterSpecies species;
    species.m_fMinSpacing = 2.0f;

    plDynamicArray<plVegetationScatterObstacle> obstacles;
    for (plInt32 y = 0; y < 10; ++y)
    {
      for (plInt32 x = 0; x < 10; ++x)
      {
        plVegetationScatterObstacle& o = obstacles.ExpandAndGetRef();
        o.m_vPosition = plVec2(x * 5.0f + 1.0f, y * 5.0f + 1.0f);
        o.m_fMinSpacing = 8.0f; // a big tree: new points keep (2 + 8) / 2 = 5 m
      }
    }

    plVegetationScatterDesc desc;
    desc.m_vMin = plVec2(0.0f);
    desc.m_vMax = plVec2(50.0f);
    desc.m_fDensity = 0.2f;
    desc.m_uiSeed = 3;
    desc.m_Species = plMakeArrayPtr(&species, 1);
    desc.m_Obstacles = obstacles;

    plDynamicArray<plVegetationScatterPoint> points;
    plVegetationScatter::Scatter(desc, points);

    for (const auto& p : points)
    {
      for (const auto& o : obstacles)
      {
        PL_TEST_BOOL((p.m_vPosition - o.m_vPosition).GetLength() >= 5.0f - 0.0001f);
      }
    }
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, ScatterDensity)
{
  plVegetationScatterSpecies species;
  species.m_fMinSpacing = 2.0f;

  plVegetationScatterDesc desc;
  desc.m_vMin = plVec2(-150.0f);
  desc.m_vMax = plVec2(150.0f);
  desc.m_fDensity = 0.05f; // 500 per hectare over 9 ha: 4500
  desc.m_uiSeed = 1234;
  desc.m_Species = plMakeArrayPtr(&species, 1);

  plDynamicArray<plVegetationScatterPoint> points;
  plVegetationScatter::Scatter(desc, points);

  const float fWanted = 4500.0f;
  PL_TEST_BOOL_MSG(plMath::Abs(points.GetCount() - fWanted) <= fWanted * 0.02f, "%u points for %u wanted", points.GetCount(), static_cast<plUInt32>(fWanted));

  // spread: every 30 m square of the 300 m region holds about a hundredth of the points
  plUInt32 counts[100] = {};
  for (const auto& p : points)
  {
    const plUInt32 x = plMath::Min(static_cast<plUInt32>((p.m_vPosition.x + 150.0f) / 30.0f), 9u);
    const plUInt32 y = plMath::Min(static_cast<plUInt32>((p.m_vPosition.y + 150.0f) / 30.0f), 9u);
    ++counts[y * 10 + x];
  }
  for (plUInt32 c : counts)
  {
    PL_TEST_BOOL_MSG(c >= 30 && c <= 60, "%u points in one 30 m square", c);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Region mask")
  {
    plVegetationScatterDesc masked = desc;
    masked.m_RegionMask = [](const plVec2& v) -> bool
    { return v.GetLength() <= 100.0f; };

    plDynamicArray<plVegetationScatterPoint> inside;
    plVegetationScatter::Scatter(masked, inside);

    PL_TEST_BOOL(!inside.IsEmpty());
    for (const auto& p : inside)
      PL_TEST_BOOL(p.m_vPosition.GetLength() <= 100.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Accept callback and cap")
  {
    plVegetationScatterDesc capped = desc;
    capped.m_uiMaxPoints = 300;

    plUInt32 uiAsked = 0;
    capped.m_Accept = [&](const plVec2& v, plUInt32) -> bool
    {
      ++uiAsked;
      return v.x > 0.0f; // "ground" only on one half
    };

    plDynamicArray<plVegetationScatterPoint> half;
    plVegetationScatter::Scatter(capped, half);

    // the strata reach about half, the top-up darts most of the rest; never more than the cap
    PL_TEST_BOOL_MSG(half.GetCount() <= 300 && half.GetCount() >= 250, "%u points", half.GetCount());
    PL_TEST_BOOL_MSG(uiAsked <= 9 * 300 + 400, "%u candidates", uiAsked); // strata plus top-up darts, bounded
    for (const auto& p : half)
      PL_TEST_BOOL(p.m_vPosition.x > 0.0f);
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, ScatterClearings)
{
  plVegetationScatterSpecies species[2];
  species[0].m_fMinSpacing = 1.5f;
  species[0].m_fClearingAvoidance = 0.0f;
  species[1].m_fMinSpacing = 3.0f;
  species[1].m_fClearingAvoidance = 4.0f;

  plVegetationScatterClearing clearings[3];
  clearings[0].m_vCenter = plVec2(20.0f, 20.0f);
  clearings[0].m_fRadius = 10.0f;
  clearings[1].m_vCenter = plVec2(-40.0f, 10.0f);
  clearings[1].m_fRadius = 15.0f;
  clearings[2].m_vCenter = plVec2(500.0f, 500.0f); // far outside
  clearings[2].m_fRadius = 5.0f;

  plVegetationScatterDesc desc;
  desc.m_vMin = plVec2(-80.0f);
  desc.m_vMax = plVec2(80.0f);
  desc.m_fDensity = 0.08f;
  desc.m_uiSeed = 5;
  desc.m_Species = plMakeArrayPtr(species, 2);
  desc.m_Clearings = plMakeArrayPtr(clearings, 3);

  plDynamicArray<plVegetationScatterPoint> points;
  plVegetationScatter::Scatter(desc, points);

  PL_TEST_BOOL(points.GetCount() > 1000);

  plUInt32 uiNearEdge = 0;
  for (const auto& p : points)
  {
    for (const auto& c : clearings)
    {
      const float fDistance = (p.m_vPosition - c.m_vCenter).GetLength();
      PL_TEST_BOOL(fDistance >= c.m_fRadius + species[p.m_uiSpecies].m_fClearingAvoidance - 0.0001f);

      if (p.m_uiSpecies == 0 && fDistance < c.m_fRadius + 4.0f)
        ++uiNearEdge;
    }
  }

  // the type without avoidance still grows up to the edge
  PL_TEST_BOOL(uiNearEdge > 0);
}

PL_CREATE_SIMPLE_TEST(Vegetation, ScatterDeterminism)
{
  plVegetationScatterSpecies species[2];
  species[0].m_fMinSpacing = 2.0f;
  species[1].m_fMinSpacing = 4.0f;
  species[1].m_fWeight = 0.5f;

  plVegetationScatterDesc desc;
  desc.m_vMin = plVec2(0.0f);
  desc.m_vMax = plVec2(120.0f);
  desc.m_fDensity = 0.07f;
  desc.m_uiSeed = 42;
  desc.m_Species = plMakeArrayPtr(species, 2);

  plDynamicArray<plVegetationScatterPoint> a, b, c;
  plVegetationScatter::Scatter(desc, a);
  plVegetationScatter::Scatter(desc, b);
  desc.m_uiSeed = 43;
  plVegetationScatter::Scatter(desc, c);

  PL_TEST_INT(a.GetCount(), b.GetCount());
  bool bSame = a.GetCount() == b.GetCount();
  for (plUInt32 i = 0; bSame && i < a.GetCount(); ++i)
    bSame = a[i].m_vPosition == b[i].m_vPosition && a[i].m_uiSpecies == b[i].m_uiSpecies;
  PL_TEST_BOOL(bSame);

  bool bDiffers = a.GetCount() != c.GetCount();
  for (plUInt32 i = 0; !bDiffers && i < a.GetCount(); ++i)
    bDiffers = a[i].m_vPosition != c[i].m_vPosition;
  PL_TEST_BOOL(bDiffers);
}

PL_CREATE_SIMPLE_TEST(Vegetation, ScatterForestScale)
{
  // 500 m x 500 m at 600 trees per hectare: 15000 trees of three types
  plVegetationScatterSpecies species[3];
  species[0].m_fMinSpacing = 3.5f;
  species[0].m_fWeight = 2.0f;
  species[1].m_fMinSpacing = 2.5f;
  species[2].m_fMinSpacing = 1.5f;
  species[2].m_fWeight = 0.5f;

  plVegetationScatterClearing clearing;
  clearing.m_vCenter = plVec2(250.0f, 250.0f);
  clearing.m_fRadius = 30.0f;

  plVegetationScatterDesc desc;
  desc.m_vMin = plVec2(0.0f);
  desc.m_vMax = plVec2(500.0f);
  desc.m_fDensity = 0.06f;
  desc.m_uiSeed = 2026;
  desc.m_Species = plMakeArrayPtr(species, 3);
  desc.m_Clearings = plMakeArrayPtr(&clearing, 1);

  const plTime start = plTime::Now();
  plDynamicArray<plVegetationScatterPoint> points;
  plVegetationScatter::Scatter(desc, points);
  const plTime duration = plTime::Now() - start;

  plLog::Info("Forest scale: {} points in {} ms", points.GetCount(), plArgF(duration.GetMilliseconds(), 1));

  PL_TEST_BOOL_MSG(plMath::Abs(points.GetCount() - 15000.0f) <= 15000.0f * 0.02f, "%u points", points.GetCount());
  PL_TEST_BOOL(MinSpacingRatio(points, desc.m_Species) >= 0.9999f);
  PL_TEST_BOOL_MSG(duration.GetSeconds() < 2.0, "%.0f ms", duration.GetMilliseconds());
}

PL_CREATE_SIMPLE_TEST(Vegetation, TypeFileVersions)
{
  plVegetationTypeResourceDescriptor original;
  original.m_Lods.ExpandAndGetRef().m_sMesh = "{ 01234567-89ab-cdef-0123-456789abcdef }";
  original.m_Materials.PushBack("");
  original.m_Placement.m_fMinScale = 0.5f;
  original.m_Placement.m_fMaxScale = 2.0f;
  original.m_Placement.m_fMinSpacing = 4.0f;
  original.m_Placement.m_MaxSlope = plAngle::MakeFromDegree(30.0f);
  original.m_Placement.m_fSink = 0.2f;
  original.m_Placement.m_fAlignToNormal = 0.25f;
  original.m_Render.m_fCullDistance = 300.0f;
  original.m_RayTracing.m_Mode = plVegetationRayTracingMode::Included;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 2 loads with the version 3 defaults")
  {
    // the version 2 layout, as 0.4.1 wrote it
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer << static_cast<plUInt8>(2);
    writer << original.m_Lods.GetCount();
    for (const auto& lod : original.m_Lods)
      writer << lod.m_sMesh << lod.m_fScreenSize;
    writer << original.m_Materials.GetCount();
    for (const auto& sMaterial : original.m_Materials)
      writer << sMaterial;
    original.m_Wind.Save(writer);
    original.m_Touch.Save(writer);
    original.m_Placement.Save(writer);
    original.m_Render.Save(writer);
    original.m_RayTracing.Save(writer);

    plVegetationTypeResourceDescriptor loaded;
    loaded.m_Placement.m_fDensityWeight = 5.0f; // must be reset
    loaded.m_Placement.m_fClearingAvoidance = 5.0f;

    plMemoryStreamReader reader(&storage);
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    PL_TEST_INT(reader.SkipBytes(1), 0); // everything read

    PL_TEST_FLOAT(loaded.m_Placement.m_fDensityWeight, 1.0f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fClearingAvoidance, 0.0f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fMinSpacing, 4.0f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fMinScale, 0.5f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fMaxScale, 2.0f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_MaxSlope.GetDegree(), 30.0f, 0.001f);
    PL_TEST_FLOAT(loaded.m_Render.m_fCullDistance, 300.0f, 0.0f);
    PL_TEST_BOOL(loaded.m_RayTracing.m_Mode == plVegetationRayTracingMode::Included);
    PL_TEST_STRING(loaded.m_Lods[0].m_sMesh, original.m_Lods[0].m_sMesh);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 3 round trip")
  {
    plVegetationTypeResourceDescriptor desc = original;
    desc.m_Placement.m_fDensityWeight = 2.5f;
    desc.m_Placement.m_fClearingAvoidance = 6.0f;

    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    desc.Save(writer);

    {
      plMemoryStreamReader versionReader(&storage);
      plUInt8 uiVersion = 0;
      versionReader >> uiVersion;
      PL_TEST_INT(uiVersion, 3);
    }

    plVegetationTypeResourceDescriptor loaded;
    plMemoryStreamReader reader(&storage);
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    PL_TEST_INT(reader.SkipBytes(1), 0);
    PL_TEST_FLOAT(loaded.m_Placement.m_fDensityWeight, 2.5f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fClearingAvoidance, 6.0f, 0.0f);
    PL_TEST_FLOAT(loaded.m_Placement.m_fMinSpacing, 4.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Unknown versions fail")
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer << static_cast<plUInt8>(4);

    plTestLogInterface log;
    plTestLogSystemScope logScope(&log);
    log.ExpectMessage("Unknown vegetation type version 4", plLogMsgType::ErrorMsg);

    plVegetationTypeResourceDescriptor loaded;
    plMemoryStreamReader reader(&storage);
    PL_TEST_BOOL(loaded.Load(reader).Failed());
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, LayerClearings)
{
  plVegetationLayerData data;
  auto& entry = data.GetOrAddEntry("tree");
  for (plInt32 i = 0; i < 20; ++i)
  {
    plVegetationLayerInstance& inst = entry.m_Instances.ExpandAndGetRef();
    inst.m_vPosition = plVec3(static_cast<float>(i), 0.0f, 3.0f);
    inst.m_qRotation = plQuat::MakeIdentity();
    inst.m_fScale = 1.0f;
    inst.m_uiColorSeed = static_cast<plUInt8>(i);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A layer without clearings stays version 1")
  {
    plDataBuffer blob;
    data.ToBuffer(blob);
    PL_TEST_INT(blob[0], 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Clearing stamp: adds a disc and removes the plants inside, in layer space")
  {
    plVegetationLayerData copy = data;

    plVegetationStrokeRequest request;
    request.m_uiMode = plVegetationBrushMode::Clearing;
    request.m_vCenter = plVec3(20.0f, 10.0f, 50.0f); // world; the layer sits at (10, 10, 0) and is scaled by 2
    request.m_fRadius = 6.0f;
    request.m_LayerTransform = plTransform(plVec3(10.0f, 10.0f, 0.0f), plQuat::MakeIdentity(), plVec3(2.0f));

    PL_TEST_BOOL(request.ApplyClearingStamp(copy));
    PL_TEST_INT(copy.m_Clearings.GetCount(), 1);
    PL_TEST_VEC2(copy.m_Clearings[0].m_vCenter, plVec2(5.0f, 0.0f), 0.0001f);
    PL_TEST_FLOAT(copy.m_Clearings[0].m_fRadius, 3.0f, 0.0001f);

    // layer x 2 .. 8 lies within 3 layer units of x = 5, whatever the height
    PL_TEST_INT(copy.GetInstanceCount(), 20 - 7);
    for (const auto& inst : copy.m_Entries[0].m_Instances)
      PL_TEST_BOOL(plMath::Abs(inst.m_vPosition.x - 5.0f) > 3.0f);

    // the same stamp again adds nothing
    PL_TEST_BOOL(!request.ApplyClearingStamp(copy));

    // round trip as version 2
    plDataBuffer blob;
    copy.ToBuffer(blob);
    PL_TEST_INT(blob[0], 2);

    plVegetationLayerData loaded;
    PL_TEST_BOOL(loaded.FromBuffer(blob).Succeeded());
    PL_TEST_INT(loaded.m_Clearings.GetCount(), 1);
    PL_TEST_INT(loaded.GetInstanceCount(), 13);

    // Shift-drag removes it again
    request.m_bRemoveClearings = true;
    PL_TEST_BOOL(request.ApplyClearingStamp(loaded));
    PL_TEST_INT(loaded.m_Clearings.GetCount(), 0);
    PL_TEST_INT(loaded.GetInstanceCount(), 13);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "AddClearing skips covered discs")
  {
    plVegetationLayerData d;
    PL_TEST_BOOL(d.AddClearing(plVec2(0.0f), 10.0f));
    PL_TEST_BOOL(!d.AddClearing(plVec2(2.0f, 0.0f), 5.0f));
    PL_TEST_BOOL(d.AddClearing(plVec2(8.0f, 0.0f), 5.0f));
    PL_TEST_INT(d.m_Clearings.GetCount(), 2);
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, StrokeRequestVersions)
{
  plVegetationStrokeRequest request;
  request.m_uiMode = plVegetationBrushMode::Paint;
  request.m_fRadius = 7.0f;
  request.m_bUseTypeRules = true;
  request.m_vFillMin = plVec3(1, 2, 3);
  request.m_vFillMax = plVec3(4, 5, 6);
  request.m_fTreesPerHectare = 450.0f;
  request.m_Types.PushBack("tree");

  plDataBuffer buffer;
  request.ToBuffer(buffer);

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 5 round trip")
  {
    plVegetationStrokeRequest read;
    PL_TEST_BOOL(read.FromBuffer(buffer).Succeeded());
    PL_TEST_BOOL(read.m_bUseTypeRules);
    PL_TEST_VEC3(read.m_vFillMax, plVec3(4, 5, 6), 0.0f);
    PL_TEST_FLOAT(read.m_fTreesPerHectare, 450.0f, 0.0f);
    PL_TEST_FLOAT(read.m_fRadius, 7.0f, 0.0f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "A version 4 request still reads and places with the brush values")
  {
    // version 4 is version 5 without the trailing fields
    constexpr plUInt32 uiVersion5Bytes = sizeof(bool) + 2 * sizeof(plVec3) + sizeof(float) + sizeof(bool);
    plDataBuffer old = buffer;
    old.SetCount(old.GetCount() - uiVersion5Bytes);
    old[0] = 4;

    plVegetationStrokeRequest read;
    PL_TEST_BOOL(read.FromBuffer(old).Succeeded());
    PL_TEST_BOOL(!read.m_bUseTypeRules);
    PL_TEST_FLOAT(read.m_fRadius, 7.0f, 0.0f);
    PL_TEST_INT(read.m_Types.GetCount(), 1);
  }
}
