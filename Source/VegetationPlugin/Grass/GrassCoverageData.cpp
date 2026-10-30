#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Algorithm/HashingUtils.h>
#include <Foundation/Algorithm/Sorting.h>
#include <Foundation/Containers/HashSet.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/StringDeduplicationContext.h>
#include <Foundation/Types/ScopeExit.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>

#include <Shaders/Vegetation/Grass/GrassConstants.h>

static_assert(plGrassCoverageTile::TexelsPerSide == GRASS_TILE_TEXELS);
static_assert(plGrassCoverageTile::TexelCount == GRASS_TILE_TEXEL_COUNT);
static_assert(plGrassCoverageTexelFlags::Discontinuous == GRASS_TEXEL_FLAG_DISCONTINUOUS);

namespace
{
  constexpr plInt32 TileSide = static_cast<plInt32>(plGrassCoverageTile::TexelsPerSide);

  plInt32 FloorDivTile(plInt32 iTexel)
  {
    return iTexel >= 0 ? iTexel / TileSide : -((-iTexel + TileSide - 1) / TileSide);
  }

  plInt32 ToTexelCoordWith(float fPosition, float fTexelSize)
  {
    return static_cast<plInt32>(plMath::Floor(fPosition / fTexelSize));
  }

  bool IsTileInRange(plInt32 iTile)
  {
    return iTile >= -32768 && iTile <= 32767;
  }

  plUInt8 QuantizeRound(float fValue)
  {
    return static_cast<plUInt8>(plMath::Clamp(plMath::Floor(fValue * 255.0f + 0.5f), 0.0f, 255.0f));
  }

  plUInt8 QuantizeFloor(float fValue)
  {
    return static_cast<plUInt8>(plMath::Clamp(plMath::Floor(fValue * 255.0f), 0.0f, 255.0f));
  }

  float SignNotZero(float f)
  {
    return f >= 0.0f ? 1.0f : -1.0f;
  }

  bool MatchesType(plArrayPtr<const plString> types, const plString& sType)
  {
    if (types.IsEmpty())
      return true;

    for (const plString& s : types)
    {
      if (s == sType)
        return true;
    }

    return false;
  }

  /// Sorted tile keys, so blobs are byte-identical for identical content.
  void GetSortedKeys(const plGrassCoverageEntry& entry, plDynamicArray<plUInt32>& out_keys)
  {
    out_keys.Clear();
    out_keys.Reserve(entry.m_Tiles.GetCount());
    for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      out_keys.PushBack(it.Key());
    }
    out_keys.Sort();
  }
} // namespace

//////////////////////////////////////////////////////////////////////////

void plGrassCoverageTexel::EncodeNormal(const plVec3& vNormal, plUInt8* out_pEncoded)
{
  plVec3 n = vNormal;
  const float fSum = plMath::Abs(n.x) + plMath::Abs(n.y) + plMath::Abs(n.z);
  if (!(fSum > 0.0f))
  {
    out_pEncoded[0] = 128;
    out_pEncoded[1] = 128;
    return;
  }

  float px = n.x / fSum;
  float py = n.y / fSum;
  if (n.z < 0.0f)
  {
    const float fx = (1.0f - plMath::Abs(py)) * SignNotZero(px);
    const float fy = (1.0f - plMath::Abs(px)) * SignNotZero(py);
    px = fx;
    py = fy;
  }

  out_pEncoded[0] = QuantizeRound(px * 0.5f + 0.5f);
  out_pEncoded[1] = QuantizeRound(py * 0.5f + 0.5f);
}

plVec3 plGrassCoverageTexel::DecodeNormal(const plUInt8* pEncoded)
{
  const float px = pEncoded[0] / 255.0f * 2.0f - 1.0f;
  const float py = pEncoded[1] / 255.0f * 2.0f - 1.0f;

  plVec3 n(px, py, 1.0f - plMath::Abs(px) - plMath::Abs(py));
  if (n.z < 0.0f)
  {
    const float fx = (1.0f - plMath::Abs(py)) * SignNotZero(px);
    const float fy = (1.0f - plMath::Abs(px)) * SignNotZero(py);
    n.x = fx;
    n.y = fy;
  }

  n.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  return n;
}

