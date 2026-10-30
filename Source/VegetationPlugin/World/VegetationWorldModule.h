#pragma once

#include <Core/World/World.h>
#include <Foundation/Containers/HashTable.h>
#include <Foundation/Containers/IdTable.h>
#include <Foundation/Containers/Map.h>
#include <Foundation/Types/UniquePtr.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>
#include <VegetationPlugin/VegetationDeclarations.h>

class plVegetationCellComponent;
class plVegetationLayerComponent;
class plGrassCoverageData;
struct plGrassCoverageEntry;
struct plGrassCoverageTileId;

/// \brief One instance as stored inside a cell: world placement plus the tile it came from.
struct plVegetationCellInstance
{
  PL_DECLARE_POD_TYPE();

  plVec3 m_vPosition;
  plQuat m_qRotation;
  float m_fScale;
  plUInt8 m_uiColorSeed;
  plBitflags<plVegetationInstanceFlags> m_Flags;
  plVegetationTileId m_TileId;
  plUInt32 m_uiIndexInTile;
};

/// \brief All instances of one type inside one cell.
struct plVegetationCellGroup
{
  plVegetationTypeResourceHandle m_hType;
  plDynamicArray<plVegetationCellInstance> m_Instances;
};

/// \brief A 32 m square column of the world holding every vegetation instance inside it, rendered by one
/// plVegetationCellComponent on an internal game object.
struct plVegetationCell
{
  plInt32 m_iX = 0;
  plInt32 m_iY = 0;
  plVec3 m_vOrigin = plVec3::MakeZero();
  plGameObjectHandle m_hObject;
  plComponentHandle m_hComponent;
  plHybridArray<plVegetationCellGroup, 4> m_Groups;
  plUInt32 m_uiVersion = 0; ///< Bumped whenever groups or instances change; the renderer rebuilds its state on mismatch.

  plUInt32 GetInstanceCount() const;
};

/// \brief Owner of every vegetation instance in a world. See plVegetationWorldModuleInterface.
class PL_VEGETATIONPLUGIN_DLL plVegetationWorldModule : public plVegetationWorldModuleInterface
{
  PL_DECLARE_WORLD_MODULE();
  PL_ADD_DYNAMIC_REFLECTION(plVegetationWorldModule, plVegetationWorldModuleInterface);

public:
  plVegetationWorldModule(plWorld* pWorld);
  ~plVegetationWorldModule();

  virtual void Initialize() override;
  virtual void Deinitialize() override;
  virtual void WorldClear() override;

  // plVegetationWorldModuleInterface
  virtual plVegetationTileHandle AddInstances(plStringView sTypeResource, plArrayPtr<const plVegetationInstanceDesc> instances, plVegetationSource::Enum source) override;
  virtual void RemoveTile(plVegetationTileHandle hTile) override;
  virtual void QueryInstances(const plBoundingSphere& sphere, plDynamicArray<plVegetationInstanceRef>& out_instances) const override;
  virtual void RemoveInstances(plArrayPtr<const plVegetationInstanceRef> instances) override;
  virtual void RegisterTouchSource(const plVec3& vPosition, float fRadius, float fStrength, const plVec3& vVelocity) override;

  /// \brief Same as the string overload with an already resolved type.
  plVegetationTileHandle AddInstances(const plVegetationTypeResourceHandle& hType, plArrayPtr<const plVegetationInstanceDesc> instances, plVegetationSource::Enum source);

  plUInt32 GetTotalInstanceCount() const { return m_uiTotalInstances; }
  plUInt32 GetCellCount() const { return m_Cells.GetCount(); }

  /// \brief Position and projection scale of the last main or editor view that extracted a cell; shadow views use it
  /// for LOD selection because their own cameras are orthographic.
  void ReportMainCamera(const plVec3& vPosition, float fTanHalfFovY) const;
  plVec3 GetMainCameraPosition() const { return m_vMainCameraPosition; }
  float GetMainCameraTanHalfFov() const { return m_fMainCameraTanHalfFov; }

