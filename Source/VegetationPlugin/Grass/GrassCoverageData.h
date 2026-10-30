#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Math/Vec2.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Delegate.h>
#include <Foundation/Types/VariantType.h>
#include <VegetationPlugin/VegetationPluginDLL.h>

struct plGrassCoverageTexelFlags
{
  enum Enum : plUInt8
  {
    Discontinuous = PL_BIT(0), ///< the four corner samples disagreed; the texel uses its centre sample
  };
};

/// \brief One coverage texel, 8 bytes.
struct PL_VEGETATIONPLUGIN_DLL plGrassCoverageTexel
{
  PL_DECLARE_POD_TYPE();

  plUInt8 m_uiDensity = 0;        ///< 0 = none, 255 = the type's full strands per m^2
  plUInt8 m_uiLength = 255;       ///< length multiplier 0..1
  plInt16 m_iHeight = 0;          ///< surface height in mm above the tile's base height
  plUInt8 m_uiNormal[2] = {128, 128}; ///< octahedral surface normal, layer space
  plUInt8 m_uiFlags = 0;          ///< plGrassCoverageTexelFlags
  plUInt8 m_uiReserved = 0;

  static void EncodeNormal(const plVec3& vNormal, plUInt8* out_pEncoded);
  static plVec3 DecodeNormal(const plUInt8* pEncoded);
};

static_assert(sizeof(plGrassCoverageTexel) == 8);

/// \brief 16 x 16 texels. At 0.5 m texels a tile covers 8 m.
struct PL_VEGETATIONPLUGIN_DLL plGrassCoverageTile
{
  static constexpr plUInt32 TexelsPerSide = 16;
  static constexpr plUInt32 TexelCount = TexelsPerSide * TexelsPerSide;

  plInt16 m_iX = 0;
  plInt16 m_iY = 0;
  float m_fBaseHeight = 0.0f; ///< layer-space height that texel heights are relative to
  plGrassCoverageTexel m_Texels[TexelCount];

  bool IsEmpty() const;

  float GetTexelHeight(plUInt32 uiTexel) const { return m_fBaseHeight + m_Texels[uiTexel].m_iHeight * 0.001f; }

  /// \brief Stores a layer-space height relative to the base height, clamped to the int16 millimetre range.
  void SetTexelHeight(plUInt32 uiTexel, float fHeight);

  static plUInt32 MakeKey(plInt32 iX, plInt32 iY) { return (static_cast<plUInt32>(static_cast<plUInt16>(iX)) << 16) | static_cast<plUInt16>(iY); }
  static plInt16 KeyToX(plUInt32 uiKey) { return static_cast<plInt16>(static_cast<plUInt16>(uiKey >> 16)); }
  static plInt16 KeyToY(plUInt32 uiKey) { return static_cast<plInt16>(static_cast<plUInt16>(uiKey & 0xFFFF)); }
};

/// \brief All coverage of one grass type in a layer.
struct PL_VEGETATIONPLUGIN_DLL plGrassCoverageEntry
{
  plString m_sType; ///< Grass Type asset guid
  plHashTable<plUInt32, plGrassCoverageTile> m_Tiles; ///< sparse, keyed by plGrassCoverageTile::MakeKey; empty tiles are dropped by Compact
};

/// \brief Identifies one tile that changed. m_uiTypeSeed is plGrassCoverageData::GetTypeSeed() of the entry's type.
struct plGrassCoverageTileId
{
  PL_DECLARE_POD_TYPE();

  plUInt32 m_uiTypeSeed = 0;
  plUInt32 m_uiTileKey = 0;

  bool operator==(const plGrassCoverageTileId& other) const { return m_uiTypeSeed == other.m_uiTypeSeed && m_uiTileKey == other.m_uiTileKey; }
};

/// \brief What a brush stamp samples at a texel centre, in layer space.
struct plGrassCoverageSample
{
  float m_fHeight = 0.0f;
  plVec3 m_vNormal = plVec3(0, 0, 1);
  bool m_bDiscontinuous = false;
};

/// \brief One brush stamp in layer space. Only x and y of the centre matter; the disc is taken in the layer's xy plane.
struct plGrassCoverageStamp
{
  plVec2 m_vCenter = plVec2::MakeZero(); ///< layer space
  float m_fRadius = 4.0f;                ///< in the units m_vAxisScale maps to (world metres for a brush)
  plVec2 m_vAxisScale = plVec2(1.0f);    ///< length of one layer-space unit along x and y; the disc is an ellipse in layer space
  float m_fFalloff = 0.5f;
  float m_fDensity = 0.6f;
  float m_fStrength = 0.5f;
  float m_fLength = 1.0f;

  /// \brief Half extents of the disc in layer space.
  plVec2 GetLayerRadius() const
  {
    const float fRadius = plMath::Max(m_fRadius, 0.001f);
    return plVec2(fRadius / plMath::Max(plMath::Abs(m_vAxisScale.x), 0.0001f), fRadius / plMath::Max(plMath::Abs(m_vAxisScale.y), 0.0001f));
  }