bool plGrassCoverageTile::IsEmpty() const
{
  for (const plGrassCoverageTexel& texel : m_Texels)
  {
    if (texel.m_uiDensity != 0)
      return false;
  }
  return true;
}

void plGrassCoverageTile::SetTexelHeight(plUInt32 uiTexel, float fHeight)
{
  const float fMillimetres = plMath::Floor((fHeight - m_fBaseHeight) * 1000.0f + 0.5f);
  m_Texels[uiTexel].m_iHeight = static_cast<plInt16>(plMath::Clamp(fMillimetres, -32768.0f, 32767.0f));
}

float plGrassCoverageStamp::GetWeight(const plVec2& vPosition) const
{
  const plVec2 vRadius = GetLayerRadius();
  const float d = (vPosition - m_vCenter).CompDiv(vRadius).GetLength();
  if (d > 1.0f)
    return 0.0f;

  const float fFalloff = plMath::Clamp(m_fFalloff, 0.0f, 1.0f);
  if (d < 1.0f - fFalloff)
    return 1.0f;

  return plMath::Clamp((1.0f - d) / plMath::Max(fFalloff, 0.001f), 0.0f, 1.0f);
}

//////////////////////////////////////////////////////////////////////////

float plGrassCoverageData::ClampTexelSize(float fTexelSize)
{
  if (!plMath::IsFinite(fTexelSize))
    return DefaultTexelSize;

  return plMath::Clamp(fTexelSize, MinTexelSize, MaxTexelSize);
}

plUInt32 plGrassCoverageData::GetTypeSeed(plStringView sType)
{
  return plHashingUtils::xxHash32String(sType);
}

void plGrassCoverageData::Serialize(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_fTexelSize;

  inout_stream << m_Entries.GetCount();

  plDynamicArray<plUInt32> keys;
  for (const auto& entry : m_Entries)
  {
    inout_stream << entry.m_sType;

    GetSortedKeys(entry, keys);
    inout_stream << keys.GetCount();

    for (plUInt32 uiKey : keys)
    {
      const plGrassCoverageTile& tile = *entry.m_Tiles.GetValue(uiKey);
      inout_stream << tile.m_iX;
      inout_stream << tile.m_iY;
      inout_stream << tile.m_fBaseHeight;

      // texels are written as raw little-endian bytes, 2 KB per tile
      inout_stream.WriteBytes(tile.m_Texels, sizeof(tile.m_Texels)).IgnoreResult();
    }
  }
}

plResult plGrassCoverageData::Deserialize(plStreamReader& inout_stream, plUInt64 uiStreamBytes)
{
  // Grass types per layer and tiles per blob (1M tiles is 2 GB of texels); a count above either marks a corrupt blob.
  constexpr plUInt32 uiMaxEntries = 4096;
  constexpr plUInt32 uiMaxTiles = 1u << 20;
  constexpr plUInt64 uiTileBytes = sizeof(plGrassCoverageTile::m_iX) + sizeof(plGrassCoverageTile::m_iY) + sizeof(plGrassCoverageTile::m_fBaseHeight) + sizeof(plGrassCoverageTile::m_Texels);

  BeginChange(true);
  m_Entries.Clear();
  m_fTexelSize = DefaultTexelSize;

  auto Fail = [&]() -> plResult
  {
    m_Entries.Clear();
    EndChange();
    return PL_FAILURE;
  };

  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return Fail();

  inout_stream >> m_fTexelSize;
  m_fTexelSize = ClampTexelSize(m_fTexelSize);

  plUInt32 uiEntries = 0;
  if (inout_stream.ReadDWordValue(&uiEntries).Failed() || uiEntries > uiMaxEntries)
    return Fail();

  // Upper bound on the bytes still unread; type strings are not subtracted.
  plUInt64 uiBytesLeft = uiStreamBytes;
  auto Consume = [&](plUInt64 uiBytes)
  {
    uiBytesLeft = uiBytesLeft > uiBytes ? uiBytesLeft - uiBytes : 0;
  };
  Consume(sizeof(plUInt8) + sizeof(float) + sizeof(plUInt32));

  plUInt32 uiTotalTiles = 0;
  for (plUInt32 e = 0; e < uiEntries; ++e)
  {
    auto& entry = m_Entries.ExpandAndGetRef();
    inout_stream >> entry.m_sType;

    plUInt32 uiTiles = 0;
    if (inout_stream.ReadDWordValue(&uiTiles).Failed())
      return Fail();
    Consume(sizeof(plUInt32));

    uiTotalTiles += plMath::Min(uiTiles, uiMaxTiles + 1);
    if (uiTiles > uiMaxTiles || uiTotalTiles > uiMaxTiles || uiTiles > uiBytesLeft / uiTileBytes)
      return Fail();

    Consume(uiTiles * uiTileBytes);
    entry.m_Tiles.Reserve(uiTiles);

    for (plUInt32 t = 0; t < uiTiles; ++t)
    {
      plGrassCoverageTile tile;
      inout_stream >> tile.m_iX;
      inout_stream >> tile.m_iY;
      inout_stream >> tile.m_fBaseHeight;

      if (inout_stream.ReadBytes(tile.m_Texels, sizeof(tile.m_Texels)) != sizeof(tile.m_Texels))
        return Fail();

      entry.m_Tiles.Insert(plGrassCoverageTile::MakeKey(tile.m_iX, tile.m_iY), tile);
    }
  }

  EndChange();
  return PL_SUCCESS;
}