  /// \brief Touch sources registered this frame, consumed by the touch map.
  struct TouchSource
  {
    plVec3 m_vPosition;
    float m_fRadius;
    plVec3 m_vVelocity;
    float m_fStrength;
  };
  plArrayPtr<const TouchSource> GetTouchSources() const { return m_TouchSources; }

  /// \brief Painted grass sources (one per layer and grass type) currently in the GPU pool.
  plUInt32 GetGrassSourceCount() const { return m_GrassSources.GetCount(); }

private:
  /// \brief Streaming state of one coverage tile of a grass source.
  struct GrassTileState
  {
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid(); ///< layer space, every strand pose of the tile
    plUInt64 m_uiContentHash = 0;
    float m_fDistance = 0.0f; ///< to the nearest streaming camera
    float m_fDensity = 1.0f;  ///< cull-side density scale set under pool pressure
    plUInt32 m_uiSampleStride = 1; ///< power-of-two resident root sampling
    plUInt32 m_uiRequestedStride = 1;
    bool m_bResident = false; ///< the tile's strands are in the pool
    bool m_bDirty = true;     ///< the coverage changed since the resident strands were grown
    bool m_bSeen = false;
  };

  /// \brief One grass type of one painted layer in the GPU strand pool, drawn through a generated render object.
  struct GrassLayerSource
  {
    plComponentHandle m_hLayer;
    plString m_sType;
    plUInt32 m_uiTypeSeed = 0;
    plGrassTypeResourceHandle m_hType;
    plUInt32 m_uiTypeChangeCounter = 0;
    plGrassSettings m_Settings;
    plMaterialResourceHandle m_hMaterial;
    float m_fTextureAspect = 1.0f;
    plGrassSourceHandle m_hSource;
    plGameObjectHandle m_hRenderObject;
    plComponentHandle m_hRenderComponent;
    plUInt32 m_uiCoverageCounter = 0;
    float m_fIndexedTexelSize = 0.0f;
    bool m_bIndexed = false;
    bool m_bSeen = false;
    bool m_bBoundsDirty = true;
    plHashTable<plUInt32, GrassTileState> m_Tiles; ///< every non-empty coverage tile, by plGrassCoverageTile::MakeKey
    plBoundingBox m_RenderBounds = plBoundingBox::MakeInvalid(); ///< layer space, union of the resident tiles
    plBoundingBox m_ChangedCasterBox = plBoundingBox::MakeInvalid(); ///< world space, tiles grown or freed this frame
    plBoundingBox m_LastCasterBox = plBoundingBox::MakeInvalid(); ///< world space, m_RenderBounds at the last update
    plTransform m_LastCasterTransform = plTransform::MakeIdentity();
    bool m_bCastsShadows = true; ///< type Cast Shadows and Vegetation.Grass.Shadows at the last update

    // Valid during UpdateGrass only.
    const plGrassCoverageEntry* m_pEntry = nullptr;
    float m_fTexelSize = 0.5f;
    plTransform m_LayerTransform = plTransform::MakeIdentity();
  };

  /// \brief Position of a main or editor view of this world, kept for a few seconds after the view last rendered.
  struct GrassCamera
  {
    plViewHandle m_hView;
    plVec3 m_vPosition = plVec3::MakeZero();
    plTime m_LastSeen;
  };

