#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Types/ArrayPtr.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plVegetationCellInstance;
struct plVegetationCellGroup;

/// \brief How foliage composite trees share wind, from the Vegetation.Composite.* cvars.
///
/// Every drawn tree borrows the pose of one of a few shared variants simulated per type and wind tile. A tree that needs
/// its own motion - a touch source reaches it, it is near the main camera, or code pinned it - is promoted to a standalone
/// plFoliageBonePool instance and blends into it, and blends back once nothing has needed it for a while.
struct plVegetationCompositeWindSettings
{
  float m_fTileSize = 256.0f;      ///< Vegetation.Composite.WindTileSize: metres per wind tile side.
  plUInt32 m_uiVariants = 8;       ///< Vegetation.Composite.WindVariants: shared variants per type and tile.
  float m_fPromoteRadius = 25.0f;  ///< Vegetation.Composite.PromoteRadius: trees this close to the main camera stand alone.
  float m_fTouchMargin = 1.0f;     ///< Metres a touch source may stay outside a tree's radius and still promote it.
  float m_fBlendTime = 0.5f;       ///< Vegetation.Composite.BlendTime: seconds to blend between shared and own.
  float m_fDemoteDelay = 2.0f;     ///< Vegetation.Composite.DemoteDelay: seconds unneeded before a tree blends back.
  float m_fTileReleaseDelay = 2.0f; ///< Seconds a tile's variants outlive its last drawn tree.
};

/// Most shared variants of one type in one tile.
constexpr plUInt32 plVegetationMaxWindVariants = 32;

/// \brief What the wind sharing needs from plFoliageBonePool. The world module forwards to the pool; tests fake it.
class PL_VEGETATIONPLUGIN_DLL plVegetationCompositeWindBackend
{
public:
  static constexpr plUInt32 InvalidInstance = 0xFFFFFFFF;

  virtual ~plVegetationCompositeWindBackend() = default;

  /// A shared variant of the type's merged mesh. InvalidInstance while the mesh is not ready.
  virtual plUInt32 RegisterVariant(plUInt64 uiTypeKey, const plVec3& vWindSample, float fPhase, float fStiffnessScale) = 0;

  /// A standalone instance of the tree. InvalidInstance while the mesh is not ready.
  virtual plUInt32 RegisterTree(plUInt64 uiTypeKey, const plVegetationCellInstance& tree) = 0;

  /// Keeps a standalone tree animating at its transform; called every frame it is registered.
  virtual void UpdateTree(plUInt32 uiInstance, const plVegetationCellInstance& tree) = 0;

  virtual void Unregister(plUInt32& inout_uiInstance) = 0;

  /// Whether the instance's palette holds a pose; out_uiPaletteOffset and out_uiBoneCount locate it.
  virtual bool GetPose(plUInt32 uiInstance, plUInt32& out_uiPaletteOffset, plUInt32& out_uiBoneCount) = 0;
};

/// \brief The shared variants of every composite type, per wind tile, with their lifetimes.
///
/// A tile of a type is marked used every frame a cell that holds the type and overlaps the tile is within draw reach.
/// Its variants register on the first use and are released once the tile went unused for m_fTileReleaseDelay. A tile is
/// ready - trees may borrow its poses - once every variant registered and holds a pose; readiness changes and releases are
/// reported so cells rewrite their trees' bone data only then.
class PL_VEGETATIONPLUGIN_DLL plVegetationCompositeWindTiles
{
public:
  struct Tile
  {
    plUInt64 m_uiTypeKey = 0;
    plUInt32 m_uiTypeChangeCounter = 0;
    plInt32 m_iX = 0;
    plInt32 m_iY = 0;
    plVec3 m_vSample = plVec3::MakeZero();
    plUInt32 m_uiVariantCount = 0;
    plUInt32 m_Variants[plVegetationMaxWindVariants];
    plUInt32 m_PaletteOffsets[plVegetationMaxWindVariants];
    plUInt32 m_uiBoneCount = 0;
    bool m_bReady = false;
    bool m_bUsed = false;
    float m_fUnusedTime = 0.0f;
  };

  /// A tile that became ready or stopped being ready (released or re-registered): its trees' bone data is stale.
  struct Change
  {
    PL_DECLARE_POD_TYPE();
    plUInt64 m_uiTypeKey = 0;
    plInt32 m_iX = 0;
    plInt32 m_iY = 0;
  };

  static plUInt64 MakeKey(plUInt64 uiTypeKey, plInt32 iX, plInt32 iY);
  static void TileCoords(const plVec3& vPosition, float fTileSize, plInt32& out_iX, plInt32& out_iY);

  /// Deterministic: the same tree (position and colour seed) always borrows the same variant.
  static plUInt32 ChooseVariant(const plVec3& vPosition, plUInt8 uiSeed, plUInt32 uiVariantCount);

