#include <Foundation/Basics.h>
#include <TestFramework/Framework/TestFramework.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>

namespace
{
  constexpr const char* s_szMeadow = "{ 5b1c7e8e-7a3f-4a55-9d3e-0c1b2a3d4e5f }";
  constexpr const char* s_szDry = "{ 0d2e4f60-1a2b-4c3d-8e9f-a0b1c2d3e4f5 }";

  plGrassCoverageStamp MakeStamp(float x, float y, float fRadius, float fDensity, float fStrength = 1.0f, float fFalloff = 0.3f)
  {
    plGrassCoverageStamp stamp;
    stamp.m_vCenter.Set(x, y);
    stamp.m_fRadius = fRadius;
    stamp.m_fDensity = fDensity;
    stamp.m_fStrength = fStrength;
    stamp.m_fFalloff = fFalloff;
    stamp.m_fLength = 0.75f;
    return stamp;
  }

  plGrassCoverageData RoundTrip(const plGrassCoverageData& data)
  {
    plDataBuffer buffer;
    data.ToBuffer(buffer);

    plGrassCoverageData result;
    PL_TEST_BOOL(result.FromBuffer(buffer).Succeeded());
    return result;
  }

  bool SameContent(const plGrassCoverageData& a, const plGrassCoverageData& b)
  {
    plDataBuffer bufferA;
    plDataBuffer bufferB;
    a.ToBuffer(bufferA);
    b.ToBuffer(bufferB);
    return bufferA == bufferB;
  }

  plUInt64 CountAllStrands(const plGrassCoverageData& data)
  {
    plUInt64 uiCount = 0;
    for (const auto& entry : data.m_Entries)
    {
      uiCount += data.CountStrands(entry, 120.0f);
    }
    return uiCount;
  }
} // namespace