void plGrassCoverageData::ToBuffer(plDataBuffer& out_buffer) const
{
  // Coverage is a self-contained blob; its type names must not reference an enclosing scene's string table.
  auto* pContext = plStringDeduplicationWriteContext::GetContext();
  if (pContext != nullptr)
    pContext->SetActive(false);
  PL_SCOPE_EXIT(if (pContext != nullptr) pContext->SetActive(true));

  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  Serialize(writer);

  out_buffer.SetCountUninitialized(storage.GetStorageSize32());
  if (storage.GetStorageSize32() > 0)
  {
    plMemoryUtils::Copy(out_buffer.GetData(), storage.GetData(), storage.GetStorageSize32());
  }
}

plResult plGrassCoverageData::FromBuffer(const plDataBuffer& buffer)
{
  if (buffer.IsEmpty())
  {
    BeginChange(true);
    m_Entries.Clear();
    m_fTexelSize = DefaultTexelSize;
    EndChange();
    return PL_SUCCESS;
  }

  auto* pContext = plStringDeduplicationReadContext::GetContext();
  if (pContext != nullptr)
    pContext->SetActive(false);
  PL_SCOPE_EXIT(if (pContext != nullptr) pContext->SetActive(true));

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
  return Deserialize(reader, buffer.GetCount());
}

void plGrassCoverageData::Clear()
{
  BeginChange(true);
  m_Entries.Clear();
  EndChange();
}

plGrassCoverageEntry& plGrassCoverageData::GetOrAddEntry(plStringView sType)
{
  for (auto& entry : m_Entries)
  {
    if (entry.m_sType == sType)
      return entry;
  }

  auto& entry = m_Entries.ExpandAndGetRef();
  entry.m_sType = sType;
  return entry;
}

const plGrassCoverageEntry* plGrassCoverageData::FindEntry(plStringView sType) const
{
  for (const auto& entry : m_Entries)
  {
    if (entry.m_sType == sType)
      return &entry;
  }

  return nullptr;
}

bool plGrassCoverageData::IsEmpty() const
{
  return GetTileCount() == 0;
}

plUInt32 plGrassCoverageData::GetTileCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& entry : m_Entries)
  {
    uiCount += entry.m_Tiles.GetCount();
  }
  return uiCount;
}

plInt32 plGrassCoverageData::ToTexelCoord(float fPosition) const
{
  return static_cast<plInt32>(plMath::Floor(fPosition / m_fTexelSize));
}

plVec2 plGrassCoverageData::GetTexelCenter(plInt32 iTexelX, plInt32 iTexelY) const
{
  return plVec2((iTexelX + 0.5f) * m_fTexelSize, (iTexelY + 0.5f) * m_fTexelSize);
}

double plGrassCoverageData::GetPaintedArea() const
{
  double fArea = 0.0;
  for (const auto& entry : m_Entries)
  {
    fArea += GetPaintedArea(entry);
  }
  return fArea;
}

