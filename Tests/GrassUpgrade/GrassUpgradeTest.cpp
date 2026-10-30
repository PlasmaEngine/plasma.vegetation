#include <Foundation/Basics.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/StringDeduplicationContext.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

#include <Shaders/Vegetation/Grass/GrassConstants.h>
#include <Shaders/Vegetation/Grass/GrassGpuData.h>

PL_TESTFRAMEWORK_ENTRY_POINT("GrassUpgradeTest", "Procedural grass tests")
PL_CREATE_SIMPLE_TEST_GROUP(GrassUpgrade);

PL_CREATE_SIMPLE_TEST(GrassUpgrade, NestedLayerBlobs)
{
  plVegetationLayerData vegetation;
  auto& instance = vegetation.GetOrAddEntry("tree-type").m_Instances.ExpandAndGetRef();
  instance.m_vPosition = plVec3(1, 2, 3);
  instance.m_qRotation = plQuat::MakeIdentity();
  instance.m_fScale = 1.0f;
  instance.m_uiColorSeed = 42;
  plGrassCoverageData grass;
  plGrassCoverageTile tile;
  tile.m_Texels[0].m_uiDensity = 255;
  grass.GetOrAddEntry("grass-type").m_Tiles.Insert(plGrassCoverageTile::MakeKey(0, 0), tile);

  plDataBuffer vegetationBlob, grassBlob, nestedVegetationBlob, nestedGrassBlob;
  vegetation.ToBuffer(vegetationBlob);
  grass.ToBuffer(grassBlob);
  plDefaultMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  {
    plStringDeduplicationWriteContext context(writer);
    auto& stream = context.Begin();
    stream << plStringView("before");
    vegetation.ToBuffer(nestedVegetationBlob);
    grass.ToBuffer(nestedGrassBlob);
    PL_TEST_BOOL(plStringDeduplicationWriteContext::GetContext() == &context);
    stream << plStringView("after");
    PL_TEST_BOOL(context.End().Succeeded());
  }
  PL_TEST_BOOL(vegetationBlob == nestedVegetationBlob);
  PL_TEST_BOOL(grassBlob == nestedGrassBlob);
  plMemoryStreamReader reader(&storage);
  plStringDeduplicationReadContext context(reader);
  plString outer;
  reader >> outer;
  PL_TEST_STRING(outer, "before");
  plVegetationLayerData loadedVegetation;
  plGrassCoverageData loadedGrass;
  PL_TEST_BOOL(loadedVegetation.FromBuffer(vegetationBlob).Succeeded());
  PL_TEST_BOOL(loadedGrass.FromBuffer(grassBlob).Succeeded());
  PL_TEST_BOOL(plStringDeduplicationReadContext::GetContext() == &context);
  PL_TEST_INT(loadedVegetation.GetInstanceCount(), 1);
  PL_TEST_BOOL(loadedVegetation.FindEntry("tree-type") != nullptr);
  PL_TEST_INT(loadedGrass.GetTileCount(), 1);
  PL_TEST_BOOL(loadedGrass.FindEntry("grass-type") != nullptr);
  reader >> outer;
  PL_TEST_STRING(outer, "after");
}