PL_CREATE_SIMPLE_TEST(Grass, Coverage)
{
  PL_TEST_BLOCK(plTestBlock::Enabled, "Paint, erase and thin round-trip through the blob")
  {
    plGrassCoverageData data;
    data.m_fTexelSize = 0.5f;

    auto Sampler = [](const plVec2& vPosition, plGrassCoverageSample& out_sample) -> bool
    {
      out_sample.m_fHeight = 1.0f + vPosition.x * 0.1f;
      out_sample.m_vNormal = plVec3(0.1f, 0.0f, 1.0f).GetNormalized();
      out_sample.m_bDiscontinuous = vPosition.y > 3.0f;
      return true;
    };

    PL_TEST_BOOL(data.PaintInDisc(s_szMeadow, MakeStamp(0, 0, 4, 0.8f), Sampler) > 0);
    PL_TEST_BOOL(data.PaintInDisc(s_szDry, MakeStamp(6, 0, 2, 1.0f)) > 0);
    PL_TEST_BOOL(SameContent(data, RoundTrip(data)));

    const plGrassCoverageData loaded = RoundTrip(data);
    PL_TEST_INT(loaded.m_Entries.GetCount(), 2);
    PL_TEST_FLOAT(loaded.m_fTexelSize, 0.5f, 0.0f);

    // heights survive at millimetre precision relative to the tile base
    const plGrassCoverageEntry* pMeadow = loaded.FindEntry(s_szMeadow);
    PL_TEST_BOOL(pMeadow != nullptr);
    if (pMeadow != nullptr)
    {
      for (auto it = pMeadow->m_Tiles.GetIterator(); it.IsValid(); ++it)
      {
        const plGrassCoverageTile& tile = it.Value();
        for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
        {
          if (tile.m_Texels[i].m_uiDensity == 0)
            continue;

          const plInt32 iX = tile.m_iX * 16 + static_cast<plInt32>(i % 16);
          const plVec2 vCenter = loaded.GetTexelCenter(iX, 0);
          PL_TEST_FLOAT(tile.GetTexelHeight(i), 1.0f + vCenter.x * 0.1f, 0.001f);
          PL_TEST_FLOAT(plGrassCoverageTexel::DecodeNormal(tile.m_Texels[i].m_uiNormal).z, plVec3(0.1f, 0.0f, 1.0f).GetNormalized().z, 0.02f);
        }
      }
    }

    const double fPaintedBefore = data.GetPaintedArea();

    const plString meadow = s_szMeadow;
    PL_TEST_BOOL(data.ThinInDisc(plArrayPtr<const plString>(&meadow, 1), MakeStamp(0, 0, 4, 0.2f, 1.0f)) > 0);
    PL_TEST_BOOL(data.GetPaintedArea() < fPaintedBefore);
    PL_TEST_BOOL(SameContent(data, RoundTrip(data)));

    PL_TEST_BOOL(data.EraseInDisc(plArrayPtr<const plString>(&meadow, 1), MakeStamp(0, 0, 10, 1.0f, 1.0f, 0.0f)) > 0);
    PL_TEST_BOOL(data.FindEntry(s_szMeadow) == nullptr);
    PL_TEST_BOOL(data.FindEntry(s_szDry) != nullptr);
    PL_TEST_BOOL(SameContent(data, RoundTrip(data)));
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Empty tiles are dropped")
  {
    plGrassCoverageData data;
    data.PaintInDisc(s_szMeadow, MakeStamp(0, 0, 3, 1.0f));
    data.PaintInDisc(s_szMeadow, MakeStamp(40, 40, 3, 1.0f));
    const plUInt32 uiTiles = data.GetTileCount();
    PL_TEST_BOOL(uiTiles >= 2);

    // erase everything around the second disc: its tiles go away, the first disc stays
    data.EraseInDisc({}, MakeStamp(40, 40, 8, 1.0f, 1.0f, 0.0f));
    PL_TEST_BOOL(data.GetTileCount() < uiTiles);
    for (const auto& entry : data.m_Entries)
    {
      for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
      {
        PL_TEST_BOOL(!it.Value().IsEmpty());
      }
    }

    data.EraseInDisc({}, MakeStamp(0, 0, 8, 1.0f, 1.0f, 0.0f));
    PL_TEST_BOOL(data.IsEmpty());
    PL_TEST_INT(data.m_Entries.GetCount(), 0);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Merging a delta equals painting in place")
  {
    plGrassCoverageData direct;
    direct.PaintInDisc(s_szMeadow, MakeStamp(1, 1, 3, 0.5f));

    plGrassCoverageData merged = direct;

    direct.PaintInDisc(s_szMeadow, MakeStamp(2, 1, 3, 0.9f));

    plGrassCoverageData delta;
    delta.PaintInDisc(s_szMeadow, MakeStamp(2, 1, 3, 0.9f));
    PL_TEST_BOOL(merged.MergeDelta(delta).Succeeded());
    PL_TEST_BOOL(SameContent(direct, merged));
    PL_TEST_BOOL(merged.GetLastChangedTiles().GetCount() > 0);

    plGrassCoverageData otherGrid;
    otherGrid.m_fTexelSize = 1.0f;
    PL_TEST_BOOL(merged.MergeDelta(otherGrid).Failed());
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Resampling 0.5 -> 0.25 -> 0.5 keeps the painted area within 2%")
  {
    plGrassCoverageData data;
    data.m_fTexelSize = 0.5f;
    data.PaintInDisc(s_szMeadow, MakeStamp(3.3f, -2.1f, 6.0f, 0.8f, 1.0f, 0.4f));
    data.PaintInDisc(s_szDry, MakeStamp(-9.0f, 4.0f, 3.0f, 1.0f, 1.0f, 0.0f));

    const double fArea = data.GetPaintedArea();
    PL_TEST_BOOL(fArea > 0.0);

    const plUInt32 uiCounter = data.GetChangeCounter();
    data.Resample(0.25f);
    PL_TEST_FLOAT(data.m_fTexelSize, 0.25f, 0.0f);
    PL_TEST_BOOL(data.GetChangeCounter() != uiCounter);
    PL_TEST_BOOL(data.GetLastChangeWasFull());

    data.Resample(0.5f);
    PL_TEST_FLOAT(data.m_fTexelSize, 0.5f, 0.0f);

    const double fRatio = data.GetPaintedArea() / fArea;
    PL_TEST_BOOL(fRatio > 0.98 && fRatio < 1.02);
  }

  PL_TEST_BLOCK(plTestBlock::Enabled, "Strand counts are deterministic")
  {
    auto Build = []()
    {
      plGrassCoverageData data;
      data.PaintInDisc(s_szMeadow, MakeStamp(0, 0, 5, 0.7f));
      data.PaintInDisc(s_szDry, MakeStamp(3, 3, 2, 0.4f));
      return data;
    };

    const plGrassCoverageData run1 = Build();
    const plGrassCoverageData run2 = Build();

    const plUInt64 uiCount1 = CountAllStrands(run1);
    PL_TEST_BOOL(uiCount1 > 0);
    PL_TEST_BOOL(uiCount1 == CountAllStrands(run2));
    PL_TEST_BOOL(uiCount1 == CountAllStrands(RoundTrip(run1)));

    // the stochastic rounding keeps the expected count
    const double fExpected = run1.GetPaintedArea(*run1.FindEntry(s_szMeadow)) * 120.0 + run1.GetPaintedArea(*run1.FindEntry(s_szDry)) * 120.0;
    PL_TEST_BOOL(plMath::Abs(static_cast<double>(uiCount1) - fExpected) < fExpected * 0.05);
  }
}