double plGrassCoverageData::GetPaintedArea(const plGrassCoverageEntry& entry) const
{
  plUInt64 uiDensitySum = 0;
  for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
  {
    for (const plGrassCoverageTexel& texel : it.Value().m_Texels)
    {
      uiDensitySum += texel.m_uiDensity;
    }
  }

  return static_cast<double>(uiDensitySum) / 255.0 * static_cast<double>(m_fTexelSize) * static_cast<double>(m_fTexelSize);
}

plUInt64 plGrassCoverageData::CountStrands(const plGrassCoverageEntry& entry, float fStrandsPerSquareMeter) const
{
  const plUInt32 uiPerTexelQ8 = GrassStrandsPerTexelQ8(fStrandsPerSquareMeter, m_fTexelSize);
  const plUInt32 uiSeed = GetTypeSeed(entry.m_sType);

  plUInt64 uiCount = 0;
  for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
  {
    const plGrassCoverageTile& tile = it.Value();
    for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
    {
      const plUInt32 uiDensity = tile.m_Texels[i].m_uiDensity;
      if (uiDensity == 0)
        continue;

      uiCount += GrassStrandCountForTexelQ8(uiDensity, uiPerTexelQ8, GrassHash(tile.m_iX, tile.m_iY, i, uiSeed));
    }
  }

  return uiCount;
}

plGrassCoverageTile& plGrassCoverageData::GetOrAddTile(plGrassCoverageEntry& entry, plInt32 iTileX, plInt32 iTileY, float fBaseHeight)
{
  const plUInt32 uiKey = plGrassCoverageTile::MakeKey(iTileX, iTileY);

  bool bExisted = false;
  plGrassCoverageTile& tile = entry.m_Tiles.FindOrAdd(uiKey, &bExisted);
  if (!bExisted)
  {
    tile.m_iX = static_cast<plInt16>(iTileX);
    tile.m_iY = static_cast<plInt16>(iTileY);
    tile.m_fBaseHeight = fBaseHeight;
  }

  return tile;
}

void plGrassCoverageData::BeginChange(bool bFull)
{
  ++m_uiChangeCounter;
  m_bLastChangeWasFull = bFull;
  m_LastChangedTiles.Clear();
}

void plGrassCoverageData::MarkTileChanged(const plGrassCoverageEntry& entry, plUInt32 uiTileKey)
{
  if (m_bLastChangeWasFull)
    return;

  plGrassCoverageTileId& id = m_LastChangedTiles.ExpandAndGetRef();
  id.m_uiTypeSeed = GetTypeSeed(entry.m_sType);
  id.m_uiTileKey = uiTileKey;
}

void plGrassCoverageData::EndChange()
{
  if (m_LastChangedTiles.GetCount() < 2)
    return;

  m_LastChangedTiles.Sort([](const plGrassCoverageTileId& a, const plGrassCoverageTileId& b)
    { return a.m_uiTypeSeed != b.m_uiTypeSeed ? a.m_uiTypeSeed < b.m_uiTypeSeed : a.m_uiTileKey < b.m_uiTileKey; });

  plUInt32 uiWrite = 1;
  for (plUInt32 i = 1; i < m_LastChangedTiles.GetCount(); ++i)
  {
    if (!(m_LastChangedTiles[i] == m_LastChangedTiles[uiWrite - 1]))
    {
      m_LastChangedTiles[uiWrite++] = m_LastChangedTiles[i];
    }
  }
  m_LastChangedTiles.SetCount(uiWrite);
}

