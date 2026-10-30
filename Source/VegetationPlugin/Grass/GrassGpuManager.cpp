#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Utilities/Stats.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphManager.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererCore/Wind/TouchMapManager.h>
#include <RendererCore/Wind/WindFieldManager.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Profiling/Profiling.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>

#include <Shaders/Vegetation/Grass/GrassConstants.h>
#include <Shaders/Vegetation/Grass/GrassGpuData.h>

plCVarBool cvar_VegetationGrassEnable("Vegetation.Grass.Enable", true, plCVarFlags::Default, "Generates, simulates and renders GPU grass");
plCVarInt cvar_VegetationGrassAOMode("Vegetation.Grass.AO.Mode", 2, plCVarFlags::Save, "Grass AO: 0=None, 1=Grass Only (roots and distant cover), 2=All (also ground contact). Scene/texture AO is unchanged.");
plCVarInt cvar_VegetationGrassMaxStrands("Vegetation.Grass.MaxStrands", 524288, plCVarFlags::Default, "Strand budget of each world's grass pool, in whole 4096-strand pages. Lowering it refuses new strands but keeps resident ones");
plCVarBool cvar_VegetationGrassSimulate("Vegetation.Grass.Simulate", true, plCVarFlags::Default, "Runs the GPU strand simulation for pages within Simulation Distance. Off: no page is flagged simulated and every strand draws its analytic rest pose from the vertex shader, as a world with simulation disabled (asset thumbnails) does");
plCVarInt cvar_VegetationGrassShadows("Vegetation.Grass.Shadows", -1, plCVarFlags::Save, "Lets grass cast shadows: -1 follows Rendering.Shadows.Quality, 0 forces off, 1 forces on. Off: the render objects carry no CastShadow and no VSMContinuousShadow tag, every shadow view skips grass and no shadow cache is invalidated for grass");
plCVarFloat cvar_VegetationGrassShadowDistanceScale("Vegetation.Grass.ShadowDistanceScale", 1.0f, plCVarFlags::Save, "Scales the Shadow Distance of every Grass Type, clamped to 0 - 1, on top of the scale the shadow quality tier contributes. At 0 no grass reaches a shadow view");

// The engine's shadow quality tier, which the quality presets set (low 0, medium 1, high and max 2).
PL_RENDERERCORE_DLL extern plCVarInt cvar_RenderingShadowsQuality;

namespace
{
  constexpr plUInt32 s_uiGrassInvalid = 0xFFFFFFFFu;
  constexpr plUInt32 s_uiGrassMaxPages = 65535; // a dispatch covers one page per thread group row
  constexpr float s_fGrassStepTime = 1.0f / 60.0f;

  struct GrassRange
  {
    PL_DECLARE_POD_TYPE();
    plUInt32 m_uiFirst = 0; ///< source-local strand index
    plUInt32 m_uiCount = 0;
  };

  struct GrassChunk
  {
    GrassRange m_Range;
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid(); ///< source-space roots
    plUInt8 m_uiDensity = 255;                             ///< cull-side keep fraction, unorm8
  };

  enum class GrassCommandType : plUInt8
  {
    Kill,
    Coverage,
    Roots,
  };

  struct GrassCommand
  {
    GrassCommandType m_Type = GrassCommandType::Kill;
    plUInt32 m_uiSourceSlot = 0;
    plUInt32 m_uiGlobalFirst = 0;
    plUInt32 m_uiCount = 0;
    plUInt32 m_uiChunkFirst = 0;
    plUInt32 m_uiUploadOffset = 0;
    plInt32 m_iTileX = 0;
    plInt32 m_iTileY = 0;
    plUInt32 m_uiTypeSeed = 0;
    plUInt32 m_uiSampleStride = 1;
    float m_fTexelSize = 0.5f;
    float m_fBaseHeight = 0.0f;
  };

  plUInt32 GrassMaxPages()
  {
    const plInt32 iStrands = plMath::Max(cvar_VegetationGrassMaxStrands.GetValue(), 1);
    return plMath::Clamp<plUInt32>((static_cast<plUInt32>(iStrands) + GRASS_PAGE_SIZE - 1) / GRASS_PAGE_SIZE, 1u, s_uiGrassMaxPages);
  }

  plUInt32 GrassSegments(const plGrassSettings& settings)
  {
    // Nested subdivisions make the geometric LOD morph exact at both transitions.
    if (settings.m_bProceduralBlades)
      return settings.m_uiSegmentCount > 4 ? 8u : 4u;
    return plMath::Clamp<plUInt32>(settings.m_uiSegmentCount, 1u, GRASS_MAX_SEGMENTS);
  }
} // namespace

struct plGrassGpuManager::Data
{
  struct Source
  {
    plUInt32 m_uiGeneration = 1;
    bool m_bUsed = false;
    plGrassSettings m_Settings;
    float m_fTextureAspect = 1.0f;
    plTransform m_Transform = plTransform::MakeIdentity();
    plTransform m_LastUploadedTransform = plTransform::MakeIdentity();
    bool m_bHasLastTransform = false;
    plDynamicArray<plUInt32> m_Pages;          ///< global page of each source-local page
    plDynamicArray<GrassRange> m_FreeRanges;   ///< sorted, merged, source-local
    plHashTable<plUInt32, GrassChunk> m_Tiles; ///< coverage tiles by plGrassCoverageTile::MakeKey
    GrassChunk m_Roots;
    bool m_bHasRoots = false;
  };

  struct Page
  {
    plUInt32 m_uiSource = s_uiGrassInvalid;
    plUInt32 m_uiStateUnit = 0; ///< first state unit; a unit is GRASS_PAGE_SIZE segments
    plUInt32 m_uiStateUnitCount = 0;
    plBoundingBox m_LocalBounds = plBoundingBox::MakeInvalid();
    bool m_bSimulated = false;
    bool m_bNeedsWake = false;
  };

  struct PendingSlot
  {
    plUInt32 m_uiSlot = 0;
    plUInt64 m_uiFrame = 0;
  };

  struct World
  {
    plDynamicArray<Source> m_Sources;
    plDynamicArray<plUInt32> m_FreeSlots;
    plDynamicArray<PendingSlot> m_PendingSlots; ///< freed slots wait a few frames so stale render data never meets a new source
    plUInt32 m_uiUsedSources = 0;

    plDynamicArray<Page> m_Pages; ///< CPU capacity; the GPU buffers follow at BeginRender
    plDynamicArray<plUInt32> m_FreePages;
    plDynamicArray<bool> m_StateUnits;

    plDynamicArray<GrassCommand> m_Commands;
    plDynamicArray<plUInt32> m_Upload;

    plVec3 m_vMainCamera = plVec3::MakeZero();
    bool m_bHasCamera = false;
    bool m_bSimulate = false;
    plTime m_DeltaTime;
    plUInt64 m_uiSubmitFrame = 0;
    float m_fAccumulatedTime = 0.0f;

    plGALBufferHandle m_hStatic;
    plGALBufferHandle m_hState;
    plGALBufferHandle m_hPages;
    plGALBufferHandle m_hSources;
    plGALBufferHandle m_hAtlas;
    plGALBufferHandle m_hResident;
    plGALBufferHandle m_hActive;
    plGALBufferHandle m_hUpload;
    plUInt32 m_uiGpuPages = 0;
    plUInt32 m_uiGpuStateUnits = 0;
    plUInt32 m_uiGpuSources = 0;
    plUInt32 m_uiGpuAtlas = 0;
    plUInt32 m_uiGpuUploadUints = 0;

    plGrassRenderSnapshot m_Snapshot;
    plUInt32 m_uiResidentStrands = 0;
    plUInt32 m_uiSimulatedStrands = 0;
    plUInt32 m_uiUsedPages = 0;
    plUInt32 m_uiUploadBytes = 0;    ///< upload buffer bytes of the last processed frame
    plUInt32 m_uiThinnedChunks = 0;  ///< tile chunks with a density below 255
    plUInt32 m_uiIdleFrames = 0;
    bool m_bLoggedFull = false;
  };

  /// One world's GPU work of the current frame, recorded by the passes of the grass PreRender graph. Rebuilt at every
  /// BeginRender; the previous frame's recording has joined by then (plRenderWorld::EndFrame finishes it).
  struct FrameWork
  {
    plUInt32 m_uiWorldIndex = 0;
    plGALBufferHandle m_hStatic;
    plGALBufferHandle m_hState;
    plGALBufferHandle m_hPages;
    plGALBufferHandle m_hSources;
    plGALBufferHandle m_hAtlas;
    plGALBufferHandle m_hResident;
    plGALBufferHandle m_hActive;
    plGALBufferHandle m_hUpload;

    // Pool buffers replaced this frame. DestroyBuffer defers destruction to the device's end of frame, after the
    // graph recorded the copy into the replacement.
    plGALBufferHandle m_hOldStatic;
    plGALBufferHandle m_hOldState;
    plUInt32 m_uiOldStaticBytes = 0;
    plUInt32 m_uiOldStateBytes = 0;

    plDynamicArray<plGrassPageGpu> m_Pages;
    plDynamicArray<plGrassSourceGpu> m_Sources;
    plDynamicArray<plGrassAtlasGpu> m_Atlas;
    plDynamicArray<plUInt32> m_Resident;
    plDynamicArray<plUInt32> m_Active;
    plDynamicArray<plUInt32> m_Upload;
    plDynamicArray<GrassCommand> m_Commands; ///< sorted by wave
    plDynamicArray<plUInt32> m_WaveStarts;   ///< first command of each wave, then the command count

    plUInt32 m_uiPageCount = 0;
    plUInt32 m_uiPoolStrands = 0;
    plUInt32 m_uiStateSegments = 0;
    plUInt32 m_uiSourceCount = 0;
    plUInt32 m_uiSteps = 0;
  };

  plMutex m_Mutex;
  plHashTable<plUInt32, World*> m_Worlds;
  plShaderResourceHandle m_hGenerateShader;
  plShaderResourceHandle m_hSimulateShader;
  plDynamicArray<plUniquePtr<FrameWork>> m_FrameWork;
  plSharedPtr<plRenderGraph> m_pGraph;

  World& GetOrCreateWorld(plUInt32 uiWorldIndex)
  {
    World* pWorld = nullptr;
    if (!m_Worlds.TryGetValue(uiWorldIndex, pWorld))
    {
      pWorld = PL_DEFAULT_NEW(World);
      m_Worlds.Insert(uiWorldIndex, pWorld);
    }
    return *pWorld;
  }

