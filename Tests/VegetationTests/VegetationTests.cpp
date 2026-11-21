#include <Foundation/Basics.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Time/Time.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestLogInterface.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/Placement/VegetationScatter.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>
#include <VegetationPlugin/World/VegetationCellShell.h>

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
    constexpr plUInt32 uiVersion5Bytes = sizeof(bool) + 2 * sizeof(plVec3) + sizeof(float) + sizeof(bool) + sizeof(bool);
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

PL_CREATE_SIMPLE_TEST(Vegetation, GrassFill)
{
  plGrassCoverageData grass;
  grass.m_fTexelSize = 0.5f;

  PL_TEST_BLOCK(plTestBlock::Enabled, "Texel rect holds the texels whose centres lie inside")
  {
    plInt32 x0, y0, x1, y1;
    PL_TEST_BOOL(grass.GetTexelRect(plVec2(0.0f), plVec2(10.0f, 5.0f), x0, y0, x1, y1));
    PL_TEST_INT(x0, 0);
    PL_TEST_INT(y0, 0);
    PL_TEST_INT(x1, 19);
    PL_TEST_INT(y1, 9);

    PL_TEST_BOOL(grass.GetTexelRect(plVec2(-1.1f, 0.2f), plVec2(-0.2f, 0.7f), x0, y0, x1, y1)); // centres -0.75, -0.25 and 0.25
    PL_TEST_INT(x0, -2);
    PL_TEST_INT(x1, -1);
    PL_TEST_INT(y0, 0);
    PL_TEST_INT(y1, 0);

    PL_TEST_BOOL(!grass.GetTexelRect(plVec2(0.3f), plVec2(0.4f), x0, y0, x1, y1));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Fill writes every accepted texel; the sampler excludes a clearing and steep ground")
  {
    const plVec2 vClearing(5.0f, 5.0f);
    const plGrassCoverageData::SampleFunc sampler = [&](const plVec2& v, plGrassCoverageSample& out_sample) -> bool
    {
      if ((v - vClearing).GetLength() < 2.0f)
        return false;

      out_sample.m_fHeight = 3.0f + v.x * 0.01f;
      out_sample.m_vNormal.Set(0, 0, 1);
      return v.y < 9.0f; // "too steep" above 9 m
    };

    plInt32 x0, y0, x1, y1;
    PL_TEST_BOOL(grass.GetTexelRect(plVec2(0.0f), plVec2(10.0f), x0, y0, x1, y1));
    const plUInt32 uiWritten = grass.PaintTexelRect("grass", x0, y0, x1, y1, 0.8f, 0.5f, sampler);

    plUInt32 uiExpected = 0;
    for (plInt32 y = y0; y <= y1; ++y)
    {
      for (plInt32 x = x0; x <= x1; ++x)
      {
        const plVec2 c = grass.GetTexelCenter(x, y);
        if ((c - vClearing).GetLength() >= 2.0f && c.y < 9.0f)
          ++uiExpected;
      }
    }

    PL_TEST_INT(uiWritten, uiExpected);
    PL_TEST_BOOL(grass.GetPaintedArea() > 0.0);

    // density and length as asked, nothing in the clearing
    const plGrassCoverageEntry* pEntry = grass.FindEntry("grass");
    PL_TEST_BOOL(pEntry != nullptr);
    plUInt32 uiPainted = 0;
    for (auto it = pEntry->m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const plGrassCoverageTile& tile = it.Value();
      for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      {
        if (tile.m_Texels[i].m_uiDensity == 0)
          continue;

        ++uiPainted;
        PL_TEST_INT(tile.m_Texels[i].m_uiDensity, 204);
        PL_TEST_INT(tile.m_Texels[i].m_uiLength, 128);
        const plVec2 c = grass.GetTexelCenter(tile.m_iX * 16 + static_cast<plInt32>(i % 16), tile.m_iY * 16 + static_cast<plInt32>(i / 16));
        PL_TEST_BOOL((c - vClearing).GetLength() >= 2.0f);
        PL_TEST_FLOAT(tile.GetTexelHeight(i), 3.0f + c.x * 0.01f, 0.002f);
      }
    }
    PL_TEST_INT(uiPainted, uiExpected);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Fill with Replace clears the fill types in the region only")
  {
    plGrassCoverageData copy = grass;
    plInt32 x0, y0, x1, y1;
    copy.GetTexelRect(plVec2(0.0f), plVec2(10.0f), x0, y0, x1, y1);
    copy.PaintTexelRect("other", x0, y0, x1, y1, 1.0f, 1.0f);

    plVegetationLayerData data;
    for (plInt32 i = 0; i < 10; ++i)
    {
      plVegetationLayerInstance& a = data.GetOrAddEntry("tree").m_Instances.ExpandAndGetRef();
      a.m_vPosition = plVec3(i * 2.0f, 1.0f, 0.0f);
      plVegetationLayerInstance& b = data.GetOrAddEntry("bush").m_Instances.ExpandAndGetRef();
      b.m_vPosition = plVec3(i * 2.0f, 1.0f, 0.0f);
    }

    plVegetationStrokeRequest request;
    request.m_uiMode = plVegetationBrushMode::Fill;
    request.m_vFillMin = plVec3(0.0f, 0.0f, -5.0f);
    request.m_vFillMax = plVec3(5.0f, 5.0f, 5.0f);
    request.m_Types.PushBack("tree");
    request.m_GrassTypes.PushBack("grass");

    PL_TEST_BOOL(!request.ApplyFillReplace(data, &copy)); // only with Replace
    request.m_bFillReplace = true;
    PL_TEST_BOOL(request.ApplyFillReplace(data, &copy));

    PL_TEST_INT(data.FindEntry("tree")->m_Instances.GetCount(), 7); // x = 0, 2, 4 went
    PL_TEST_INT(data.FindEntry("bush")->m_Instances.GetCount(), 10);

    for (const plGrassCoverageEntry& entry : copy.m_Entries)
    {
      for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
      {
        const plGrassCoverageTile& tile = it.Value();
        for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
        {
          if (tile.m_Texels[i].m_uiDensity == 0)
            continue;

          const plVec2 c = copy.GetTexelCenter(tile.m_iX * 16 + static_cast<plInt32>(i % 16), tile.m_iY * 16 + static_cast<plInt32>(i / 16));
          if (entry.m_sType == "grass")
            PL_TEST_BOOL(c.x > 5.0f || c.y > 5.0f);
        }
      }
    }

    PL_TEST_FLOAT(static_cast<float>(copy.GetPaintedArea(*copy.FindEntry("other"))), 100.0f, 0.01f);
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, LayerFillProperties)
{
  // the names the scripted editor sets with object_modify
  const plRTTI* pType = plGetStaticRTTI<plVegetationLayerComponent>();
  for (const char* szName : {"FillSpecies", "FillTreesPerHectare", "FillGrassDensity", "FillGrassLength", "FillGrassMaxSlope", "FillCenter", "FillExtent", "FillSeed", "FillReplace"})
  {
    PL_TEST_BOOL_MSG(pType->FindPropertyByName(szName) != nullptr, "%s", szName);
  }

  const plRTTI* pSpecies = plGetStaticRTTI<plVegetationFillSpecies>();
  PL_TEST_BOOL(pSpecies->FindPropertyByName("Type") != nullptr);
  PL_TEST_BOOL(pSpecies->FindPropertyByName("Weight") != nullptr);
}

namespace
{
  /// A closed box of 12 triangles: slot 0 on the lower half (bark), slot 1 on the upper half (leaves).
  plVegetationImpostorBakeMesh MakeBoxMesh(const plVec3& vHalf, float fZOffset)
  {
    plVegetationImpostorBakeMesh mesh;
    for (plUInt32 c = 0; c < 8; ++c)
      mesh.m_Positions.PushBack(plVec3((c & 1) ? vHalf.x : -vHalf.x, (c & 2) ? vHalf.y : -vHalf.y, ((c & 4) ? vHalf.z : -vHalf.z) + fZOffset));

    const plUInt32 faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (plUInt32 f = 0; f < 6; ++f)
    {
      for (plUInt32 t = 0; t < 2; ++t)
      {
        mesh.m_Indices.PushBack(faces[f][0]);
        mesh.m_Indices.PushBack(faces[f][t + 1]);
        mesh.m_Indices.PushBack(faces[f][t + 2]);
        mesh.m_TriangleSlots.PushBack(f == 1 ? 1 : 0);
      }
    }

    mesh.m_Slots.SetCount(2);
    mesh.m_Slots[0].m_BaseColor = plColor(0.3f, 0.2f, 0.1f);
    mesh.m_Slots[1].m_BaseColor = plColor(0.1f, 0.6f, 0.1f);
    mesh.m_Slots[1].m_fTranslucency = 0.8f;
    return mesh;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Vegetation, CellShell)
{
  // A dense 2 m box crown, sampled at 0.25 m.
  const plVegetationImpostorBakeMesh mesh = MakeBoxMesh(plVec3(1.0f, 1.0f, 1.0f), 3.0f);
  plSharedPtr<plVegetationVoxelProxy> pProxy = PL_DEFAULT_NEW(plVegetationVoxelProxy);
  PL_TEST_BOOL(plVegetationImpostorBaker::BakeVoxelProxy(mesh, 0.25f, *pProxy).Succeeded());

  PL_TEST_BLOCK(plTestBlock::Enabled, "Voxel proxy")
  {
    PL_TEST_BOOL(pProxy->IsValid());
    float fArea = 0.0f;
    for (const plVegetationVoxelProxy::Voxel& v : pProxy->m_Voxels)
    {
      fArea += v.m_fArea;
      // Normals point out of the crown.
      PL_TEST_BOOL(v.m_vNormal.Dot(v.m_vCenter - plVec3(0, 0, 3)) >= -0.001f);
    }
    // Six faces of 4 square metres.
    PL_TEST_FLOAT(fArea, 24.0f, 0.01f);

    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    pProxy->Save(writer);
    plMemoryStreamReader reader(&storage);
    plVegetationVoxelProxy loaded;
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    PL_TEST_INT(loaded.m_Voxels.GetCount(), pProxy->m_Voxels.GetCount());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Overlapping trees share one shell")
  {
    plVegetationCellShellSettings settings;
    settings.m_fVoxelSize = 0.5f;

    plVegetationCellShellTree tree;
    tree.m_pProxy = pProxy.Borrow();
    tree.m_Transform = plTransform::MakeIdentity();

    plMeshResourceDescriptor one;
    plVegetationCellShellStats oneStats;
    PL_TEST_BOOL(plVegetationCellShellBuilder::Build(plArrayPtr<const plVegetationCellShellTree>(&tree, 1), plVec3::MakeZero(), settings, one, &oneStats).Succeeded());
    PL_TEST_INT(oneStats.m_uiTrees, 1);
    PL_TEST_BOOL(oneStats.m_uiFaces > 0);

    // Two trees half a crown apart: the faces between them are hidden, so fewer than twice as many remain.
    plVegetationCellShellTree two[2] = {tree, tree};
    two[1].m_Transform.m_vPosition = plVec3(1.0f, 0.0f, 0.0f);
    plMeshResourceDescriptor merged;
    plVegetationCellShellStats mergedStats;
    PL_TEST_BOOL(plVegetationCellShellBuilder::Build(plArrayPtr<const plVegetationCellShellTree>(two, 2), plVec3::MakeZero(), settings, merged, &mergedStats).Succeeded());
    PL_TEST_INT(mergedStats.m_uiTrees, 2);
    PL_TEST_BOOL(mergedStats.m_uiFaces < oneStats.m_uiFaces * 2);
    PL_TEST_INT(merged.MeshBufferDesc().GetPrimitiveCount(), mergedStats.m_uiFaces * 2);
    PL_TEST_BOOL(merged.MeshBufferDesc().GetVertexStreamConfig().HasColor0());

    // No proxies, no shell.
    plVegetationCellShellTree empty;
    plMeshResourceDescriptor none;
    PL_TEST_BOOL(plVegetationCellShellBuilder::Build(plArrayPtr<const plVegetationCellShellTree>(&empty, 1), plVec3::MakeZero(), settings, none).Failed());
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, ImpostorMath)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Hemi-octahedral mapping round trip")
  {
    for (plUInt32 i = 0; i <= 16; ++i)
    {
      for (plUInt32 j = 0; j <= 16; ++j)
      {
        const plVec2 e(i / 8.0f - 1.0f, j / 8.0f - 1.0f);
        const plVec3 d = plVegetationImpostorMath::HemiOctDecode(e);
        PL_TEST_BOOL(d.z >= 0.0f);
        PL_TEST_FLOAT(d.GetLength(), 1.0f, 0.0001f);
        const plVec2 back = plVegetationImpostorMath::HemiOctEncode(d);
        PL_TEST_FLOAT(back.x, e.x, 0.0001f);
        PL_TEST_FLOAT(back.y, e.y, 0.0001f);
      }
    }

    // The grid's centre view looks straight down, its corners along the horizon.
    PL_TEST_BOOL(plVegetationImpostorMath::GetViewDirection(4, 4, 9).IsEqual(plVec3(0, 0, 1), 0.0001f));
    PL_TEST_FLOAT(plVegetationImpostorMath::GetViewDirection(0, 0, 9).z, 0.0f, 0.0001f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "View basis")
  {
    for (plUInt32 i = 0; i < 8; ++i)
    {
      for (plUInt32 j = 0; j < 8; ++j)
      {
        const plVec3 w = plVegetationImpostorMath::GetViewDirection(i, j, 8);
        plVec3 r, u;
        plVegetationImpostorMath::GetViewBasis(w, r, u);
        PL_TEST_FLOAT(r.GetLength(), 1.0f, 0.0001f);
        PL_TEST_FLOAT(u.GetLength(), 1.0f, 0.0001f);
        PL_TEST_FLOAT(r.Dot(w), 0.0f, 0.0001f);
        PL_TEST_FLOAT(u.Dot(w), 0.0f, 0.0001f);
        PL_TEST_FLOAT(r.Dot(u), 0.0f, 0.0001f);
        // Up leans toward world up except for the view from straight above.
        if (w.z < 0.99f)
          PL_TEST_BOOL(u.z > 0.0f);
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Normal encoding round trip")
  {
    for (plUInt32 i = 0; i < 64; ++i)
    {
      plVec3 n(plMath::Sin(plAngle::MakeFromRadian(i * 0.7f)), plMath::Cos(plAngle::MakeFromRadian(i * 1.3f)), plMath::Sin(plAngle::MakeFromRadian(i * 2.1f + 0.3f)));
      n.Normalize();
      const plVec3 back = plVegetationImpostorMath::OctDecodeNormal(plVegetationImpostorMath::OctEncodeNormal(n));
      PL_TEST_BOOL(back.IsEqual(n, 0.001f));
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Settings validation")
  {
    plVegetationImpostorSettings s;
    s.m_uiViews = 40;
    s.m_uiResolution = 100;
    const plVegetationImpostorSettings v = s.GetValidated();
    PL_TEST_INT(v.m_uiViews, 16);
    PL_TEST_INT(v.m_uiResolution, 96);
    PL_TEST_INT(plVegetationImpostorData::ComputeMipCount(192), 6);
    PL_TEST_INT(plVegetationImpostorData::ComputeMipCount(128), 6);
  }
}

PL_CREATE_SIMPLE_TEST(Vegetation, ImpostorBake)
{
  plVegetationImpostorSettings settings;
  settings.m_uiViews = 4;
  settings.m_uiResolution = 32;

  // A 2 m wide, 4 m tall box standing on the pivot.
  const plVegetationImpostorBakeMesh mesh = MakeBoxMesh(plVec3(1.0f, 1.0f, 2.0f), 2.0f);
  plVegetationImpostorData data;
  PL_TEST_BOOL(plVegetationImpostorBaker::Bake(mesh, settings, false, data).Succeeded());

  PL_TEST_BLOCK(plTestBlock::Enabled, "Layout and bounds")
  {
    PL_TEST_INT(data.m_uiViews, 4);
    PL_TEST_INT(data.m_uiResolution, 32);
    PL_TEST_BOOL(data.m_vCenter.IsEqual(plVec3(0, 0, 2), 0.0001f));
    PL_TEST_FLOAT(data.m_fRadius, plMath::Sqrt(6.0f) * 1.01f, 0.001f);
    PL_TEST_INT(data.m_Albedo.GetWidth(), 128);
    PL_TEST_INT(data.m_Albedo.GetNumMipLevels(), 4);
    PL_TEST_INT(data.m_NormalDepth.GetNumMipLevels(), 4);
    PL_TEST_BOOL(data.IsValid());
    // Slot 0 (sides and bottom) has more area than slot 1 (the top face): it shades the impostor.
    PL_TEST_INT(data.m_uiMaterialSlot, 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Coverage, depth and normal of the side view")
  {
    // View (0, 3) looks along the horizon; its tile centre shows the middle of a side face.
    const plVec3 w = plVegetationImpostorMath::GetViewDirection(0, 3, 4);
    PL_TEST_FLOAT(w.z, 0.0f, 0.0001f);

    const plUInt32 x = 0 * 32 + 16, y = 3 * 32 + 16;
    const plUInt8* pA = data.m_Albedo.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
    const plUInt8* pB = data.m_NormalDepth.GetPixelPointer<plUInt8>(0, 0, 0, x, y);
    PL_TEST_INT(pA[3], 255);
    // A side face of the box is in front of the centre: depth above the middle of the range.
    PL_TEST_BOOL(pB[2] > 128);
    // Bark: no translucency.
    PL_TEST_INT(pB[3], 0);

    const plVec3 n = plVegetationImpostorMath::OctDecodeNormal(plVec2(pB[0] / 255.0f * 2.0f - 1.0f, pB[1] / 255.0f * 2.0f - 1.0f));
    PL_TEST_BOOL(n.Dot(w) > 0.5f);

    // The tile's corner lies outside the box's silhouette.
    const plUInt8* pCorner = data.m_Albedo.GetPixelPointer<plUInt8>(0, 0, 0, 0 * 32 + 1, 3 * 32 + 1);
    PL_TEST_INT(pCorner[3], 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "The view from above shows the leaves")
  {
    // The top view sits between grid points of an even grid: use a 3x3 grid, whose centre is the top.
    plVegetationImpostorSettings top = settings;
    top.m_uiViews = 3;
    plVegetationImpostorData topData;
    PL_TEST_BOOL(plVegetationImpostorBaker::Bake(mesh, top, false, topData).Succeeded());
    const plUInt8* pB = topData.m_NormalDepth.GetPixelPointer<plUInt8>(0, 0, 0, 32 + 16, 32 + 16);
    PL_TEST_BOOL(pB[3] > 190);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Mips keep the coverage")
  {
    plUInt32 uiCovered0 = 0, uiCoveredLast = 0;
    const plUInt32 uiLast = data.m_Albedo.GetNumMipLevels() - 1;
    const plUInt32 uiTile0 = 32, uiTileLast = 32 >> uiLast;
    for (plUInt32 y = 0; y < uiTile0; ++y)
      for (plUInt32 x = 0; x < uiTile0; ++x)
        uiCovered0 += data.m_Albedo.GetPixelPointer<plUInt8>(0, 0, 0, x, 3 * uiTile0 + y)[3] >= 128 ? 1 : 0;
    for (plUInt32 y = 0; y < uiTileLast; ++y)
      for (plUInt32 x = 0; x < uiTileLast; ++x)
        uiCoveredLast += data.m_Albedo.GetPixelPointer<plUInt8>(uiLast, 0, 0, x, 3 * uiTileLast + y)[3] >= 128 ? 1 : 0;

    const float fShare0 = static_cast<float>(uiCovered0) / (uiTile0 * uiTile0);
    const float fShareLast = static_cast<float>(uiCoveredLast) / (uiTileLast * uiTileLast);
    PL_TEST_FLOAT(fShareLast, fShare0, 0.2f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Type file round trip")
  {
    plVegetationTypeResourceDescriptor desc;
    desc.m_Impostor = settings;
    desc.m_Impostor.m_fDistance = 95.0f;
    desc.m_pImpostorData = PL_DEFAULT_NEW(plVegetationImpostorData);
    PL_TEST_BOOL(plVegetationImpostorBaker::Bake(mesh, settings, false, *desc.m_pImpostorData).Succeeded());

    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    desc.Save(writer);

    plMemoryStreamReader reader(&storage);
    plVegetationTypeResourceDescriptor loaded;
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    PL_TEST_FLOAT(loaded.m_Impostor.m_fDistance, 95.0f, 0.0f);
    PL_TEST_BOOL(loaded.m_pImpostorData != nullptr);
    if (loaded.m_pImpostorData != nullptr)
    {
      PL_TEST_INT(loaded.m_pImpostorData->m_uiViews, 4);
      PL_TEST_FLOAT(loaded.m_pImpostorData->m_fRadius, desc.m_pImpostorData->m_fRadius, 0.0f);
      PL_TEST_INT(loaded.m_pImpostorData->m_Albedo.GetNumMipLevels(), desc.m_pImpostorData->m_Albedo.GetNumMipLevels());
      PL_TEST_BOOL(plMemoryUtils::IsEqual(loaded.m_pImpostorData->m_Albedo.GetPixelPointer<plUInt8>(), desc.m_pImpostorData->m_Albedo.GetPixelPointer<plUInt8>(), 128 * 128 * 4));
    }
  }
}