plUInt32 plGrassCoverageData::PaintInDisc(plStringView sType, const plGrassCoverageStamp& stamp, SampleFunc sampler)
{
  if (sType.IsEmpty() || !(stamp.m_fDensity > 0.0f))
    return 0;

  const plVec2 vRadius = stamp.GetLayerRadius();
  const plInt32 iX0 = ToTexelCoord(stamp.m_vCenter.x - vRadius.x);
  const plInt32 iX1 = ToTexelCoord(stamp.m_vCenter.x + vRadius.x);
  const plInt32 iY0 = ToTexelCoord(stamp.m_vCenter.y - vRadius.y);
  const plInt32 iY1 = ToTexelCoord(stamp.m_vCenter.y + vRadius.y);

  const plUInt8 uiLength = QuantizeRound(plMath::Clamp(stamp.m_fLength, 0.0f, 1.0f));
  const float fDensity = plMath::Clamp(stamp.m_fDensity, 0.0f, 1.0f);

  BeginChange(false);

  plGrassCoverageEntry& entry = GetOrAddEntry(sType);
  plUInt32 uiWritten = 0;

  for (plInt32 iY = iY0; iY <= iY1; ++iY)
  {
    const plInt32 iTileY = FloorDivTile(iY);
    if (!IsTileInRange(iTileY))
      continue;

    for (plInt32 iX = iX0; iX <= iX1; ++iX)
    {
      const plInt32 iTileX = FloorDivTile(iX);
      if (!IsTileInRange(iTileX))
        continue;

      const plVec2 vCenter = GetTexelCenter(iX, iY);
      const float fWeight = stamp.GetWeight(vCenter);
      if (fWeight <= 0.0f)
        continue;

      const plUInt8 uiDensity = QuantizeRound(fDensity * fWeight);
      if (uiDensity == 0)
        continue;

      plGrassCoverageSample sample;
      if (sampler.IsValid() && !sampler(vCenter, sample))
        continue;

      plGrassCoverageTile& tile = GetOrAddTile(entry, iTileX, iTileY, sample.m_fHeight);
      const plUInt32 uiTexel = static_cast<plUInt32>((iY - iTileY * TileSide) * TileSide + (iX - iTileX * TileSide));

      plGrassCoverageTexel& texel = tile.m_Texels[uiTexel];
      texel.m_uiDensity = plMath::Max(texel.m_uiDensity, uiDensity);
      texel.m_uiLength = uiLength;
      tile.SetTexelHeight(uiTexel, sample.m_fHeight);
      plGrassCoverageTexel::EncodeNormal(sample.m_vNormal, texel.m_uiNormal);
      texel.m_uiFlags = sample.m_bDiscontinuous ? plGrassCoverageTexelFlags::Discontinuous : 0;

      MarkTileChanged(entry, plGrassCoverageTile::MakeKey(iTileX, iTileY));
      ++uiWritten;
    }
  }

  if (entry.m_Tiles.IsEmpty())
  {
    Compact();
  }

  EndChange();
  return uiWritten;
}

template <typename Func>
plUInt32 plGrassCoverageData::ModifyDensityInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp, Func func)
{
  const plVec2 vRadius = stamp.GetLayerRadius();
  const plInt32 iTileX0 = FloorDivTile(ToTexelCoord(stamp.m_vCenter.x - vRadius.x));
  const plInt32 iTileX1 = FloorDivTile(ToTexelCoord(stamp.m_vCenter.x + vRadius.x));
  const plInt32 iTileY0 = FloorDivTile(ToTexelCoord(stamp.m_vCenter.y - vRadius.y));
  const plInt32 iTileY1 = FloorDivTile(ToTexelCoord(stamp.m_vCenter.y + vRadius.y));

  BeginChange(false);

  plUInt32 uiChanged = 0;

  for (auto& entry : m_Entries)
  {
    if (!MatchesType(types, entry.m_sType))
      continue;

    for (plInt32 iTileY = iTileY0; iTileY <= iTileY1; ++iTileY)
    {
      for (plInt32 iTileX = iTileX0; iTileX <= iTileX1; ++iTileX)
      {
        if (!IsTileInRange(iTileX) || !IsTileInRange(iTileY))
          continue;

        const plUInt32 uiKey = plGrassCoverageTile::MakeKey(iTileX, iTileY);
        plGrassCoverageTile* pTile = nullptr;
        if (!entry.m_Tiles.TryGetValue(uiKey, pTile))
          continue;

        bool bTileChanged = false;
        for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
        {
          plGrassCoverageTexel& texel = pTile->m_Texels[i];
          if (texel.m_uiDensity == 0)
            continue;

          const plInt32 iX = iTileX * TileSide + static_cast<plInt32>(i % plGrassCoverageTile::TexelsPerSide);
          const plInt32 iY = iTileY * TileSide + static_cast<plInt32>(i / plGrassCoverageTile::TexelsPerSide);
          const float fWeight = stamp.GetWeight(GetTexelCenter(iX, iY));
          if (fWeight <= 0.0f)
            continue;

          const plUInt8 uiNew = func(texel.m_uiDensity, fWeight);
          if (uiNew != texel.m_uiDensity)
          {
            texel.m_uiDensity = uiNew;
            bTileChanged = true;
            ++uiChanged;
          }
        }

        if (bTileChanged)
        {
          MarkTileChanged(entry, uiKey);
        }
      }
    }
  }

  if (uiChanged > 0)
  {
    Compact();
  }

  EndChange();
  return uiChanged;
}