  World* FindWorld(plUInt32 uiWorldIndex)
  {
    World* pWorld = nullptr;
    m_Worlds.TryGetValue(uiWorldIndex, pWorld);
    return pWorld;
  }

  static Source* FindSource(World& world, plGrassSourceHandle hSource, plUInt32* out_pSlot = nullptr)
  {
    if (hSource.IsInvalidated())
      return nullptr;

    const plUInt32 uiSlot = hSource.GetInternalID().m_InstanceIndex;
    if (uiSlot >= world.m_Sources.GetCount())
      return nullptr;

    Source& source = world.m_Sources[uiSlot];
    if (!source.m_bUsed || (source.m_uiGeneration & 0xFFF) != hSource.GetInternalID().m_Generation)
      return nullptr;

    if (out_pSlot)
      *out_pSlot = uiSlot;
    return &source;
  }

  // Splits a source-local range into per-page pieces and hands each piece's global first strand and offset.
  template <typename Func>
  static void ForEachPagePiece(const Source& source, const GrassRange& range, Func func)
  {
    plUInt32 uiDone = 0;
    while (uiDone < range.m_uiCount)
    {
      const plUInt32 uiLocal = range.m_uiFirst + uiDone;
      const plUInt32 uiLocalPage = uiLocal / GRASS_PAGE_SIZE;
      const plUInt32 uiInPage = uiLocal % GRASS_PAGE_SIZE;
      const plUInt32 uiCount = plMath::Min(range.m_uiCount - uiDone, GRASS_PAGE_SIZE - uiInPage);
      if (uiLocalPage < source.m_Pages.GetCount())
      {
        func(source.m_Pages[uiLocalPage], source.m_Pages[uiLocalPage] * GRASS_PAGE_SIZE + uiInPage, uiDone, uiCount);
      }
      uiDone += uiCount;
    }
  }

  static void QueueKill(World& world, plUInt32 uiGlobalFirst, plUInt32 uiCount)
  {
    GrassCommand& cmd = world.m_Commands.ExpandAndGetRef();
    cmd.m_Type = GrassCommandType::Kill;
    cmd.m_uiGlobalFirst = uiGlobalFirst;
    cmd.m_uiCount = uiCount;
  }

  static void InsertFreeRange(Source& source, GrassRange range)
  {
    plUInt32 uiInsert = 0;
    while (uiInsert < source.m_FreeRanges.GetCount() && source.m_FreeRanges[uiInsert].m_uiFirst < range.m_uiFirst)
      ++uiInsert;

    source.m_FreeRanges.InsertAt(uiInsert, range);

    // merge with the successor, then with the predecessor
    if (uiInsert + 1 < source.m_FreeRanges.GetCount())
    {
      GrassRange& cur = source.m_FreeRanges[uiInsert];
      const GrassRange next = source.m_FreeRanges[uiInsert + 1];
      if (cur.m_uiFirst + cur.m_uiCount == next.m_uiFirst)
      {
        cur.m_uiCount += next.m_uiCount;
        source.m_FreeRanges.RemoveAtAndCopy(uiInsert + 1);
      }
    }
    if (uiInsert > 0)
    {
      GrassRange& prev = source.m_FreeRanges[uiInsert - 1];
      const GrassRange cur = source.m_FreeRanges[uiInsert];
      if (prev.m_uiFirst + prev.m_uiCount == cur.m_uiFirst)
      {
        prev.m_uiCount += cur.m_uiCount;
        source.m_FreeRanges.RemoveAtAndCopy(uiInsert);
      }
    }
  }

  static bool AllocateStateUnits(World& world, plUInt32 uiUnits, plUInt32& out_uiFirst)
  {
    const plUInt32 uiMaxUnits = GrassMaxPages() * GRASS_MAX_SEGMENTS;

    for (int iPass = 0; iPass < 2; ++iPass)
    {
      plUInt32 uiRun = 0;
      for (plUInt32 i = 0; i < world.m_StateUnits.GetCount(); ++i)
      {
        uiRun = world.m_StateUnits[i] ? 0 : uiRun + 1;
        if (uiRun == uiUnits)
        {
          out_uiFirst = i + 1 - uiUnits;
          for (plUInt32 u = out_uiFirst; u <= i; ++u)
            world.m_StateUnits[u] = true;
          return true;
        }
      }

      // Grow by at least the request, doubling while small.
      const plUInt32 uiOld = world.m_StateUnits.GetCount();
      const plUInt32 uiNew = plMath::Min(uiMaxUnits, plMath::Max(uiOld + uiUnits, uiOld * 2));
      if (uiNew <= uiOld)
        return false;
      world.m_StateUnits.SetCount(uiNew, false);
    }

    return false;
  }

  static bool AcquirePage(World& world, plUInt32 uiSlot, plUInt32& out_uiPage)
  {
    if (world.m_FreePages.IsEmpty())
    {
      const plUInt32 uiOld = world.m_Pages.GetCount();
      const plUInt32 uiNew = plMath::Min(GrassMaxPages(), plMath::Max(uiOld + 1, uiOld * 2));
      if (uiNew <= uiOld)
        return false;

      world.m_Pages.SetCount(uiNew);
      for (plUInt32 p = uiNew; p > uiOld; --p)
        world.m_FreePages.PushBack(p - 1);
    }

    out_uiPage = world.m_FreePages.PeekBack();
    world.m_FreePages.PopBack();

    Page& page = world.m_Pages[out_uiPage];
    page = Page();
    page.m_uiSource = uiSlot;
    // Simulation storage is acquired only when this page approaches the simulation camera.

    // Whatever the previous owner left must not render.
    QueueKill(world, out_uiPage * GRASS_PAGE_SIZE, GRASS_PAGE_SIZE);
    return true;
  }

  static void ReleasePage(World& world, plUInt32 uiPage)
  {
    Page& page = world.m_Pages[uiPage];
    for (plUInt32 u = 0; u < page.m_uiStateUnitCount; ++u)
    {
      if (page.m_uiStateUnit + u < world.m_StateUnits.GetCount())
        world.m_StateUnits[page.m_uiStateUnit + u] = false;
    }

    QueueKill(world, uiPage * GRASS_PAGE_SIZE, GRASS_PAGE_SIZE);
    page = Page();
    world.m_FreePages.PushBack(uiPage);
  }

  /// Returns trailing pages that hold no strands to the pool.
  static void ReleaseTailPages(World& world, Source& source)
  {
    while (!source.m_Pages.IsEmpty() && !source.m_FreeRanges.IsEmpty())
    {
      GrassRange& tail = source.m_FreeRanges.PeekBack();
      const plUInt32 uiEnd = source.m_Pages.GetCount() * GRASS_PAGE_SIZE;
      const plUInt32 uiLastPageStart = uiEnd - GRASS_PAGE_SIZE;
      if (tail.m_uiFirst + tail.m_uiCount != uiEnd || tail.m_uiFirst > uiLastPageStart)
        break;

      ReleasePage(world, source.m_Pages.PeekBack());
      source.m_Pages.PopBack();
      tail.m_uiCount -= GRASS_PAGE_SIZE;
      if (tail.m_uiCount == 0)
        source.m_FreeRanges.PopBack();
    }
  }

  static void FreeRange(World& world, Source& source, const GrassRange& range)
  {
    if (range.m_uiCount == 0)
      return;

    ForEachPagePiece(source, range, [&](plUInt32, plUInt32 uiGlobalFirst, plUInt32, plUInt32 uiCount)
      { QueueKill(world, uiGlobalFirst, uiCount); });

    InsertFreeRange(source, range);
    ReleaseTailPages(world, source);
  }

  static bool AllocateRange(World& world, plUInt32 uiSlot, plUInt32 uiCount, GrassRange& out_range)
  {
    Source& source = world.m_Sources[uiSlot];

    for (int iPass = 0; iPass < 2; ++iPass)
    {
      for (plUInt32 i = 0; i < source.m_FreeRanges.GetCount(); ++i)
      {
        GrassRange& free = source.m_FreeRanges[i];
        if (free.m_uiCount < uiCount)
          continue;

        out_range.m_uiFirst = free.m_uiFirst;
        out_range.m_uiCount = uiCount;
        free.m_uiFirst += uiCount;
        free.m_uiCount -= uiCount;
        if (free.m_uiCount == 0)
          source.m_FreeRanges.RemoveAtAndCopy(i);
        return true;
      }

      // Append pages; a free tail range extends into them.
      const plUInt32 uiEnd = source.m_Pages.GetCount() * GRASS_PAGE_SIZE;
      plUInt32 uiTailFree = 0;
      if (!source.m_FreeRanges.IsEmpty() && source.m_FreeRanges.PeekBack().m_uiFirst + source.m_FreeRanges.PeekBack().m_uiCount == uiEnd)
        uiTailFree = source.m_FreeRanges.PeekBack().m_uiCount;

      const plUInt32 uiNeededPages = (uiCount - uiTailFree + GRASS_PAGE_SIZE - 1) / GRASS_PAGE_SIZE;
      for (plUInt32 p = 0; p < uiNeededPages; ++p)
      {
        plUInt32 uiPage = 0;
        if (!AcquirePage(world, uiSlot, uiPage))
        {
          ReleaseTailPages(world, source);
          return false;
        }

        const plUInt32 uiLocalFirst = source.m_Pages.GetCount() * GRASS_PAGE_SIZE;
        source.m_Pages.PushBack(uiPage);
        InsertFreeRange(source, GrassRange{uiLocalFirst, GRASS_PAGE_SIZE});
      }
    }

    return false;
  }

  static void AddChunkBoundsToPages(World& world, const Source& source, const GrassChunk& chunk)
  {
    ForEachPagePiece(source, chunk.m_Range, [&](plUInt32 uiPage, plUInt32, plUInt32, plUInt32)
      {
        Page& page = world.m_Pages[uiPage];
        if (chunk.m_Bounds.IsValid())
        {
          if (page.m_LocalBounds.IsValid())
            page.m_LocalBounds.ExpandToInclude(chunk.m_Bounds);
          else
            page.m_LocalBounds = chunk.m_Bounds;
        }
      });
  }

