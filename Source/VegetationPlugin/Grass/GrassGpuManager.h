#pragma once

#include <Foundation/Math/BoundingBox.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Time/Time.h>
#include <Foundation/Types/Id.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

struct plGrassCoverageTile;
struct plGrassStrandStatic;
struct plRenderWorldRenderEvent;

enum class plGrassAOMode
{
  None = 0,
  GrassOnly = 1,
  All = 2
};

using plGrassSourceId = plGenericId<20, 12>;

/// \brief One grass source in a world's GPU strand pool: a plGrassSurfaceComponent or one grass type of a painted layer.
class plGrassSourceHandle
{
  PL_DECLARE_HANDLE_TYPE(plGrassSourceHandle, plGrassSourceId);
};

/// \brief One resident pool page as plGrassCullPass's per-view prefilter sees it: the world-space box every strand of
/// the page stays inside, and the distances of its source. An invalid box keeps the page in every view except shadow
/// views of a source that casts no shadows.
struct PL_VEGETATIONPLUGIN_DLL plGrassPageCullInfo
{
  PL_DECLARE_POD_TYPE();

  plBoundingBox m_WorldBounds = plBoundingBox::MakeInvalid();
  float m_fViewDistance = 0.0f;
  float m_fShadowDistance = 0.0f; ///< scaled, as the GPU cull applies it
  plUInt32 m_uiPage = 0;
  plUInt32 m_uiSource = 0;
  bool m_bCastShadows = true; ///< the source type's Cast Shadows; false skips the page in every shadow view
};

/// \brief A world's strand pool as the render side sees it for the current frame. Written at BeginRender; read by
/// plGrassCullPass when it declares or re-validates its graph and passed on to plGrassRenderer.
struct PL_VEGETATIONPLUGIN_DLL plGrassRenderSnapshot
{
  plGALBufferHandle m_hStatic;
  plGALBufferHandle m_hState;
  plGALBufferHandle m_hPages;
  plGALBufferHandle m_hSources;
  plGALBufferHandle m_hAtlas;
  plGALBufferHandle m_hResidentPages;

  plUInt32 m_uiPageCount = 0;         ///< pages the pool buffers hold
  plUInt32 m_uiPoolStrands = 0;       ///< m_uiPageCount * page size
  plUInt32 m_uiStateSegments = 0;     ///< segments the state buffer holds
  plUInt32 m_uiSourceCount = 0;       ///< source slots in the source table
  plUInt32 m_uiAtlasCount = 0;        ///< entries in the atlas table
  plUInt32 m_uiResidentPageCount = 0; ///< pages owned by a source
  plUInt32 m_uiVisibleEntries = 0;    ///< entries a per-view visible buffer needs
  plDynamicArray<plGrassPageCullInfo> m_ResidentPages; ///< one entry per resident page; each view culls a subset
  plVec3 m_vMainCamera = plVec3::MakeZero();
  plUInt64 m_uiFrame = 0;
  bool m_bValid = false;

  bool IsUsable() const { return m_bValid && m_uiResidentPageCount > 0 && m_uiSourceCount > 0; }
};

/// \brief Owns every world's GPU grass: a pool of fixed 4096-strand pages (static data and simulation state), the
/// per-source tables and the upload queue. At BeginRender, after the wind field and touch map have advanced, it sizes
/// the buffers and enqueues a PreRender render graph that uploads the frame's tables, copies grown pool buffers and
/// runs the generation and simulation dispatches of every world. PreRender graphs execute before the views' graphs, so
/// the cull and draw passes of the same frame read the pool this graph wrote; every access is declared, so the graph
/// places the barriers. Layouts are documented in Shaders/Vegetation/Grass/GrassGpuData.h.
///
/// Every world-side call happens on the update thread and only changes CPU bookkeeping and the queue; GPU work runs
/// at BeginRender. Extraction does not call the manager. Sources are addressed per world (plWorld::GetIndex).
class PL_VEGETATIONPLUGIN_DLL plGrassGpuManager
{
public:
  /// \brief True while Vegetation.Grass.Enable is set.
  static bool IsEnabled();

  /// Procedural root/cover AO and receiver contact AO, controlled by Vegetation.Grass.AO.Mode.
  static plGrassAOMode GetAOMode();

  /// \brief Whether grass casts shadows: Vegetation.Grass.Shadows, or Rendering.Shadows.Quality >= 1 while it follows.
  /// Off: grass render objects carry no shadow tags, shadow views skip grass and grass performs no shadow cache
  /// invalidation.
  static bool AreShadowsEnabled();