plUInt32 plGrassCoverageData::EraseInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp)
{
  const float fStrength = plMath::Clamp(stamp.m_fStrength, 0.0f, 1.0f);

  return ModifyDensityInDisc(types, stamp, [fStrength](plUInt8 uiDensity, float fWeight) -> plUInt8
    {
      const float f = uiDensity / 255.0f;
      const float fNew = plMath::Max(0.0f, f - fStrength * fWeight);
      return fNew < f ? QuantizeFloor(fNew) : uiDensity;
    });
}

plUInt32 plGrassCoverageData::ThinInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp)
{
  const float fStrength = plMath::Clamp(stamp.m_fStrength, 0.0f, 1.0f);
  const float fDensity = plMath::Clamp(stamp.m_fDensity, 0.0f, 1.0f);

  return ModifyDensityInDisc(types, stamp, [fStrength, fDensity](plUInt8 uiDensity, float fWeight) -> plUInt8
    {
      const float f = uiDensity / 255.0f;
      const float fNew = f + (plMath::Min(f, fDensity) - f) * fStrength * fWeight;
      return QuantizeRound(fNew);
    });
}

plResult plGrassCoverageData::MergeDelta(const plGrassCoverageData& delta)
{
  if (plMath::Abs(delta.m_fTexelSize - m_fTexelSize) > 0.0001f)
    return PL_FAILURE;

  BeginChange(false);

  for (const plGrassCoverageEntry& deltaEntry : delta.m_Entries)
  {
    if (deltaEntry.m_Tiles.IsEmpty())
      continue;

    plGrassCoverageEntry& entry = GetOrAddEntry(deltaEntry.m_sType);

    for (auto it = deltaEntry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const plGrassCoverageTile& src = it.Value();
      plGrassCoverageTile* pDst = nullptr;

      for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      {
        const plGrassCoverageTexel& s = src.m_Texels[i];
        if (s.m_uiDensity == 0)
          continue;

        const float fHeight = src.GetTexelHeight(i);
        if (pDst == nullptr)
        {
          pDst = &GetOrAddTile(entry, src.m_iX, src.m_iY, fHeight);
        }

        plGrassCoverageTexel& d = pDst->m_Texels[i];
        d.m_uiDensity = plMath::Max(d.m_uiDensity, s.m_uiDensity);
        d.m_uiLength = s.m_uiLength;
        pDst->SetTexelHeight(i, fHeight);
        d.m_uiNormal[0] = s.m_uiNormal[0];
        d.m_uiNormal[1] = s.m_uiNormal[1];
        d.m_uiFlags = s.m_uiFlags;
      }

      if (pDst != nullptr)
      {
        MarkTileChanged(entry, it.Key());
      }
    }
  }

  EndChange();
  return PL_SUCCESS;
}