  /// \brief 1 inside the inner radius, fading linearly to 0 at the edge; 0 outside. Same curve as the instance brush.
  float GetWeight(const plVec2& vPosition) const;
};

/// \brief Painted grass of a plVegetationLayerComponent: per grass type, a sparse grid of 16 x 16 texel tiles in the
/// layer object's local xy plane. Stored as the layer's GrassCoverage blob. Shared by the runtime component, the
/// editor paint tool and the engine-process brush.
///
/// Every operation that changes texels increments the change counter and records the tiles it touched, so the runtime
/// only regrows those.
class PL_VEGETATIONPLUGIN_DLL plGrassCoverageData
{
public:
  static constexpr float MinTexelSize = 0.25f;
  static constexpr float MaxTexelSize = 2.0f;
  static constexpr float DefaultTexelSize = 0.5f;

  /// \brief Returns true for a successful sample; false skips the texel (slope rejection, no surface).
  using SampleFunc = plDelegate<bool(const plVec2& vPosition, plGrassCoverageSample& out_sample)>;

  float m_fTexelSize = DefaultTexelSize; ///< the layer's Grass Texel Size in metres, 0.25 .. 2
  plHybridArray<plGrassCoverageEntry, 2> m_Entries;

  static float ClampTexelSize(float fTexelSize);

  /// \brief Deterministic per-type seed for GrassHash (xxHash32 of the type guid string).
  static plUInt32 GetTypeSeed(plStringView sType);

  void Serialize(plStreamWriter& inout_stream) const;
  /// \brief Fails on counts beyond the caps or beyond what uiStreamBytes (an upper bound on the bytes left in the
  /// stream) can hold, before anything is reserved.
  plResult Deserialize(plStreamReader& inout_stream, plUInt64 uiStreamBytes = plMath::MaxValue<plUInt64>());

  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);

  void Clear();

  plGrassCoverageEntry& GetOrAddEntry(plStringView sType);
  const plGrassCoverageEntry* FindEntry(plStringView sType) const;

  bool IsEmpty() const;
  plUInt32 GetTileCount() const;

  /// \brief Texel-grid coordinates of a layer-space position.
  plInt32 ToTexelCoord(float fPosition) const;

  /// \brief Layer-space centre of a texel given its global texel coordinates.
  plVec2 GetTexelCenter(plInt32 iTexelX, plInt32 iTexelY) const;

  /// \brief Painted area weighted by density: sum of density / 255 * texel area, in m^2.
  double GetPaintedArea() const;
  double GetPaintedArea(const plGrassCoverageEntry& entry) const;

  /// \brief Exact strand count of one entry, using the same GrassHash / GrassStrandCountForTexel as the GPU.
  plUInt64 CountStrands(const plGrassCoverageEntry& entry, float fStrandsPerSquareMeter) const;

  /// \brief Raises density toward Density * weight and writes Length and the sampled surface under the stamp. Without
  /// a sampler the surface is the plane z = 0. Returns the number of texels written.
  plUInt32 PaintInDisc(plStringView sType, const plGrassCoverageStamp& stamp, SampleFunc sampler = {});

  /// \brief Lowers density by Strength * weight. An empty types list matches all types. Returns the texels changed.
  plUInt32 EraseInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp);

  /// \brief Moves density toward min(density, Density) by Strength * weight. An empty types list matches all types.
  plUInt32 ThinInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp);

  /// \brief Applies a delta produced by PaintInDisc on empty data (the engine brush result): density takes the maximum,
  /// and length, height, normal and flags come from the delta wherever it has density. Fails when the texel sizes
  /// differ.
  plResult MergeDelta(const plGrassCoverageData& delta);

  /// \brief Re-grids every entry to a new texel size. Density and length are sampled bilinearly (length weighted by
  /// density), height, normal and flags come from the nearest painted source texel.
  void Resample(float fNewTexelSize);

  /// \brief Drops empty tiles and entries.
  void Compact();

  /// \brief Replaces the content and records every tile whose texels differ as changed.
  void AssignAndTrackChanges(const plGrassCoverageData& other);

  plUInt32 GetChangeCounter() const { return m_uiChangeCounter; }

  /// \brief Tiles touched by the last change, including tiles that became empty or were dropped.
  plArrayPtr<const plGrassCoverageTileId> GetLastChangedTiles() const { return m_LastChangedTiles; }

  /// \brief True when the last change replaced the grid (load, texel size); every tile regrows.
  bool GetLastChangeWasFull() const { return m_bLastChangeWasFull; }

private:
  plGrassCoverageTile& GetOrAddTile(plGrassCoverageEntry& entry, plInt32 iTileX, plInt32 iTileY, float fBaseHeight);
  void BeginChange(bool bFull);
  void MarkTileChanged(const plGrassCoverageEntry& entry, plUInt32 uiTileKey);
  void EndChange();

  template <typename Func>
  plUInt32 ModifyDensityInDisc(plArrayPtr<const plString> types, const plGrassCoverageStamp& stamp, Func func);

  plUInt32 m_uiChangeCounter = 0;
  bool m_bLastChangeWasFull = false;
  plDynamicArray<plGrassCoverageTileId> m_LastChangedTiles;
};