PL_CREATE_SIMPLE_TEST(GrassUpgrade, ProgressiveResidency)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Nested root subsets preserve identities and match allocated counts")
  {
    for (plUInt32 seed = 0; seed < 64; ++seed)
    {
      const plUInt32 hash = GrassHash(-17, 32, seed, 42);
      for (plUInt32 count : {0u, 1u, 7u, 30u, 256u, 1000u})
      {
        for (plUInt32 stride = 1; stride <= 256; stride *= 2)
        {
          const plUInt32 resident = GrassResidentCount(count, hash, stride);
          plUInt32 enumerated = 0;
          for (plUInt32 original = 0; original < count; ++original)
          {
            if (((original ^ hash) & (stride - 1u)) == 0)
            {
              PL_TEST_INT(original, enumerated * stride + (hash & (stride - 1u)));
              if (stride > 1)
                PL_TEST_INT((original ^ hash) & (stride / 2 - 1), 0);
              ++enumerated;
            }
          }
          PL_TEST_INT(resident, enumerated);
        }
      }
    }
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Sparse coverage can disappear entirely at a coarse residency level")
  {
    PL_TEST_INT(GrassResidentCount(1, 255, 256), 0);
    PL_TEST_INT(GrassResidentCount(1, 0, 256), 1);
    PL_TEST_INT(GrassResidentCount(256, 255, 256), 1);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Nearby roots share clumps while distant cells and seeds vary")
  {
    plUInt32 nearbyMatches = 0;
    plUInt32 differentSeeds = 0;
    for (plInt32 x = -20; x <= 20; ++x)
    {
      const float root = float(x) * 0.31f;
      const plUInt32 clump = GrassClumpSeed(root, -0.25f, 1.5f, 17);
      PL_TEST_BOOL(clump <= 65535);
      nearbyMatches += clump == GrassClumpSeed(root + 0.001f, -0.25f, 1.5f, 17);
      differentSeeds += clump != GrassClumpSeed(root, -0.25f, 1.5f, 29);
    }
    PL_TEST_BOOL(nearbyMatches >= 39);
    PL_TEST_BOOL(differentSeeds >= 39);
    PL_TEST_BOOL(GrassClumpSeed(-10.0f, 0.0f, 1.5f, 17) != GrassClumpSeed(10.0f, 0.0f, 1.5f, 17));
  }
}

PL_CREATE_SIMPLE_TEST(GrassUpgrade, SettingsCompatibility)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 3 preserves the procedural settings")
  {
    plGrassTypeResourceDescriptor original;
    auto& settings = original.m_Settings;
    settings.m_bProceduralBlades = true;
    settings.m_fBladeWidth = 0.04f;
    settings.m_fBladeTilt = 0.4f;
    settings.m_fBladeBend = 0.7f;
    settings.m_fBladeSideCurve = 0.2f;
    settings.m_fClumpSize = 2.7f;
    settings.m_fClumpStrength = 0.8f;
    settings.m_fClumpColorVariation = 0.3f;
    settings.m_fBladeNormalRoundness = 0.9f;
    settings.m_bAdaptiveDensity = false;
    settings.m_fFarDensity = 0.025f;
    settings.m_bFarGroundCover = false;
    settings.m_fGroundCoverStartDistance = 80.0f;
    settings.m_fGroundCoverMaxSize = 0.5f;
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    original.Save(writer);
    plMemoryStreamReader reader(&storage);
    plGrassTypeResourceDescriptor loaded;
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    const auto& actual = loaded.m_Settings;
    PL_TEST_BOOL(actual.m_bProceduralBlades);
    PL_TEST_BOOL(!actual.m_bAdaptiveDensity);
    PL_TEST_FLOAT(actual.m_fBladeWidth, settings.m_fBladeWidth, 0.00001f);
    PL_TEST_FLOAT(actual.m_fBladeTilt, settings.m_fBladeTilt, 0.00001f);
    PL_TEST_FLOAT(actual.m_fBladeBend, settings.m_fBladeBend, 0.00001f);
    PL_TEST_FLOAT(actual.m_fBladeSideCurve, settings.m_fBladeSideCurve, 0.00001f);
    PL_TEST_FLOAT(actual.m_fClumpSize, settings.m_fClumpSize, 0.00001f);
    PL_TEST_FLOAT(actual.m_fClumpStrength, settings.m_fClumpStrength, 0.00001f);
    PL_TEST_FLOAT(actual.m_fClumpColorVariation, settings.m_fClumpColorVariation, 0.00001f);
    PL_TEST_FLOAT(actual.m_fBladeNormalRoundness, settings.m_fBladeNormalRoundness, 0.00001f);
    PL_TEST_FLOAT(actual.m_fFarDensity, settings.m_fFarDensity, 0.00001f);
    PL_TEST_BOOL(!actual.m_bFarGroundCover);
    PL_TEST_FLOAT(actual.m_fGroundCoverStartDistance, 80.0f, 0.00001f);
    PL_TEST_FLOAT(actual.m_fGroundCoverMaxSize, 0.5f, 0.00001f);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Version 2 cooked assets load as legacy cards")
  {
    // Write precisely the old stream layout, including a following sentinel to catch over-reading new fields.
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    writer << plUInt8(2) << 120.0f << plUInt32(3) << plUInt32(2) << plUInt32(1);
    writer << 1.0f << 0.1f << 0.2f << 1.0f << plUInt32(0) << plString();
    writer << 0.5f << 0.1f << 0.0f << 1.0f << true;
    writer << 0.6f << 1.5f << 200.0f << 40.0f << 15.0f << 25.0f << true << 8.0f;
    writer << plUInt32(0x12345678);
    plMemoryStreamReader reader(&storage);
    plGrassTypeResourceDescriptor loaded;
    loaded.m_Settings.m_bProceduralBlades = true;
    loaded.m_Settings.m_fClumpSize = 9.0f;
    PL_TEST_BOOL(loaded.Load(reader).Succeeded());
    PL_TEST_BOOL(!loaded.m_Settings.m_bProceduralBlades);
    PL_TEST_FLOAT(loaded.m_Settings.m_fClumpSize, 1.5f, 0.00001f);
    PL_TEST_INT(loaded.m_Settings.m_uiSegmentCount, 3);
    PL_TEST_INT(loaded.m_Settings.m_uiBillboardCount, 2);
    plUInt32 sentinel = 0;
    reader >> sentinel;
    PL_TEST_INT(sentinel, 0x12345678);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Sanitize and regrow distinguish layout from live shape controls")
  {
    plGrassSettings settings;
    const plGrassSettings defaults;
    settings.m_fBladeWidth = -1.0f;
    settings.m_fClumpSize = 0.0f;
    settings.m_fFarDensity = 0.0f;
    settings.Sanitize();
    PL_TEST_BOOL(settings.m_fBladeWidth > 0 && settings.m_fClumpSize > 0 && settings.m_fFarDensity > 0);
    settings = defaults;
    settings.m_fBladeBend = 0.7f;
    PL_TEST_BOOL(!settings.RequiresRegrow(defaults));
    settings.m_fClumpSize = 4.0f;
    PL_TEST_BOOL(settings.RequiresRegrow(defaults));
    settings = defaults;
    settings.m_bProceduralBlades = true;
    PL_TEST_BOOL(settings.RequiresRegrow(defaults));
  }
}