void plGrassCoverageData::Resample(float fNewTexelSize)
{
  fNewTexelSize = ClampTexelSize(fNewTexelSize);
  if (plMath::Abs(fNewTexelSize - m_fTexelSize) < 0.00001f)
    return;

  const float fOld = m_fTexelSize;
  plHybridArray<plGrassCoverageEntry, 2> source = std::move(m_Entries);
  m_Entries.Clear();

  BeginChange(true);
  m_fTexelSize = fNewTexelSize;

  for (const plGrassCoverageEntry& src : source)
  {
    if (src.m_Tiles.IsEmpty())
      continue;

    // Destination tiles overlapping every source tile, grown by one source texel for the bilinear footprint.
    plHashSet<plUInt32> destKeys;
    for (auto it = src.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const plGrassCoverageTile& tile = it.Value();
      const float fMinX = (tile.m_iX * TileSide - 1) * fOld;
      const float fMinY = (tile.m_iY * TileSide - 1) * fOld;
      const float fMaxX = ((tile.m_iX + 1) * TileSide + 1) * fOld;
      const float fMaxY = ((tile.m_iY + 1) * TileSide + 1) * fOld;

      const plInt32 iTX0 = FloorDivTile(ToTexelCoord(fMinX));
      const plInt32 iTX1 = FloorDivTile(ToTexelCoord(fMaxX));
      const plInt32 iTY0 = FloorDivTile(ToTexelCoord(fMinY));
      const plInt32 iTY1 = FloorDivTile(ToTexelCoord(fMaxY));

      for (plInt32 ty = iTY0; ty <= iTY1; ++ty)
      {
        for (plInt32 tx = iTX0; tx <= iTX1; ++tx)
        {
          if (IsTileInRange(tx) && IsTileInRange(ty))
          {
            destKeys.Insert(plGrassCoverageTile::MakeKey(tx, ty));
          }
        }
      }
    }

    auto GetSourceTexel = [&](plInt32 iX, plInt32 iY, const plGrassCoverageTile*& out_pTile) -> const plGrassCoverageTexel*
    {
      const plInt32 iTileX = FloorDivTile(iX);
      const plInt32 iTileY = FloorDivTile(iY);
      if (!IsTileInRange(iTileX) || !IsTileInRange(iTileY))
        return nullptr;

      const plGrassCoverageTile* pTile = src.m_Tiles.GetValue(plGrassCoverageTile::MakeKey(iTileX, iTileY));
      if (pTile == nullptr)
        return nullptr;

      out_pTile = pTile;
      return &pTile->m_Texels[(iY - iTileY * TileSide) * TileSide + (iX - iTileX * TileSide)];
    };

    plGrassCoverageEntry& dst = m_Entries.ExpandAndGetRef();
    dst.m_sType = src.m_sType;

    for (auto keyIt = destKeys.GetIterator(); keyIt.IsValid(); ++keyIt)
    {
      const plInt32 iTileX = plGrassCoverageTile::KeyToX(keyIt.Key());
      const plInt32 iTileY = plGrassCoverageTile::KeyToY(keyIt.Key());
      plGrassCoverageTile* pDstTile = nullptr;

      for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
      {
        const plInt32 iX = iTileX * TileSide + static_cast<plInt32>(i % plGrassCoverageTile::TexelsPerSide);
        const plInt32 iY = iTileY * TileSide + static_cast<plInt32>(i / plGrassCoverageTile::TexelsPerSide);
        const plVec2 vCenter = GetTexelCenter(iX, iY);

        // bilinear footprint in source texel space (texel centres at integer + 0.5)
        const float u = vCenter.x / fOld - 0.5f;
        const float v = vCenter.y / fOld - 0.5f;
        const plInt32 iU0 = static_cast<plInt32>(plMath::Floor(u));
        const plInt32 iV0 = static_cast<plInt32>(plMath::Floor(v));
        const float fu = u - iU0;
        const float fv = v - iV0;

        const float fWeights[4] = {(1.0f - fu) * (1.0f - fv), fu * (1.0f - fv), (1.0f - fu) * fv, fu * fv};
        const plInt32 iOffX[4] = {0, 1, 0, 1};
        const plInt32 iOffY[4] = {0, 0, 1, 1};

        float fDensity = 0.0f;
        float fLengthSum = 0.0f;
        float fBestContribution = 0.0f;
        const plGrassCoverageTexel* pBest = nullptr;
        const plGrassCoverageTile* pBestTile = nullptr;
        plUInt32 uiBestIndex = 0;

        for (plUInt32 k = 0; k < 4; ++k)
        {
          const plGrassCoverageTile* pTile = nullptr;
          const plGrassCoverageTexel* pTexel = GetSourceTexel(iU0 + iOffX[k], iV0 + iOffY[k], pTile);
          if (pTexel == nullptr || pTexel->m_uiDensity == 0)
            continue;

          const float fContribution = fWeights[k] * (pTexel->m_uiDensity / 255.0f);
          fDensity += fContribution;
          fLengthSum += fContribution * (pTexel->m_uiLength / 255.0f);

          if (fContribution > fBestContribution)
          {
            fBestContribution = fContribution;
            pBest = pTexel;
            pBestTile = pTile;
            uiBestIndex = static_cast<plUInt32>(pTexel - pTile->m_Texels);
          }
        }

        const plUInt8 uiDensity = QuantizeRound(fDensity);
        if (uiDensity == 0 || pBest == nullptr)
          continue;

        // nearest: the source texel containing the centre when painted, else the strongest bilinear contributor
        {
          const plGrassCoverageTile* pTile = nullptr;
          const plGrassCoverageTexel* pNearest = GetSourceTexel(ToTexelCoordWith(vCenter.x, fOld), ToTexelCoordWith(vCenter.y, fOld), pTile);
          if (pNearest != nullptr && pNearest->m_uiDensity != 0)
          {
            pBest = pNearest;
            pBestTile = pTile;
            uiBestIndex = static_cast<plUInt32>(pNearest - pTile->m_Texels);
          }
        }

        const float fHeight = pBestTile->GetTexelHeight(uiBestIndex);
        if (pDstTile == nullptr)
        {
          pDstTile = &GetOrAddTile(dst, iTileX, iTileY, fHeight);
        }

        plGrassCoverageTexel& d = pDstTile->m_Texels[i];
        d.m_uiDensity = uiDensity;
        d.m_uiLength = QuantizeRound(fLengthSum / fDensity);
        pDstTile->SetTexelHeight(i, fHeight);
        d.m_uiNormal[0] = pBest->m_uiNormal[0];
        d.m_uiNormal[1] = pBest->m_uiNormal[1];
        d.m_uiFlags = pBest->m_uiFlags;
      }
    }
  }

  Compact();
  EndChange();
}