  /// \brief Multiplies the Shadow Distance of every Grass Type: Vegetation.Grass.ShadowDistanceScale clamped to 0 - 1,
  /// halved on shadow quality 1 while Vegetation.Grass.Shadows follows the tier.
  static float GetShadowDistanceScale();

  /// \brief Registers a source with its type settings and object transform. The source owns no strands until tiles or
  /// roots are uploaded.
  static plGrassSourceHandle AllocateSource(plUInt32 uiWorldIndex, const plGrassSettings& settings, const plTransform& transform, float fTextureAspect);

  /// \brief Frees the source's strands and slot.
  static void Free(plUInt32 uiWorldIndex, plGrassSourceHandle& inout_hSource);

  /// \brief The object transform the source's strands are relative to.
  static void SetSourceTransform(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plTransform& transform);

  /// \brief Replaces the source's type settings without touching its strands: the source table is rebuilt from them
  /// every frame, so simulation, distance, billboard and touch values take effect at once. Fails when the segment count
  /// differs, since it shapes the state layout; growth values (plGrassSettings::RequiresRegrow) are the caller's
  /// responsibility.
  static plResult UpdateSourceSettings(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plGrassSettings& settings, float fTextureAspect);

  /// \brief Replaces the strands of one coverage tile. The strand count of each texel is
  /// GrassStrandCountForTexelQ8(density, GrassStrandsPerTexelQ8(strands per m^2, texel size), GrassHash(...)); the GPU
  /// grows exactly that many strands from the uploaded 2 KB of texels and their prefix offsets. An empty tile only
  /// frees. A regrow that fits the tile's previous strands reuses them in place; a larger one allocates before it frees.
  /// Fails when the pool is full, in which case the tile keeps its previous strands (if any). A replaced tile keeps its
  /// density scale.
  static plResult UploadCoverageTile(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plGrassCoverageTile& tile, plUInt32 uiTypeSeed, float fTexelSize, plUInt32 uiSampleStride = 1);

  /// \brief Frees the strands of one coverage tile.
  static void RemoveCoverageTile(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plUInt32 uiTileKey);

  /// \brief Cull-side density scale of a resident coverage tile, 1/255 .. 1. The cull pass drops that fraction of the
  /// strands of every page the tile occupies; a page shared by several chunks uses the largest of their scales.
  static void SetCoverageTileDensity(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plUInt32 uiTileKey, float fDensity);

  /// \brief Pool pages owned by a source and the page budget (Vegetation.Grass.MaxStrands in whole pages).
  static void GetPoolUsage(plUInt32 uiWorldIndex, plUInt32& out_uiUsedPages, plUInt32& out_uiBudgetPages);

  /// \brief Frees every tile and root strand of the source, keeping the slot.
  static void ClearSource(plUInt32 uiWorldIndex, plGrassSourceHandle hSource);

  /// \brief Replaces the source's root strands with CPU-built static records (plGrassSurfaceComponent). Fails when the
  /// pool is full.
  static plResult UploadRoots(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plArrayPtr<const plGrassStrandStatic> roots, const plBoundingBox& localBounds);

  /// \brief Packs one CPU-grown strand for UploadRoots: source-space root and growth axis, length in metres, tangent
  /// angle as a 0..1 fraction of a turn, atlas rect index and a hash seed.
  static void MakeStrandStatic(const plVec3& vRoot, const plVec3& vAxis, float fLength, float fTangentAngle01, plUInt32 uiAtlasIndex, plUInt32 uiSeed, plGrassStrandStatic& out_strand);

  /// \brief GPU source table index of a source, for plGrassRenderData. plInvalidIndex for a stale handle.
  static plUInt32 GetSourceSlot(plUInt32 uiWorldIndex, plGrassSourceHandle hSource);

  /// \brief Per frame from the world update: the main camera (simulation distance reference), the world's time step
  /// and whether the world simulates. A world that stops submitting keeps its pool but stops simulating.
  static void SubmitWorldFrame(plUInt32 uiWorldIndex, const plVec3& vMainCamera, bool bHasCamera, plTime deltaTime, bool bSimulate);

  /// \brief Copies the world's render snapshot of the current frame. False when the world has no pool.
  static bool GetRenderSnapshot(plUInt32 uiWorldIndex, plGrassRenderSnapshot& out_snapshot);

  /// \brief Strands in the world's pool pages and strands simulated last frame.
  static void GetStatistics(plUInt32 uiWorldIndex, plUInt32& out_uiResidentStrands, plUInt32& out_uiSimulatedStrands, plUInt32& out_uiCapacityStrands);

  static void OnStartup();
  static void OnShutdown();

private:
  static void OnRenderEvent(const plRenderWorldRenderEvent& e);

  struct Data;
  static Data* s_pData;
};