  /// Deterministic per tile and variant: phases spread over the cycle with a little jitter, stiffness within +-20 %.
  static void ComputeVariantParams(plUInt64 uiTileKey, plUInt32 uiVariant, plUInt32 uiVariantCount, float& out_fPhase, float& out_fStiffnessScale);

  /// Starts a frame: every tile is unused until marked again.
  void BeginFrame();

  /// The tile of a type is wanted this frame. A tile of an older type change counter is released and made again.
  /// fSampleHeight is the height its variants sample the wind at (the first time it is made).
  void MarkUsed(plUInt64 uiTypeKey, plUInt32 uiTypeChangeCounter, plInt32 iX, plInt32 iY, float fSampleHeight, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, plDynamicArray<Change>& out_changes);

  /// Registers missing variants, checks pending tiles for readiness and releases tiles unused for too long.
  void Update(float fDeltaTime, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, plDynamicArray<Change>& out_changes);

  void ReleaseAll(plVegetationCompositeWindBackend& ref_backend);

  const Tile* Find(plUInt64 uiTypeKey, plInt32 iX, plInt32 iY) const;

  /// The shared palette a tree borrows. False while its tile is not ready.
  bool GetTreePalette(plUInt64 uiTypeKey, const plVegetationCellInstance& tree, float fTileSize, plUInt32& out_uiPaletteOffset, plUInt32& out_uiBoneCount) const;

  plUInt32 GetTileCount() const { return m_Tiles.GetCount(); }

  /// Variants registered with the pool over every tile.
  plUInt32 GetVariantCount() const { return m_uiRegisteredVariants; }

private:
  void Release(Tile& ref_tile, plVegetationCompositeWindBackend& ref_backend);

  plHashTable<plUInt64, Tile> m_Tiles;
  plUInt32 m_uiRegisteredVariants = 0;
};

/// \brief Per-tree promotion to and demotion from a standalone pool instance.
class PL_VEGETATIONPLUGIN_DLL plVegetationCompositeTreeWind
{
public:
  enum class Event : plUInt8
  {
    None,    ///< Nothing changed: no bone data to write.
    Promote, ///< The tree wants a standalone instance: register it.
    Blend,   ///< The blend moved, or the own pose is pending: rewrite the tree's bone data this frame.
    Demote,  ///< Fully blended back to shared: unregister it.
  };

  /// Advances one tree by a frame. bWanted: a touch, the camera or a pin needs it standalone. bOwnPoseValid: its own
  /// instance's palette holds a pose (the blend holds until it does).
  static Event Step(plVegetationCellInstance& ref_tree, bool bWanted, bool bOwnPoseValid, float fDeltaTime, const plVegetationCompositeWindSettings& settings);

  /// What the cell writes as plFoliageInstanceBones::BlendWeight: 1 = shared only, 0 = own only.
  static float GetSharedWeight(const plVegetationCellInstance& tree);

  struct TouchSphere
  {
    PL_DECLARE_POD_TYPE();
    plVec3 m_vPosition;
    float m_fRadius;
  };

  struct GroupInputs
  {
    plUInt64 m_uiTypeKey = 0;
    plVec3 m_vCamera = plVec3::MakeZero();
    plVec2 m_vCellMin = plVec2::MakeZero(); ///< the cell's xy footprint
    float m_fCellSize = 32.0f;
    float m_fDrawRange = 0.0f;  ///< trees further than this (plus their radius) are not drawn and never promoted
    float m_fTypeRadius = 1.0f; ///< the type's bounding radius at scale 1
    float m_fMaxScale = 4.0f;   ///< bound on instance scales for the per-cell reach tests
    plArrayPtr<const TouchSphere> m_Touches;
  };

  struct GroupResult
  {
    bool m_bEvaluatedPerTree = false; ///< Trees were looked at one by one (something near, or promoted trees).
    plUInt32 m_uiStandalone = 0;      ///< Trees standing alone, fully blended.
    plUInt32 m_uiBlending = 0;        ///< Trees blending, or waiting for their own pose.
    plUInt32 m_uiPromoted = 0;        ///< Registered this frame.
    plUInt32 m_uiDemoted = 0;         ///< Unregistered this frame.
  };

  /// Steps every tree of a composite group that might want or hold a standalone instance. Cells far from the camera and
  /// every touch source, with nothing promoted or pinned, cost no per-tree work. Promotions and demotions bump
  /// m_uiBonesGeneration; m_bBonesBlending says whether any promoted tree needs its bone data rewritten this frame.
  static void UpdateGroup(plVegetationCellGroup& ref_group, const GroupInputs& inputs, float fDeltaTime, const plVegetationCompositeWindSettings& settings, plVegetationCompositeWindBackend& ref_backend, GroupResult& out_result);

  /// Releases a tree's standalone instance at once (removal, type reload) and resets its blend.
  static void ReleaseTree(plVegetationCellGroup& ref_group, plVegetationCellInstance& ref_tree, plVegetationCompositeWindBackend& ref_backend);
};