void plGrassCoverageData::Compact()
{
  plHybridArray<plUInt32, 16> emptyKeys;

  for (plUInt32 e = m_Entries.GetCount(); e > 0; --e)
  {
    plGrassCoverageEntry& entry = m_Entries[e - 1];

    emptyKeys.Clear();
    for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().IsEmpty())
      {
        emptyKeys.PushBack(it.Key());
      }
    }

    for (plUInt32 uiKey : emptyKeys)
    {
      entry.m_Tiles.Remove(uiKey);
    }

    if (entry.m_Tiles.IsEmpty())
    {
      m_Entries.RemoveAtAndCopy(e - 1);
    }
  }
}

void plGrassCoverageData::AssignAndTrackChanges(const plGrassCoverageData& other)
{
  if (plMath::Abs(other.m_fTexelSize - m_fTexelSize) > 0.00001f)
  {
    BeginChange(true);
    m_fTexelSize = other.m_fTexelSize;
    m_Entries = other.m_Entries;
    EndChange();
    return;
  }

  BeginChange(false);

  auto CompareTiles = [&](const plGrassCoverageEntry& a, const plGrassCoverageEntry* pB)
  {
    for (auto it = a.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      const plGrassCoverageTile* pOther = pB != nullptr ? pB->m_Tiles.GetValue(it.Key()) : nullptr;
      if (pOther == nullptr || pOther->m_fBaseHeight != it.Value().m_fBaseHeight || plMemoryUtils::Compare(pOther->m_Texels, it.Value().m_Texels, plGrassCoverageTile::TexelCount) != 0)
      {
        MarkTileChanged(a, it.Key());
      }
    }
  };

  // tiles changed or removed, then tiles added
  for (const plGrassCoverageEntry& oldEntry : m_Entries)
  {
    CompareTiles(oldEntry, other.FindEntry(oldEntry.m_sType));
  }
  for (const plGrassCoverageEntry& newEntry : other.m_Entries)
  {
    const plGrassCoverageEntry* pOld = FindEntry(newEntry.m_sType);
    for (auto it = newEntry.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (pOld == nullptr || !pOld->m_Tiles.Contains(it.Key()))
      {
        MarkTileChanged(newEntry, it.Key());
      }
    }
  }

  m_Entries = other.m_Entries;
  EndChange();
}