  /// Frees one tile's strands. Returns its density, 255 when the tile was not resident.
  static plUInt8 RemoveTile(World& world, Source& source, plUInt32 uiTileKey)
  {
    GrassChunk* pChunk = source.m_Tiles.GetValue(uiTileKey);
    if (pChunk == nullptr)
      return 255;

    const GrassChunk chunk = *pChunk;
    source.m_Tiles.Remove(uiTileKey);
    if (chunk.m_uiDensity < 255 && world.m_uiThinnedChunks > 0)
      --world.m_uiThinnedChunks;
    FreeRange(world, source, chunk.m_Range);
    return chunk.m_uiDensity;
  }

  static void ClearSourceStrands(World& world, Source& source)
  {
    for (auto it = source.m_Tiles.GetIterator(); it.IsValid(); ++it)
    {
      if (it.Value().m_uiDensity < 255 && world.m_uiThinnedChunks > 0)
        --world.m_uiThinnedChunks;
      FreeRange(world, source, it.Value().m_Range);
    }
    source.m_Tiles.Clear();

    if (source.m_bHasRoots)
    {
      FreeRange(world, source, source.m_Roots.m_Range);
      source.m_Roots = GrassChunk();
      source.m_bHasRoots = false;
    }

    // Anything left (fragmented free space) goes back as whole pages.
    for (plUInt32 uiPage : source.m_Pages)
      ReleasePage(world, uiPage);
    source.m_Pages.Clear();
    source.m_FreeRanges.Clear();
  }

  void DestroyGpuResources(World& world)
  {
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    for (plGALBufferHandle* pHandle : {&world.m_hStatic, &world.m_hState, &world.m_hPages, &world.m_hSources, &world.m_hAtlas, &world.m_hResident, &world.m_hActive, &world.m_hUpload})
    {
      if (!pHandle->IsInvalidated())
        pDevice->DestroyBuffer(*pHandle);
    }
    world.m_uiGpuPages = 0;
    world.m_uiGpuStateUnits = 0;
    world.m_uiGpuSources = 0;
    world.m_uiGpuAtlas = 0;
    world.m_uiGpuUploadUints = 0;
    world.m_Snapshot = plGrassRenderSnapshot();
  }

  void ProcessWorld(plUInt32 uiWorldIndex, World& world);
  void DeclareWorldPasses(plRenderGraph& ref_graph, FrameWork& work);
  static void ExecuteUpload(const plRenderGraphContext& ctx, const FrameWork& work);
  static void ExecuteGenerate(const plRenderGraphContext& ctx, const FrameWork& work, plUInt32 uiWave);
  static void ExecuteSimulate(const plRenderGraphContext& ctx, const FrameWork& work);
};

plGrassGpuManager::Data* plGrassGpuManager::s_pData = nullptr;

namespace
{
  plGALBufferHandle GrassCreateBuffer(plUInt32 uiStructSize, plUInt32 uiElements, bool bUav, bool bRaw, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = bRaw ? 0 : uiStructSize;
    desc.m_uiTotalSize = plMath::Max(uiStructSize * uiElements, 16u);
    desc.m_BufferFlags = (bRaw ? plGALBufferUsageFlags::ByteAddressBuffer : plGALBufferUsageFlags::StructuredBuffer) | plGALBufferUsageFlags::ShaderResource;
    if (bUav)
      desc.m_BufferFlags |= plGALBufferUsageFlags::UnorderedAccess;
    desc.m_ResourceAccess.m_bImmutable = false;

    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc);
    if (const plGALBuffer* pBuffer = pDevice->GetBuffer(hBuffer))
      pBuffer->SetDebugName(szName);
    return hBuffer;
  }

  // Replaces a pool buffer with a larger one. The old buffer is destroyed through the device, which keeps it alive until
  // its end of frame, so this frame's upload pass can still copy the old content (out_hOld, out_uiCopyBytes) into the
  // replacement. The new tail stays undefined until written.
  void GrassGrowPoolBuffer(plGALBufferHandle& inout_hBuffer, plUInt32 uiOldBytes, plGALBufferHandle hNew, plGALBufferHandle& out_hOld, plUInt32& out_uiCopyBytes)
  {
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    out_hOld.Invalidate();
    out_uiCopyBytes = 0;

    if (!inout_hBuffer.IsInvalidated())
    {
      if (uiOldBytes > 0 && pDevice->GetBuffer(hNew) != nullptr)
      {
        out_hOld = inout_hBuffer;
        out_uiCopyBytes = uiOldBytes;
      }

      plGALBufferHandle hDestroy = inout_hBuffer;
      pDevice->DestroyBuffer(hDestroy);
    }
    inout_hBuffer = hNew;
  }

  // Orders the commands into waves: a command whose strand range overlaps an earlier command's runs in a later wave, so
  // a kill and a regrow of the same strands keep their queue order, while the commands of one wave write disjoint
  // strands. Each wave is one graph pass, so the graph's write-after-write barrier separates the waves. Commands without
  // work are dropped. out_waveStarts receives the first command of each wave, then the command count.
  void GrassSortIntoWaves(plDynamicArray<GrassCommand>& inout_commands, plUInt32 uiPoolStrands, plDynamicArray<plUInt32>& out_waveStarts)
  {
    plDynamicArray<GrassCommand> commands;
    commands.Reserve(inout_commands.GetCount());
    for (const GrassCommand& cmd : inout_commands)
    {
      if (cmd.m_uiCount != 0 && cmd.m_uiGlobalFirst < uiPoolStrands)
        commands.PushBack(cmd);
    }

    struct Entry
    {
      plUInt32 m_uiFirst = 0;
      plUInt32 m_uiEnd = 0;
      plUInt32 m_uiWave = 0;
      plUInt32 m_uiPrevious = plInvalidIndex; ///< previous entry on the same page
    };

    // Commands are split per page, so the overlap search only walks the earlier commands of the same page.
    plDynamicArray<Entry> entries;
    plHashTable<plUInt32, plUInt32> pageLast;
    plDynamicArray<plUInt32> waves;
    waves.SetCount(commands.GetCount());
    plUInt32 uiWaveCount = 0;

    for (plUInt32 i = 0; i < commands.GetCount(); ++i)
    {
      const GrassCommand& cmd = commands[i];
      const plUInt32 uiFirst = cmd.m_uiGlobalFirst;
      const plUInt32 uiEnd = uiFirst + plMath::Min(cmd.m_uiCount, uiPoolStrands - uiFirst);
      const plUInt32 uiFirstPage = uiFirst / GRASS_PAGE_SIZE;
      const plUInt32 uiLastPage = (uiEnd - 1) / GRASS_PAGE_SIZE;

      plUInt32 uiWave = 0;
      for (plUInt32 p = uiFirstPage; p <= uiLastPage; ++p)
      {
        plUInt32 uiEntry = plInvalidIndex;
        pageLast.TryGetValue(p, uiEntry);
        for (; uiEntry != plInvalidIndex; uiEntry = entries[uiEntry].m_uiPrevious)
        {
          if (entries[uiEntry].m_uiFirst < uiEnd && uiFirst < entries[uiEntry].m_uiEnd)
            uiWave = plMath::Max(uiWave, entries[uiEntry].m_uiWave + 1);
        }
      }

      for (plUInt32 p = uiFirstPage; p <= uiLastPage; ++p)
      {
        plUInt32 uiPrevious = plInvalidIndex;
        pageLast.TryGetValue(p, uiPrevious);
        Entry& entry = entries.ExpandAndGetRef();
        entry.m_uiFirst = uiFirst;
        entry.m_uiEnd = uiEnd;
        entry.m_uiWave = uiWave;
        entry.m_uiPrevious = uiPrevious;
        pageLast[p] = entries.GetCount() - 1;
      }

      waves[i] = uiWave;
      uiWaveCount = plMath::Max(uiWaveCount, uiWave + 1);
    }

    // Stable counting sort by wave.
    out_waveStarts.Clear();
    out_waveStarts.SetCount(uiWaveCount + 1, 0);
    for (plUInt32 uiWave : waves)
      ++out_waveStarts[uiWave + 1];
    for (plUInt32 w = 0; w < uiWaveCount; ++w)
      out_waveStarts[w + 1] += out_waveStarts[w];

    plDynamicArray<plUInt32> next;
    next.SetCount(uiWaveCount);
    for (plUInt32 w = 0; w < uiWaveCount; ++w)
      next[w] = out_waveStarts[w];

    inout_commands.Clear();
    inout_commands.SetCount(commands.GetCount());
    for (plUInt32 i = 0; i < commands.GetCount(); ++i)
      inout_commands[next[waves[i]]++] = commands[i];
  }
} // namespace