  /// Streams painted grass in the world update: indexes every active layer's coverage tiles per grass type, makes the
  /// tiles near a camera resident nearest first within the per-frame upload budget and frees far tiles. While the pool
  /// is full, a tile that does not fit evicts the farthest resident tiles beyond it, and the farthest tiles thin.
  /// Extraction never changes the pool.
  void UpdateGrass(const plWorldModule::UpdateContext& context);
  bool PrepareGrassSource(GrassLayerSource& ref_source, const plVegetationLayerComponent& layer);
  void UpdateGrassCameras();
  void IndexGrassTiles(GrassLayerSource& ref_source, const plGrassCoverageData& coverage, const plGrassCoverageEntry& entry, plUInt32 uiCoverageCounter);
  void IndexGrassTile(GrassLayerSource& ref_source, const plGrassCoverageEntry& entry, plUInt32 uiTileKey);
  void FreeGrassTile(GrassLayerSource& ref_source, plUInt32 uiTileKey, GrassTileState& ref_tile);

  /// Frees the farthest resident tile of any source whose camera distance exceeds fFartherThan. False when none does.
  bool EvictFarthestGrassTile(float fFartherThan);
  void AddGrassCasterChange(GrassLayerSource& ref_source, const GrassTileState& tile);
  void StreamGrassTiles();
  void UpdateGrassBudgetThinning();
  void UpdateGrassRenderObject(GrassLayerSource& ref_source, const plVegetationLayerComponent& layer);
  void DestroyGrassSource(GrassLayerSource& ref_source, bool bDeleteObjectNow = true);

  plDynamicArray<GrassLayerSource*> m_GrassSources; ///< owned
  plHybridArray<GrassCamera, 4> m_GrassCameras;
  bool m_bGrassRefusing = false; ///< a new tile did not fit the pool; new tiles wait until a tile is freed
  bool m_bGrassPressure = false; ///< thinning is active; ends once the pool is at most 90 % used
  plUInt32 m_uiGrassBudgetPages = 0;
  plUInt32 m_uiGrassResidentTiles = 0;
  plUInt32 m_uiGrassPendingTiles = 0; ///< tiles in range that wait for an upload
  struct Tile
  {
    plVegetationTypeResourceHandle m_hType;
    plVegetationSource::Enum m_Source = plVegetationSource::Script;
    plUInt32 m_uiInstanceCount = 0;
    plHybridArray<plUInt64, 8> m_Cells; ///< Keys of every cell holding instances of this tile.
  };

  static plUInt64 MakeCellKey(plInt32 iX, plInt32 iY);
  static void CellCoordsForPosition(const plVec3& vPosition, plInt32& out_iX, plInt32& out_iY);

  plVegetationCell& GetOrCreateCell(plInt32 iX, plInt32 iY);
  void DestroyCell(plUInt64 uiKey);
  void RemoveTileFromCell(plVegetationCell& cell, plVegetationTileId tileId);
  void NotifyCellChanged(plVegetationCell& cell);
  void BeginFrame(const plWorldModule::UpdateContext& context);
  void PublishTouchMap(const plWorldModule::UpdateContext& context);

  /// Registers a touch source for every dynamic or character physics actor inside the touch map around vFocus, nearest
  /// first up to Vegetation.Touch.MaxPhysicsSources. Actors carrying a plVegetationTouchComponent are skipped.
  void GatherPhysicsTouchSources(const plVec3& vFocus);

  /// Last position of a physics actor that registered a touch source, for its velocity.
  struct PhysicsTouchState
  {
    plVec3 m_vLastPosition = plVec3::MakeZero();
    plUInt32 m_uiLastSeenFrame = 0;
  };
  plHashTable<plGameObjectHandle, PhysicsTouchState> m_PhysicsTouchStates;
  plUInt32 m_uiPhysicsTouchFrame = 0;

  plIdTable<plVegetationTileId, Tile> m_Tiles;
  plMap<plUInt64, plVegetationCell*> m_Cells; ///< Owned; freed in DestroyCell / WorldClear.
  plUInt32 m_uiTotalInstances = 0;

  mutable plVec3 m_vMainCameraPosition = plVec3::MakeZero();
  mutable float m_fMainCameraTanHalfFov = 0.5f;

  plHybridArray<TouchSource, 32> m_TouchSources;
};