void plGrassGpuManager::Data::ProcessWorld(plUInt32 uiWorldIndex, World& world)
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();

  // Recycle source slots once no frame in flight can still reference them.
  for (plUInt32 i = world.m_PendingSlots.GetCount(); i > 0; --i)
  {
    if (uiFrame >= world.m_PendingSlots[i - 1].m_uiFrame + 4)
    {
      world.m_FreeSlots.PushBack(world.m_PendingSlots[i - 1].m_uiSlot);
      world.m_PendingSlots.RemoveAtAndSwap(i - 1);
    }
  }

  const plUInt32 uiPageCount = world.m_Pages.GetCount();
  const plUInt32 uiSourceCount = world.m_Sources.GetCount();

  if (uiPageCount == 0 || uiSourceCount == 0)
  {
    world.m_Commands.Clear();
    world.m_Upload.Clear();
    world.m_Snapshot = plGrassRenderSnapshot();
    world.m_uiResidentStrands = 0;
    world.m_uiSimulatedStrands = 0;
    world.m_uiUsedPages = 0;
    world.m_uiUploadBytes = 0;
    return;
  }

  // Fixed 60 Hz steps, at most four per frame. A world that stopped submitting (paused document) does not simulate,
  // nor does any world while Vegetation.Grass.Simulate is off.
  plUInt32 uiSteps = 0;
  const bool bSubmitted = uiFrame <= world.m_uiSubmitFrame + 2;
  const bool bSimulateWorld = bSubmitted && world.m_bSimulate && world.m_bHasCamera && cvar_VegetationGrassSimulate;
  if (bSimulateWorld)
  {
    world.m_fAccumulatedTime = plMath::Min(world.m_fAccumulatedTime + world.m_DeltaTime.AsFloatInSeconds(), s_fGrassStepTime * 4.0f);
    uiSteps = static_cast<plUInt32>(world.m_fAccumulatedTime / s_fGrassStepTime);
    world.m_fAccumulatedTime -= uiSteps * s_fGrassStepTime;
  }
  else
  {
    world.m_fAccumulatedTime = 0.0f;
  }
  world.m_DeltaTime = plTime::MakeZero();

  // CPU tables
  plDynamicArray<plGrassSourceGpu> sources;
  sources.SetCount(uiSourceCount);
  plDynamicArray<plGrassAtlasGpu> atlas;
  plDynamicArray<plGrassPageGpu> pages;
  pages.SetCount(uiPageCount);
  plDynamicArray<plUInt32> resident;
  plDynamicArray<plGrassPageCullInfo> residentInfo;
  plDynamicArray<plUInt32> active;

  plUInt32 uiVisibleBase = 0;
  for (plUInt32 uiSlot = 0; uiSlot < uiSourceCount; ++uiSlot)
  {
    Source& source = world.m_Sources[uiSlot];
    plGrassSourceGpu& gpu = sources[uiSlot];
    plMemoryUtils::ZeroFill(&gpu, 1);
    if (!source.m_bUsed)
      continue;

    const plGrassSettings& s = source.m_Settings;
    if (!source.m_bHasLastTransform)
    {
      source.m_LastUploadedTransform = source.m_Transform;
      source.m_bHasLastTransform = true;
    }

    gpu.ObjectToWorld = source.m_Transform;
    gpu.LastObjectToWorld = source.m_LastUploadedTransform;
    source.m_LastUploadedTransform = source.m_Transform;

    const plVec3 vScale = source.m_Transform.m_vScale.Abs();
    gpu.Shape = plVec4(s.m_fLength, s.m_fWidth, source.m_fTextureAspect, s.m_fRandomness);
    gpu.Sim = plVec4(s.m_fStiffness, s.m_fDrag, s.m_fGravityPower, s.m_fWindInfluence);
    gpu.Touch = plVec4(s.m_fTouchCrush, s.m_fTouchRecovery, plMath::Max(vScale.x, vScale.y, vScale.z), s.m_fUniformity);
    // The GPU cull applies the same scaled shadow distance as the extraction does.
    gpu.Distances = plVec4(s.m_fViewDistance, s.m_fSimulationDistance, s.m_fLodStartDistance, s.m_fShadowDistance * GetShadowDistanceScale());
    gpu.Blade = plVec4(s.m_fBladeWidth, s.m_fBladeTilt, s.m_fBladeBend, s.m_fBladeSideCurve);
    gpu.Clump = plVec4(s.m_fClumpSize, s.m_fClumpStrength, s.m_fClumpColorVariation, s.m_fBladeNormalRoundness);
    gpu.Detail = plVec4(s.m_fFarDensity, s.m_fStrandsPerSquareMeter, s.m_fGroundCoverStartDistance, s.m_bFarGroundCover ? s.m_fGroundCoverMaxSize : 0.0f);
    gpu.MainCamera = plVec4(world.m_vMainCamera.x, world.m_vMainCamera.y, world.m_vMainCamera.z, 0);

    const plUInt32 uiAtlasStart = atlas.GetCount();
    const plUInt32 uiAtlasCount = plMath::Min<plUInt32>(s.m_AtlasRects.GetCount(), 255u);
    for (plUInt32 a = 0; a < uiAtlasCount; ++a)
    {
      const plGrassAtlasRect& rect = s.m_AtlasRects[a];
      plGrassAtlasGpu& entry = atlas.ExpandAndGetRef();
      plMemoryUtils::ZeroFill(&entry, 1);
      entry.TexMulAdd = rect.m_vTexMulAdd;
      entry.Size = rect.m_fSize;
      entry.Aspect = plMath::Abs(rect.m_vTexMulAdd.y) > 0.00001f ? plMath::Abs(rect.m_vTexMulAdd.x / rect.m_vTexMulAdd.y) : 1.0f;
    }

    gpu.Counts = plVec4U32(GrassSegments(s), s.m_bProceduralBlades ? 1u : plMath::Clamp<plUInt32>(s.m_uiBillboardCount, 1u, GRASS_MAX_BILLBOARDS), uiAtlasStart, uiAtlasCount);

    const plUInt32 uiCapacity = source.m_Pages.GetCount() * GRASS_PAGE_SIZE;
    plUInt32 uiFlags = s.m_bCameraBend ? GRASS_SOURCE_FLAG_CAMERA_BEND : 0u;
    if (s.m_bProceduralBlades)
      uiFlags |= GRASS_SOURCE_FLAG_BLADES;
    if (s.m_bProceduralBlades && s.m_bAdaptiveDensity)
      uiFlags |= GRASS_SOURCE_FLAG_ADAPTIVE;
    if (uiSteps > 0)
      uiFlags |= GRASS_SOURCE_FLAG_STEPPED;
    gpu.Layout = plVec4U32(uiVisibleBase, uiCapacity, uiFlags, s.m_uiRandomSeed);
    uiVisibleBase += 2 * uiCapacity;
  }

  // Cull-side density of each page: the largest density of the chunks in it, so a page shared with an unthinned tile
  // keeps all its strands.
  plDynamicArray<plUInt8> pageDensity;
  if (world.m_uiThinnedChunks > 0)
  {
    pageDensity.SetCount(uiPageCount, 0);
    auto markChunk = [&](const Source& source, const GrassChunk& chunk)
    {
      ForEachPagePiece(source, chunk.m_Range, [&](plUInt32 uiPage, plUInt32, plUInt32, plUInt32)
        {
          if (uiPage < uiPageCount)
            pageDensity[uiPage] = plMath::Max(pageDensity[uiPage], chunk.m_uiDensity);
        });
    };

    for (const Source& source : world.m_Sources)
    {
      if (!source.m_bUsed)
        continue;
      for (auto it = source.m_Tiles.GetIterator(); it.IsValid(); ++it)
        markChunk(source, it.Value());
      if (source.m_bHasRoots)
        markChunk(source, source.m_Roots);
    }
  }

  world.m_uiResidentStrands = 0;
  world.m_uiSimulatedStrands = 0;
  world.m_uiUsedPages = 0;
  world.m_uiUploadBytes = world.m_Upload.GetCount() * sizeof(plUInt32);
  for (plUInt32 p = 0; p < uiPageCount; ++p)
  {
    Page& page = world.m_Pages[p];
    plGrassPageGpu& gpu = pages[p];
    plMemoryUtils::ZeroFill(&gpu, 1);
    gpu.SourceIndex = s_uiGrassInvalid;

    if (page.m_uiSource == s_uiGrassInvalid || page.m_uiSource >= uiSourceCount)
      continue;

    const Source& source = world.m_Sources[page.m_uiSource];
    gpu.SourceIndex = page.m_uiSource;
    gpu.StrandCount = GRASS_PAGE_SIZE;
    const plUInt32 uiDensity = (pageDensity.IsEmpty() || pageDensity[p] == 0) ? 255u : pageDensity[p];
    gpu.Flags = uiDensity << GRASS_PAGE_DENSITY_SHIFT;
    resident.PushBack(p);
    world.m_uiResidentStrands += GRASS_PAGE_SIZE;
    ++world.m_uiUsedPages;

    // Per-view prefilter box: the root bounds grown by the largest sphere the GPU cull tests for a strand of this
    // source (longest atlas rect, full randomness, card width at the largest object scale).
    {
      plGrassPageCullInfo& info = residentInfo.ExpandAndGetRef();
      info.m_uiPage = p;
      info.m_uiSource = page.m_uiSource;
      info.m_fViewDistance = source.m_Settings.m_fViewDistance;
      info.m_fShadowDistance = source.m_Settings.m_fShadowDistance * GetShadowDistanceScale();
      info.m_bCastShadows = source.m_Settings.m_bCastShadows;
      if (page.m_LocalBounds.IsValid())
      {
        const plVec3 vScale = source.m_Transform.m_vScale.Abs();
        const float fMaxScale = plMath::Max(vScale.x, vScale.y, vScale.z);
        float fAtlasSize = 1.0f;
        for (const plGrassAtlasRect& rect : source.m_Settings.m_AtlasRects)
          fAtlasSize = plMath::Max(fAtlasSize, plMath::Clamp(rect.m_fSize, 0.0f, 10.0f));
        const float fLength = source.m_Settings.m_fLength * fAtlasSize * (1.0f + source.m_Settings.m_fRandomness) * fMaxScale;
        const float fMargin = source.m_Settings.m_bProceduralBlades ? fLength * (3.0f + source.m_Settings.m_fWidth * source.m_Settings.m_fBladeWidth * 3.0f) + source.m_Settings.m_fGroundCoverMaxSize : fLength * (1.8f + source.m_Settings.m_fWidth * source.m_fTextureAspect * 4.0f * fMaxScale * 2.0f);

        plBoundingBox worldBounds = page.m_LocalBounds;
        worldBounds.TransformFromOrigin(source.m_Transform.GetAsMat4());
        worldBounds.Grow(plVec3(fMargin));
        info.m_WorldBounds = worldBounds;
      }
    }

    bool bSimulated = false;
    if (bSimulateWorld && source.m_Settings.m_fSimulationDistance > 0.0f && page.m_LocalBounds.IsValid())
    {
      plBoundingBox worldBounds = page.m_LocalBounds;
      const float fMargin = source.m_Settings.m_fLength * 2.0f;
      worldBounds.Grow(plVec3(fMargin));
      worldBounds.TransformFromOrigin(source.m_Transform.GetAsMat4());
      bSimulated = worldBounds.GetDistanceTo(world.m_vMainCamera) < source.m_Settings.m_fSimulationDistance;
    }

    if (bSimulated && page.m_uiStateUnitCount == 0)
    {
      const plUInt32 uiSegments = GrassSegments(source.m_Settings);
      if (uiSteps == 0 || !AllocateStateUnits(world, uiSegments, page.m_uiStateUnit))
        bSimulated = false;
      else
        page.m_uiStateUnitCount = uiSegments;
    }
    if (!bSimulated && page.m_uiStateUnitCount > 0)
    {
      for (plUInt32 u = 0; u < page.m_uiStateUnitCount; ++u)
        world.m_StateUnits[page.m_uiStateUnit + u] = false;
      page.m_uiStateUnitCount = 0;
    }
    gpu.StateBase = page.m_uiStateUnitCount > 0 ? page.m_uiStateUnit * GRASS_PAGE_SIZE : s_uiGrassInvalid;

    if (bSimulated && !page.m_bSimulated)
      page.m_bNeedsWake = true;
    page.m_bSimulated = bSimulated;

    if (bSimulated)
    {
      gpu.Flags |= GRASS_PAGE_FLAG_SIMULATED;
      if (page.m_bNeedsWake && uiSteps > 0)
      {
        gpu.Flags |= GRASS_PAGE_FLAG_WAKE;
        page.m_bNeedsWake = false;
      }
      active.PushBack(p);
      world.m_uiSimulatedStrands += GRASS_PAGE_SIZE;
    }
  }

  // Free trailing state capacity in large steps, avoiding a GPU reallocation on every page boundary.
  plUInt32 uiStateTail = world.m_StateUnits.GetCount();
  while (uiStateTail > 0 && !world.m_StateUnits[uiStateTail - 1])
    --uiStateTail;
  if (uiStateTail * 2 < world.m_StateUnits.GetCount())
    world.m_StateUnits.SetCount(uiStateTail);
  const plUInt32 uiStateUnits = world.m_StateUnits.GetCount();

  // Buffers are (re)created here; everything the GPU does with them is recorded by this frame's grass PreRender graph
  // (DeclareWorldPasses), which runs before every view's graph of the frame. Pool buffers keep their content when they
  // grow; the tables are rewritten every frame.
  plUniquePtr<FrameWork> pWork = PL_DEFAULT_NEW(FrameWork);
  FrameWork& work = *pWork;

  if (world.m_uiGpuPages != uiPageCount || world.m_hStatic.IsInvalidated())
  {
    const plUInt32 uiOldBytes = world.m_uiGpuPages * GRASS_PAGE_SIZE * sizeof(plGrassStrandStatic);
    GrassGrowPoolBuffer(world.m_hStatic, plMath::Min(uiOldBytes, uiPageCount * GRASS_PAGE_SIZE * (plUInt32)sizeof(plGrassStrandStatic)),
      GrassCreateBuffer(sizeof(plGrassStrandStatic), uiPageCount * GRASS_PAGE_SIZE, true, false, "Grass - Strand Static"), work.m_hOldStatic, work.m_uiOldStaticBytes);

    for (plGALBufferHandle* pHandle : {&world.m_hPages, &world.m_hResident, &world.m_hActive})
    {
      if (!pHandle->IsInvalidated())
        pDevice->DestroyBuffer(*pHandle);
    }
    world.m_hPages = GrassCreateBuffer(sizeof(plGrassPageGpu), uiPageCount, false, false, "Grass - Pages");
    world.m_hResident = GrassCreateBuffer(sizeof(plUInt32), uiPageCount, false, false, "Grass - Resident Pages");
    world.m_hActive = GrassCreateBuffer(sizeof(plUInt32), uiPageCount, false, false, "Grass - Active Pages");
    world.m_uiGpuPages = uiPageCount;
  }

  if (world.m_uiGpuStateUnits != uiStateUnits || world.m_hState.IsInvalidated())
  {
    const plUInt32 uiUnitBytes = GRASS_PAGE_SIZE * GRASS_STATE_SEGMENT_BYTES;
    const plUInt32 uiOldBytes = plMath::Min(world.m_uiGpuStateUnits, uiStateUnits) * uiUnitBytes;
    GrassGrowPoolBuffer(world.m_hState, uiOldBytes, GrassCreateBuffer(uiUnitBytes, plMath::Max(uiStateUnits, 1u), true, true, "Grass - Strand State"), work.m_hOldState, work.m_uiOldStateBytes);
    world.m_uiGpuStateUnits = uiStateUnits;
  }

  if (world.m_uiGpuSources < uiSourceCount || world.m_hSources.IsInvalidated())
  {
    if (!world.m_hSources.IsInvalidated())
      pDevice->DestroyBuffer(world.m_hSources);
    world.m_uiGpuSources = plMath::Max(uiSourceCount, 8u);
    world.m_hSources = GrassCreateBuffer(sizeof(plGrassSourceGpu), world.m_uiGpuSources, false, false, "Grass - Sources");
  }

  if (world.m_uiGpuAtlas < plMath::Max(atlas.GetCount(), 1u) || world.m_hAtlas.IsInvalidated())
  {
    if (!world.m_hAtlas.IsInvalidated())
      pDevice->DestroyBuffer(world.m_hAtlas);
    world.m_uiGpuAtlas = plMath::Max(atlas.GetCount(), 16u);
    world.m_hAtlas = GrassCreateBuffer(sizeof(plGrassAtlasGpu), world.m_uiGpuAtlas, false, false, "Grass - Atlas");
  }

  if (world.m_uiGpuUploadUints < world.m_Upload.GetCount() || world.m_hUpload.IsInvalidated())
  {
    if (!world.m_hUpload.IsInvalidated())
      pDevice->DestroyBuffer(world.m_hUpload);
    world.m_uiGpuUploadUints = plMath::Max(world.m_Upload.GetCount(), 1024u);
    world.m_hUpload = GrassCreateBuffer(sizeof(plUInt32), world.m_uiGpuUploadUints, false, true, "Grass - Upload");
  }

  work.m_uiWorldIndex = uiWorldIndex;
  work.m_hStatic = world.m_hStatic;
  work.m_hState = world.m_hState;
  work.m_hPages = world.m_hPages;
  work.m_hSources = world.m_hSources;
  work.m_hAtlas = world.m_hAtlas;
  work.m_hResident = world.m_hResident;
  work.m_hActive = world.m_hActive;
  work.m_hUpload = world.m_hUpload;
  work.m_uiPageCount = uiPageCount;
  work.m_uiPoolStrands = uiPageCount * GRASS_PAGE_SIZE;
  work.m_uiStateSegments = uiStateUnits * GRASS_PAGE_SIZE;
  work.m_uiSourceCount = uiSourceCount;
  work.m_uiSteps = active.IsEmpty() ? 0 : uiSteps;
  work.m_Pages = std::move(pages);
  work.m_Sources = std::move(sources);
  work.m_Atlas = atlas;
  work.m_Resident = resident;
  work.m_Active = std::move(active);
  work.m_Upload = std::move(world.m_Upload);
  work.m_Commands = std::move(world.m_Commands);
  GrassSortIntoWaves(work.m_Commands, work.m_uiPoolStrands, work.m_WaveStarts);
  m_FrameWork.PushBack(std::move(pWork));

  world.m_Commands.Clear();
  world.m_Upload.Clear();

  plGrassRenderSnapshot& snapshot = world.m_Snapshot;
  snapshot.m_hStatic = world.m_hStatic;
  snapshot.m_hState = world.m_hState;
  snapshot.m_hPages = world.m_hPages;
  snapshot.m_hSources = world.m_hSources;
  snapshot.m_hAtlas = world.m_hAtlas;
  snapshot.m_hResidentPages = world.m_hResident;
  snapshot.m_uiPageCount = uiPageCount;
  snapshot.m_uiPoolStrands = uiPageCount * GRASS_PAGE_SIZE;
  snapshot.m_uiStateSegments = uiStateUnits * GRASS_PAGE_SIZE;
  snapshot.m_uiSourceCount = uiSourceCount;
  snapshot.m_uiAtlasCount = atlas.GetCount();
  snapshot.m_uiResidentPageCount = resident.GetCount();
  snapshot.m_ResidentPages = std::move(residentInfo);
  snapshot.m_uiVisibleEntries = uiVisibleBase;
  snapshot.m_vMainCamera = world.m_vMainCamera;
  snapshot.m_uiFrame = uiFrame;
  snapshot.m_bValid = true;
}

void plGrassGpuManager::Data::DeclareWorldPasses(plRenderGraph& ref_graph, FrameWork& work)
{
  // Every GPU access of the pool is declared here, so the graph's state tracker places all barriers: within this graph,
  // and against the per-view cull and draw passes of the same frame, which read the pool in later graphs.
  const plRenderGraphBufferHandle hStatic = ref_graph.ImportBuffer(work.m_hStatic);
  const plRenderGraphBufferHandle hState = ref_graph.ImportBuffer(work.m_hState);
  const plRenderGraphBufferHandle hPages = ref_graph.ImportBuffer(work.m_hPages);
  const plRenderGraphBufferHandle hSources = ref_graph.ImportBuffer(work.m_hSources);
  FrameWork* pWork = &work;

  // Table uploads and the copies into grown pool buffers. The tables are declared by no pass of this one, so they sit in
  // their resting state, which is the state an AheadOfTime copy transitions from and back to.
  {
    plRenderGraphPassBuilder pass = ref_graph.AddTransferPass("Grass Upload");
    pass.HasSideEffects();
    if (!work.m_hOldStatic.IsInvalidated())
    {
      pass.ReadBuffer(ref_graph.ImportBuffer(work.m_hOldStatic), plGALResourceState::CopySource);
      pass.WriteBuffer(hStatic, plGALResourceState::CopyDestination);
    }
    if (!work.m_hOldState.IsInvalidated())
    {
      pass.ReadBuffer(ref_graph.ImportBuffer(work.m_hOldState), plGALResourceState::CopySource);
      pass.WriteBuffer(hState, plGALResourceState::CopyDestination);
    }
    pass.SetExecuteCallback([pWork](const plRenderGraphContext& ctx) { ExecuteUpload(ctx, *pWork); });
  }

  if (!work.m_Commands.IsEmpty())
  {
    const plRenderGraphBufferHandle hAtlas = ref_graph.ImportBuffer(work.m_hAtlas);
    const plRenderGraphBufferHandle hUpload = ref_graph.ImportBuffer(work.m_hUpload);

    // One pass per wave; the write-after-write barrier between the passes orders overlapping commands.
    for (plUInt32 uiWave = 0; uiWave + 1 < work.m_WaveStarts.GetCount(); ++uiWave)
    {
      plRenderGraphPassBuilder pass = ref_graph.AddComputePass("Grass Generate");
      pass.HasSideEffects();
      pass.WriteBuffer(hStatic);
      pass.WriteBuffer(hState);
      pass.ReadBuffer(hSources, plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(hPages, plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(hAtlas, plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(hUpload, plGALResourceState::ComputeShaderRead);
      pass.SetExecuteCallback([pWork, uiWave](const plRenderGraphContext& ctx) { ExecuteGenerate(ctx, *pWork, uiWave); });
    }
  }

  if (work.m_uiSteps > 0)
  {
    plRenderGraphPassBuilder pass = ref_graph.AddComputePass("Grass Simulate");
    pass.HasSideEffects();
    pass.WriteBuffer(hState);
    pass.ReadBuffer(hStatic, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(hSources, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(hPages, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(ref_graph.ImportBuffer(work.m_hActive), plGALResourceState::ComputeShaderRead);
    pass.SetExecuteCallback([pWork](const plRenderGraphContext& ctx) { ExecuteSimulate(ctx, *pWork); });
  }
}

void plGrassGpuManager::Data::ExecuteUpload(const plRenderGraphContext& ctx, const FrameWork& work)
{
  plGALCommandEncoder* pEncoder = ctx.GetCommandEncoder();
  PL_PROFILE_AND_MARKER(pEncoder, "Grass Upload");

  if (!work.m_hOldStatic.IsInvalidated())
    pEncoder->CopyBufferRegion(work.m_hStatic, 0, work.m_hOldStatic, 0, work.m_uiOldStaticBytes);
  if (!work.m_hOldState.IsInvalidated())
    pEncoder->CopyBufferRegion(work.m_hState, 0, work.m_hOldState, 0, work.m_uiOldStateBytes);

  pEncoder->UpdateBuffer(work.m_hPages, 0, work.m_Pages.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
  pEncoder->UpdateBuffer(work.m_hSources, 0, work.m_Sources.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
  if (!work.m_Atlas.IsEmpty())
    pEncoder->UpdateBuffer(work.m_hAtlas, 0, work.m_Atlas.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
  if (!work.m_Resident.IsEmpty())
    pEncoder->UpdateBuffer(work.m_hResident, 0, work.m_Resident.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
  if (!work.m_Active.IsEmpty())
    pEncoder->UpdateBuffer(work.m_hActive, 0, work.m_Active.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
  if (!work.m_Upload.IsEmpty())
    pEncoder->UpdateBuffer(work.m_hUpload, 0, work.m_Upload.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
}

void plGrassGpuManager::Data::ExecuteGenerate(const plRenderGraphContext& ctx, const FrameWork& work, plUInt32 uiWave)
{
  if (s_pData == nullptr || uiWave + 1 >= work.m_WaveStarts.GetCount())
    return;

  plRenderContext* pRenderContext = ctx.GetRenderContext();
  PL_PROFILE_AND_MARKER(ctx.GetCommandEncoder(), "Grass Generate");

  for (plUInt32 i = work.m_WaveStarts[uiWave]; i < work.m_WaveStarts[uiWave + 1]; ++i)
  {
    const GrassCommand& cmd = work.m_Commands[i];

    plGrassGenerateConstants constants;
    plMemoryUtils::ZeroFill(&constants, 1);
    constants.GenMode = cmd.m_Type == GrassCommandType::Kill ? GRASS_GENERATE_KILL : (cmd.m_Type == GrassCommandType::Coverage ? GRASS_GENERATE_COVERAGE : GRASS_GENERATE_ROOTS);
    constants.GenSourceIndex = cmd.m_uiSourceSlot;
    constants.GenGlobalFirst = cmd.m_uiGlobalFirst;
    constants.GenCount = plMath::Min(cmd.m_uiCount, work.m_uiPoolStrands - cmd.m_uiGlobalFirst);
    constants.GenChunkFirst = cmd.m_uiChunkFirst;
    constants.GenUploadOffset = cmd.m_uiUploadOffset;
    constants.GenUploadCount = work.m_Upload.GetCount();
    constants.GenPoolStrands = work.m_uiPoolStrands;
    constants.GenTileX = cmd.m_iTileX;
    constants.GenTileY = cmd.m_iTileY;
    constants.GenTypeSeed = cmd.m_uiTypeSeed;
    constants.GenSampleStride = cmd.m_uiSampleStride;
    constants.GenTexelSize = cmd.m_fTexelSize;
    constants.GenBaseHeight = cmd.m_fBaseHeight;
    constants.GenStateSegments = work.m_uiStateSegments;
    constants.GenSourceCount = work.m_uiSourceCount;

    plBindGroupBuilder& bindGroup = pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("GrassStaticOut", work.m_hStatic);
    bindGroup.BindBuffer("GrassStateOut", work.m_hState);
    bindGroup.BindBuffer("GrassSources", work.m_hSources);
    bindGroup.BindBuffer("GrassPages", work.m_hPages);
    bindGroup.BindBuffer("GrassAtlas", work.m_hAtlas);
    bindGroup.BindBuffer("GrassUpload", work.m_hUpload);
    pRenderContext->SetPushConstants("plGrassGenerateConstants", constants);
    pRenderContext->BindShader(s_pData->m_hGenerateShader);
    pRenderContext->Dispatch((constants.GenCount + GRASS_THREAD_GROUP_SIZE - 1) / GRASS_THREAD_GROUP_SIZE, 1, 1).IgnoreResult();
  }
}

void plGrassGpuManager::Data::ExecuteSimulate(const plRenderGraphContext& ctx, const FrameWork& work)
{
  if (s_pData == nullptr || work.m_Active.IsEmpty())
    return;

  plRenderContext* pRenderContext = ctx.GetRenderContext();
  PL_PROFILE_AND_MARKER(ctx.GetCommandEncoder(), "Grass Simulate");

  plGrassSimulateConstants constants;
  plMemoryUtils::ZeroFill(&constants, 1);
  constants.SimActivePageCount = work.m_Active.GetCount();
  constants.SimPageCount = work.m_uiPageCount;
  constants.SimSourceCount = work.m_uiSourceCount;
  constants.SimStateSegments = work.m_uiStateSegments;
  constants.SimSteps = work.m_uiSteps;
  constants.SimStepTime = s_fGrassStepTime;
  constants.SimPoolStrands = work.m_uiPoolStrands;

  // The wind field and touch map of this frame were updated at BeginRender, before this graph was enqueued.
  plWindFieldManager::BindForWorld(pRenderContext, work.m_uiWorldIndex);
  plVegetationTouchMapManager::BindForWorld(pRenderContext, work.m_uiWorldIndex);

  plBindGroupBuilder& bindGroup = pRenderContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
  bindGroup.BindBuffer("GrassStateOut", work.m_hState);
  bindGroup.BindBuffer("GrassStatic", work.m_hStatic);
  bindGroup.BindBuffer("GrassSources", work.m_hSources);
  bindGroup.BindBuffer("GrassPages", work.m_hPages);
  bindGroup.BindBuffer("GrassActivePages", work.m_hActive);
  pRenderContext->SetPushConstants("plGrassSimulateConstants", constants);
  pRenderContext->BindShader(s_pData->m_hSimulateShader);
  pRenderContext->Dispatch(GRASS_GROUPS_PER_PAGE, work.m_Active.GetCount(), 1).IgnoreResult();
}

//////////////////////////////////////////////////////////////////////////

plGrassAOMode plGrassGpuManager::GetAOMode()
{
  return static_cast<plGrassAOMode>(plMath::Clamp(plInt32(cvar_VegetationGrassAOMode), 0, 2));
}

bool plGrassGpuManager::IsEnabled()
{
  return cvar_VegetationGrassEnable;
}

bool plGrassGpuManager::AreShadowsEnabled()
{
  // Follow mode drops grass shadows on the lowest shadow quality tier.
  if (cvar_VegetationGrassShadows.GetValue() < 0)
    return cvar_RenderingShadowsQuality.GetValue() >= 1;

  return cvar_VegetationGrassShadows.GetValue() != 0;
}

float plGrassGpuManager::GetShadowDistanceScale()
{
  // Follow mode halves the shadow distance on the medium tier; a forced on or off state uses the type's distance.
  float fTierScale = 1.0f;
  if (cvar_VegetationGrassShadows.GetValue() < 0 && cvar_RenderingShadowsQuality.GetValue() == 1)
    fTierScale = 0.5f;

  return fTierScale * plMath::Clamp(cvar_VegetationGrassShadowDistanceScale.GetValue(), 0.0f, 1.0f);
}

void plGrassGpuManager::OnStartup()
{
  s_pData = PL_DEFAULT_NEW(Data);
  s_pData->m_hGenerateShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/GrassGenerate.plShader");
  s_pData->m_hSimulateShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/GrassSimulate.plShader");

  // Registered after RendererCore's wind field and touch map managers, so this world's fields are current when the
  // simulation samples them.
  plRenderWorld::GetRenderEvent().AddEventHandler(OnRenderEvent);
}

void plGrassGpuManager::OnShutdown()
{
  if (s_pData == nullptr)
    return;

  plRenderWorld::GetRenderEvent().RemoveEventHandler(OnRenderEvent);

  {
    PL_LOCK(s_pData->m_Mutex);
    for (auto it = s_pData->m_Worlds.GetIterator(); it.IsValid(); ++it)
    {
      s_pData->DestroyGpuResources(*it.Value());
      PL_DEFAULT_DELETE(it.Value());
    }
    s_pData->m_Worlds.Clear();
    s_pData->m_FrameWork.Clear();
    s_pData->m_pGraph = nullptr;
  }

  PL_DEFAULT_DELETE(s_pData);
}

plGrassSourceHandle plGrassGpuManager::AllocateSource(plUInt32 uiWorldIndex, const plGrassSettings& settings, const plTransform& transform, float fTextureAspect)
{
  if (s_pData == nullptr)
    return plGrassSourceHandle();

  PL_LOCK(s_pData->m_Mutex);
  Data::World& world = s_pData->GetOrCreateWorld(uiWorldIndex);

  plUInt32 uiSlot = 0;
  if (!world.m_FreeSlots.IsEmpty())
  {
    uiSlot = world.m_FreeSlots.PeekBack();
    world.m_FreeSlots.PopBack();
  }
  else
  {
    uiSlot = world.m_Sources.GetCount();
    if (uiSlot >= (1u << 20) - 1)
      return plGrassSourceHandle();
    world.m_Sources.ExpandAndGetRef();
  }

  Data::Source& source = world.m_Sources[uiSlot];
  const plUInt32 uiGeneration = source.m_uiGeneration;
  source = Data::Source();
  source.m_uiGeneration = uiGeneration;
  source.m_bUsed = true;
  source.m_Settings = settings;
  source.m_Settings.Sanitize();
  source.m_fTextureAspect = fTextureAspect;
  source.m_Transform = transform;
  ++world.m_uiUsedSources;

  return plGrassSourceHandle(plGrassSourceId(uiSlot, uiGeneration & 0xFFF));
}

void plGrassGpuManager::Free(plUInt32 uiWorldIndex, plGrassSourceHandle& inout_hSource)
{
  if (s_pData == nullptr || inout_hSource.IsInvalidated())
  {
    inout_hSource.Invalidate();
    return;
  }

  PL_LOCK(s_pData->m_Mutex);
  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
  {
    plUInt32 uiSlot = 0;
    if (Data::Source* pSource = Data::FindSource(*pWorld, inout_hSource, &uiSlot))
    {
      Data::ClearSourceStrands(*pWorld, *pSource);
      pSource->m_bUsed = false;
      // Skip the generation that equals the invalid id.
      pSource->m_uiGeneration = (pSource->m_uiGeneration + 1) & 0xFFF;
      if (pSource->m_uiGeneration == 0 || pSource->m_uiGeneration == 0xFFF)
        pSource->m_uiGeneration = 1;
      pWorld->m_PendingSlots.PushBack({uiSlot, plRenderWorld::GetFrameCounter()});
      --pWorld->m_uiUsedSources;
    }
  }

  inout_hSource.Invalidate();
}

void plGrassGpuManager::SetSourceTransform(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plTransform& transform)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
  {
    if (Data::Source* pSource = Data::FindSource(*pWorld, hSource))
      pSource->m_Transform = transform;
  }
}

plResult plGrassGpuManager::UpdateSourceSettings(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plGrassSettings& settings, float fTextureAspect)
{
  if (s_pData == nullptr)
    return PL_FAILURE;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource) : nullptr;
  if (pSource == nullptr)
    return PL_FAILURE;

  plGrassSettings sanitized = settings;
  sanitized.Sanitize();
  if (GrassSegments(sanitized) != GrassSegments(pSource->m_Settings))
    return PL_FAILURE;

  pSource->m_Settings = sanitized;
  pSource->m_fTextureAspect = fTextureAspect;
  return PL_SUCCESS;
}

plResult plGrassGpuManager::UploadCoverageTile(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, const plGrassCoverageTile& tile, plUInt32 uiTypeSeed, float fTexelSize, plUInt32 uiSampleStride)
{
  PL_ASSERT_DEV(uiSampleStride >= 1 && uiSampleStride <= 256 && (uiSampleStride & (uiSampleStride - 1)) == 0, "Grass sample stride must be a power of two in [1, 256]");
  if (uiSampleStride < 1 || uiSampleStride > 256 || (uiSampleStride & (uiSampleStride - 1)) != 0)
    return PL_FAILURE;
  if (s_pData == nullptr)
    return PL_FAILURE;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  plUInt32 uiSlot = 0;
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource, &uiSlot) : nullptr;
  if (pSource == nullptr)
    return PL_FAILURE;

  // A regrown tile keeps the density it was thinned to.
  const plUInt32 uiKey = plGrassCoverageTile::MakeKey(tile.m_iX, tile.m_iY);
  const GrassChunk* pOldChunk = pSource->m_Tiles.GetValue(uiKey);
  const plUInt8 uiDensity = pOldChunk != nullptr ? pOldChunk->m_uiDensity : 255;

  // Per-texel counts from the shared integer maths, so the GPU grows exactly this many strands.
  const float fTexel = plGrassCoverageData::ClampTexelSize(fTexelSize);
  const plUInt32 uiPerTexelQ8 = GrassStrandsPerTexelQ8(pSource->m_Settings.m_fStrandsPerSquareMeter, fTexel);
  plUInt32 offsets[plGrassCoverageTile::TexelCount + 1];
  offsets[0] = 0;
  float fMinHeight = plMath::MaxValue<float>();
  float fMaxHeight = -plMath::MaxValue<float>();
  for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
  {
    const plGrassCoverageTexel& texel = tile.m_Texels[i];
    plUInt32 uiCount = 0;
    if (texel.m_uiDensity != 0)
    {
      const plUInt32 uiHash = GrassHash(tile.m_iX, tile.m_iY, i, uiTypeSeed);
      uiCount = GrassResidentCount(GrassStrandCountForTexelQ8(texel.m_uiDensity, uiPerTexelQ8, uiHash), uiHash, uiSampleStride);
      fMinHeight = plMath::Min(fMinHeight, tile.GetTexelHeight(i));
      fMaxHeight = plMath::Max(fMaxHeight, tile.GetTexelHeight(i));
    }
    offsets[i + 1] = offsets[i] + uiCount;
  }

  const plUInt32 uiTotal = offsets[plGrassCoverageTile::TexelCount];
  if (uiTotal == 0)
  {
    Data::RemoveTile(*pWorld, *pSource, uiKey);
    return PL_SUCCESS;
  }

  GrassChunk chunk;
  if (pOldChunk != nullptr && uiTotal <= pOldChunk->m_Range.m_uiCount)
  {
    // Regrow in place: the tile's own strands take the new growth and the rest of its range is freed, so this never
    // needs pool space. The commands of one frame run in queue order where their strands overlap.
    const GrassRange oldRange = pOldChunk->m_Range;
    if (pOldChunk->m_uiDensity < 255 && pWorld->m_uiThinnedChunks > 0)
      --pWorld->m_uiThinnedChunks;
    pSource->m_Tiles.Remove(uiKey);
    pOldChunk = nullptr;

    chunk.m_Range = GrassRange{oldRange.m_uiFirst, uiTotal};
    Data::FreeRange(*pWorld, *pSource, GrassRange{oldRange.m_uiFirst + uiTotal, oldRange.m_uiCount - uiTotal});
  }
  else if (Data::AllocateRange(*pWorld, uiSlot, uiTotal, chunk.m_Range))
  {
    // Allocated while the previous strands were still held; only now are they freed.
    Data::RemoveTile(*pWorld, *pSource, uiKey);
    pOldChunk = nullptr;
  }
  else
  {
    if (!pWorld->m_bLoggedFull)
    {
      pWorld->m_bLoggedFull = true;
      plLog::Warning("Grass pool of world {} is full (Vegetation.Grass.MaxStrands = {}); painted tiles are not grown.", uiWorldIndex, cvar_VegetationGrassMaxStrands.GetValue());
    }
    return PL_FAILURE;
  }

  const float fTileSize = fTexel * GRASS_TILE_TEXELS;
  chunk.m_Bounds = plBoundingBox::MakeFromMinMax(plVec3(tile.m_iX * fTileSize, tile.m_iY * fTileSize, fMinHeight), plVec3((tile.m_iX + 1) * fTileSize, (tile.m_iY + 1) * fTileSize, fMaxHeight));

  const plUInt32 uiUploadOffset = pWorld->m_Upload.GetCount();
  static_assert(sizeof(plGrassCoverageTexel) * plGrassCoverageTile::TexelCount == GRASS_TILE_TEXEL_UINTS * sizeof(plUInt32));
  pWorld->m_Upload.SetCountUninitialized(uiUploadOffset + GRASS_TILE_PAYLOAD_UINTS);
  plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(pWorld->m_Upload.GetData() + uiUploadOffset), reinterpret_cast<const plUInt8*>(tile.m_Texels), GRASS_TILE_TEXEL_UINTS * sizeof(plUInt32));
  plMemoryUtils::Copy(pWorld->m_Upload.GetData() + uiUploadOffset + GRASS_TILE_TEXEL_UINTS, offsets, plGrassCoverageTile::TexelCount + 1);

  Data::ForEachPagePiece(*pSource, chunk.m_Range, [&](plUInt32, plUInt32 uiGlobalFirst, plUInt32 uiChunkOffset, plUInt32 uiCount)
    {
      GrassCommand& cmd = pWorld->m_Commands.ExpandAndGetRef();
      cmd.m_Type = GrassCommandType::Coverage;
      cmd.m_uiSourceSlot = uiSlot;
      cmd.m_uiGlobalFirst = uiGlobalFirst;
      cmd.m_uiCount = uiCount;
      cmd.m_uiChunkFirst = uiChunkOffset;
      cmd.m_uiUploadOffset = uiUploadOffset;
      cmd.m_iTileX = tile.m_iX;
      cmd.m_iTileY = tile.m_iY;
      cmd.m_uiTypeSeed = uiTypeSeed;
      cmd.m_uiSampleStride = uiSampleStride;
      cmd.m_fTexelSize = fTexel;
      cmd.m_fBaseHeight = tile.m_fBaseHeight;
    });

  Data::AddChunkBoundsToPages(*pWorld, *pSource, chunk);
  chunk.m_uiDensity = uiDensity;
  if (uiDensity < 255)
    ++pWorld->m_uiThinnedChunks;
  pSource->m_Tiles.Insert(uiKey, chunk);
  return PL_SUCCESS;
}

void plGrassGpuManager::RemoveCoverageTile(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plUInt32 uiTileKey)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource) : nullptr;
  if (pSource == nullptr)
    return;

  Data::RemoveTile(*pWorld, *pSource, uiTileKey);
}

void plGrassGpuManager::SetCoverageTileDensity(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plUInt32 uiTileKey, float fDensity)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource) : nullptr;
  GrassChunk* pChunk = pSource ? pSource->m_Tiles.GetValue(uiTileKey) : nullptr;
  if (pChunk == nullptr)
    return;

  // 0 marks pages without a chunk in ProcessWorld, so a chunk keeps at least 1/255.
  const plUInt8 uiDensity = static_cast<plUInt8>(plMath::Clamp(plMath::Round(fDensity * 255.0f), 1.0f, 255.0f));
  if (pChunk->m_uiDensity < 255 && uiDensity == 255)
    --pWorld->m_uiThinnedChunks;
  else if (pChunk->m_uiDensity == 255 && uiDensity < 255)
    ++pWorld->m_uiThinnedChunks;
  pChunk->m_uiDensity = uiDensity;
}

void plGrassGpuManager::GetPoolUsage(plUInt32 uiWorldIndex, plUInt32& out_uiUsedPages, plUInt32& out_uiBudgetPages)
{
  out_uiUsedPages = 0;
  out_uiBudgetPages = GrassMaxPages();
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
    out_uiUsedPages = pWorld->m_Pages.GetCount() - pWorld->m_FreePages.GetCount();
}

void plGrassGpuManager::ClearSource(plUInt32 uiWorldIndex, plGrassSourceHandle hSource)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource) : nullptr;
  if (pSource != nullptr)
    Data::ClearSourceStrands(*pWorld, *pSource);
}

plResult plGrassGpuManager::UploadRoots(plUInt32 uiWorldIndex, plGrassSourceHandle hSource, plArrayPtr<const plGrassStrandStatic> roots, const plBoundingBox& localBounds)
{
  if (s_pData == nullptr)
    return PL_FAILURE;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  plUInt32 uiSlot = 0;
  Data::Source* pSource = pWorld ? Data::FindSource(*pWorld, hSource, &uiSlot) : nullptr;
  if (pSource == nullptr)
    return PL_FAILURE;

  if (pSource->m_bHasRoots)
  {
    const GrassRange oldRange = pSource->m_Roots.m_Range;
    pSource->m_Roots = GrassChunk();
    pSource->m_bHasRoots = false;
    Data::FreeRange(*pWorld, *pSource, oldRange);
  }

  if (roots.IsEmpty())
    return PL_SUCCESS;

  GrassChunk chunk;
  if (!Data::AllocateRange(*pWorld, uiSlot, roots.GetCount(), chunk.m_Range))
  {
    if (!pWorld->m_bLoggedFull)
    {
      pWorld->m_bLoggedFull = true;
      plLog::Warning("Grass pool of world {} is full (Vegetation.Grass.MaxStrands = {}); {} surface strands are not grown.", uiWorldIndex, cvar_VegetationGrassMaxStrands.GetValue(), roots.GetCount());
    }
    return PL_FAILURE;
  }
  chunk.m_Bounds = localBounds;

  static_assert(sizeof(plGrassStrandStatic) == 8 * sizeof(plUInt32));
  const plUInt32 uiUploadOffset = pWorld->m_Upload.GetCount();
  pWorld->m_Upload.SetCountUninitialized(uiUploadOffset + roots.GetCount() * 8);
  plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(pWorld->m_Upload.GetData() + uiUploadOffset), reinterpret_cast<const plUInt8*>(roots.GetPtr()), roots.GetCount() * sizeof(plGrassStrandStatic));

  Data::ForEachPagePiece(*pSource, chunk.m_Range, [&](plUInt32, plUInt32 uiGlobalFirst, plUInt32 uiChunkOffset, plUInt32 uiCount)
    {
      GrassCommand& cmd = pWorld->m_Commands.ExpandAndGetRef();
      cmd.m_Type = GrassCommandType::Roots;
      cmd.m_uiSourceSlot = uiSlot;
      cmd.m_uiGlobalFirst = uiGlobalFirst;
      cmd.m_uiCount = uiCount;
      cmd.m_uiChunkFirst = uiChunkOffset;
      cmd.m_uiUploadOffset = uiUploadOffset;
    });

  Data::AddChunkBoundsToPages(*pWorld, *pSource, chunk);
  pSource->m_Roots = chunk;
  pSource->m_bHasRoots = true;
  return PL_SUCCESS;
}

void plGrassGpuManager::MakeStrandStatic(const plVec3& vRoot, const plVec3& vAxis, float fLength, float fTangentAngle01, plUInt32 uiAtlasIndex, plUInt32 uiSeed, plGrassStrandStatic& out_strand)
{
  // Octahedral axis, 2 x unorm16, matching GrassOctDecode.
  plVec3 n = vAxis;
  const float fSum = plMath::Abs(n.x) + plMath::Abs(n.y) + plMath::Abs(n.z);
  n = fSum > 0.00001f ? n / fSum : plVec3(0, 0, 1);
  float ex = n.x;
  float ey = n.y;
  if (n.z < 0.0f)
  {
    ex = (1.0f - plMath::Abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
    ey = (1.0f - plMath::Abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
  }
  const plUInt32 qx = static_cast<plUInt32>(plMath::Round(plMath::Saturate(ex * 0.5f + 0.5f) * 65535.0f));
  const plUInt32 qy = static_cast<plUInt32>(plMath::Round(plMath::Saturate(ey * 0.5f + 0.5f) * 65535.0f));

  out_strand.Root = vRoot;
  out_strand.Axis = qx | (qy << 16);
  out_strand.LengthAngle = static_cast<plUInt32>(plFloat16(fLength).GetRawData()) |
                           (static_cast<plUInt32>(plMath::Saturate(fTangentAngle01) * 65535.0f) << 16);
  out_strand.AtlasFlags = (plMath::Min(uiAtlasIndex, 255u)) | (GRASS_STRAND_FLAG_ALIVE << 8);
  out_strand.Seed = uiSeed;
  out_strand.Reserved = 0;
}

plUInt32 plGrassGpuManager::GetSourceSlot(plUInt32 uiWorldIndex, plGrassSourceHandle hSource)
{
  if (s_pData == nullptr)
    return plInvalidIndex;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  plUInt32 uiSlot = plInvalidIndex;
  if (pWorld == nullptr || Data::FindSource(*pWorld, hSource, &uiSlot) == nullptr)
    return plInvalidIndex;
  return uiSlot;
}

void plGrassGpuManager::SubmitWorldFrame(plUInt32 uiWorldIndex, const plVec3& vMainCamera, bool bHasCamera, plTime deltaTime, bool bSimulate)
{
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  if (pWorld == nullptr)
    return;

  // Several owners (surface components, the world module) may submit for the same world in one update.
  const plUInt64 uiFrame = plRenderWorld::GetFrameCounter();
  if (pWorld->m_uiSubmitFrame != uiFrame)
    pWorld->m_DeltaTime = deltaTime;

  pWorld->m_vMainCamera = vMainCamera;
  pWorld->m_bHasCamera = bHasCamera;
  pWorld->m_bSimulate = bSimulate;
  pWorld->m_uiSubmitFrame = uiFrame;
}

bool plGrassGpuManager::GetRenderSnapshot(plUInt32 uiWorldIndex, plGrassRenderSnapshot& out_snapshot)
{
  out_snapshot = plGrassRenderSnapshot();
  if (s_pData == nullptr || !cvar_VegetationGrassEnable)
    return false;

  PL_LOCK(s_pData->m_Mutex);
  Data::World* pWorld = s_pData->FindWorld(uiWorldIndex);
  if (pWorld == nullptr || !pWorld->m_Snapshot.m_bValid)
    return false;

  out_snapshot = pWorld->m_Snapshot;
  return true;
}

void plGrassGpuManager::GetStatistics(plUInt32 uiWorldIndex, plUInt32& out_uiResidentStrands, plUInt32& out_uiSimulatedStrands, plUInt32& out_uiCapacityStrands)
{
  out_uiResidentStrands = 0;
  out_uiSimulatedStrands = 0;
  out_uiCapacityStrands = GrassMaxPages() * GRASS_PAGE_SIZE;
  if (s_pData == nullptr)
    return;

  PL_LOCK(s_pData->m_Mutex);
  if (Data::World* pWorld = s_pData->FindWorld(uiWorldIndex))
  {
    out_uiResidentStrands = pWorld->m_uiResidentStrands;
    out_uiSimulatedStrands = pWorld->m_uiSimulatedStrands;
  }
}

void plGrassGpuManager::OnRenderEvent(const plRenderWorldRenderEvent& e)
{
  if (e.m_Type != plRenderWorldRenderEvent::Type::BeginRender || s_pData == nullptr)
    return;

  PL_PROFILE_SCOPE("Grass");
  PL_LOCK(s_pData->m_Mutex);

  // The previous frame's graph finished recording before this frame's BeginRender.
  s_pData->m_FrameWork.Clear();

  plUInt32 uiResident = 0;
  plUInt32 uiSimulated = 0;
  plUInt32 uiUsedPages = 0;
  plUInt32 uiUploadBytes = 0;
  plHybridArray<plUInt32, 4> emptyWorlds;

  for (auto it = s_pData->m_Worlds.GetIterator(); it.IsValid(); ++it)
  {
    Data::World& world = *it.Value();

    // A world without sources releases its pool after a grace period.
    if (world.m_uiUsedSources == 0 && world.m_PendingSlots.IsEmpty())
    {
      if (++world.m_uiIdleFrames > 60)
        emptyWorlds.PushBack(it.Key());
    }
    else
    {
      world.m_uiIdleFrames = 0;
    }

    if (!cvar_VegetationGrassEnable)
    {
      world.m_Snapshot.m_bValid = false;
      continue;
    }

    s_pData->ProcessWorld(it.Key(), world);
    uiResident += world.m_uiResidentStrands;
    uiSimulated += world.m_uiSimulatedStrands;
    uiUsedPages += world.m_uiUsedPages;
    uiUploadBytes += world.m_uiUploadBytes;
  }

  for (plUInt32 uiWorld : emptyWorlds)
  {
    Data::World* pWorld = nullptr;
    if (s_pData->m_Worlds.Remove(uiWorld, &pWorld))
    {
      s_pData->DestroyGpuResources(*pWorld);
      PL_DEFAULT_DELETE(pWorld);
    }
  }

  // PreRender graphs execute before every view's graph of the frame, so the cull and draw passes see this frame's
  // generation and simulation. Buffers destroyed above stay alive until the device's end of frame.
  if (!s_pData->m_FrameWork.IsEmpty())
  {
    if (s_pData->m_pGraph == nullptr)
      s_pData->m_pGraph = plRenderGraphManager::CreateRenderGraph("Grass Update", plRenderGraphPhase::PreRender);

    for (plUniquePtr<Data::FrameWork>& pWork : s_pData->m_FrameWork)
      s_pData->DeclareWorldPasses(*s_pData->m_pGraph.Borrow(), *pWork);

    plRenderGraphManager::EnqueueRenderGraph(s_pData->m_pGraph);
  }

  // Summed over every world; the page budget applies to each world separately.
  plStats::SetStat("Vegetation/Grass/ResidentStrands", uiResident);
  plStats::SetStat("Vegetation/Grass/SimulatedStrands", uiSimulated);
  plStats::SetStat("Vegetation/Grass/PoolPagesUsed", uiUsedPages);
  plStats::SetStat("Vegetation/Grass/PoolPagesBudget", GrassMaxPages());
  plStats::SetStat("Vegetation/Grass/UploadKB", (uiUploadBytes + 1023) / 1024);
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassGpuManager);
