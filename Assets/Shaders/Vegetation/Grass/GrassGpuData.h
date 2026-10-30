#pragma once

#include <Shaders/Common/ConstantBufferMacros.h>
#include <Shaders/Common/Platforms.h>

// GPU grass strand pool, shared by plGrassGpuManager / plGrassCullPass / plGrassRenderer (C++) and the grass compute
// and material shaders (HLSL).
//
// Pool layout, one set per world:
//   GrassStatic   RWStructuredBuffer<plGrassStrandStatic>, 32 bytes per strand, indexed by the global strand index
//                 page * GRASS_PAGE_SIZE + strand in page.
//   GrassState    RWByteAddressBuffer, GRASS_STATE_SEGMENT_BYTES (20) per simulated segment: the segment end point after
//                 the last step, before the last step (Verlet velocity) and at the start of the frame (the pose the
//                 previous frame rendered, for motion vectors), as half-precision world-space offsets from the strand's
//                 world root:
//                   uint0 = current.x | current.y << 16, uint1 = current.z | step.x << 16, uint2 = step.y | step.z << 16,
//                   uint3 = frame.x | frame.y << 16, uint4 = frame.z (high 16 bits unused)
//                 Segment j of a strand lives at (page.StateBase + strand in page * source segment count + j) * 20.
//                 Pool memory per strand: 32 static bytes plus 20 per segment, e.g. 92 bytes at 3 segments, so the
//                 default Vegetation.Grass.MaxStrands of 512k strands at 3 segments is about 48 MB.
//   GrassPages    StructuredBuffer<plGrassPageGpu>, one entry per pool page. A page belongs to one source.
//   GrassSources  StructuredBuffer<plGrassSourceGpu>, one entry per source slot.
//   GrassAtlas    StructuredBuffer<plGrassAtlasGpu>, the atlas rects of every source, CountsAtlas.zw addresses them.
//
// Per view (plGrassCullPass):
//   GrassArgs     RWByteAddressBuffer of draw-instanced indirect records, GRASS_ARGS_STRIDE bytes each, record
//                 source slot * GRASS_LOD_COUNT + lod.
//   GrassVisible  RWStructuredBuffer<uint> of global strand indices. A source owns 2 * VisibleCapacity entries from
//                 VisibleBase: LOD 0 grows up from VisibleBase, LOD 1 grows up from VisibleBase + VisibleCapacity and
//                 LOD 2 grows down from VisibleBase + 2 * VisibleCapacity - 1. Every strand lands in at most one bucket,
//                 so LOD 1 and LOD 2 never overlap.

#define GRASS_PAGE_SIZE 4096
#define GRASS_PAGE_SHIFT 12
#define GRASS_THREAD_GROUP_SIZE 64
#define GRASS_GROUPS_PER_PAGE 64
#define GRASS_CULL_THREAD_GROUP_SIZE 256 ///< the cull aggregates its bucket atomics per group, so it runs wider groups
#define GRASS_CULL_GROUPS_PER_PAGE 16
#define GRASS_LOD_COUNT 3
#define GRASS_MAX_SEGMENTS 10
#define GRASS_MAX_BILLBOARDS 10
#define GRASS_STATE_SEGMENT_BYTES 20
#define GRASS_STATE_FRAME_OFFSET 12 ///< byte offset of the frame-start pose inside a state segment
#define GRASS_ARGS_STRIDE 20 ///< DrawIndexedInstancedIndirect

// plGrassStrandStatic::AtlasFlags bits 8-15
#define GRASS_STRAND_FLAG_ALIVE 1u

// plGrassPageGpu::Flags
#define GRASS_PAGE_FLAG_SIMULATED 1u ///< the state buffer holds the simulated pose; otherwise the rest pose applies
#define GRASS_PAGE_FLAG_WAKE 2u      ///< first simulated frame after a pause: previous = current before stepping
#define GRASS_PAGE_DENSITY_SHIFT 16  ///< bits 16-23: cull-side keep fraction as unorm8, 255 keeps every strand

// plGrassSourceGpu::Layout.z
#define GRASS_SOURCE_FLAG_CAMERA_BEND 1u
#define GRASS_SOURCE_FLAG_STEPPED 2u ///< the simulation advanced this frame, so previous positions carry motion
#define GRASS_SOURCE_FLAG_BLADES 4u
#define GRASS_SOURCE_FLAG_ADAPTIVE 8u

// plGrassGenerateConstants::Mode
#define GRASS_GENERATE_COVERAGE 0u ///< grow strands from one coverage tile in the upload buffer
#define GRASS_GENERATE_ROOTS 1u    ///< copy CPU-built plGrassStrandStatic records from the upload buffer
#define GRASS_GENERATE_KILL 2u     ///< mark a strand range dead

// Coverage tile payload in the upload buffer, in uints: 2 per texel (plGrassCoverageTexel), then 257 prefix offsets.
#define GRASS_TILE_TEXEL_UINTS 512
#define GRASS_TILE_PAYLOAD_UINTS 769

/// Static data of one strand, 32 bytes. Positions and directions are in source space; the source's object transform
/// maps them into the world.
struct PL_SHADER_STRUCT plGrassStrandStatic
{
  FLOAT3(Root);       ///< source-space root position
  UINT1(Axis);        ///< octahedral source-space growth axis, x and y as unorm16 of (e * 0.5 + 0.5)
  UINT1(LengthAngle); ///< low 16 bits: length in metres as half; high 16 bits: tangent angle as unorm16 of 2 pi
  UINT1(AtlasFlags);  ///< bits 0-7 atlas rect index, bits 8-15 GRASS_STRAND_FLAG_*
  UINT1(Seed);        ///< per-strand hash; billboard angles and sizes derive from it
  UINT1(Reserved); ///< progressive density rank, unorm32; high 16 AtlasFlags bits hold the clump seed
};

/// One pool page.
struct PL_SHADER_STRUCT plGrassPageGpu
{
  UINT1(SourceIndex);
  UINT1(StrandCount); ///< strands in use from the page start; every dispatch bounds-checks against it
  UINT1(StateBase);   ///< first state segment of the page
  UINT1(Flags);       ///< GRASS_PAGE_FLAG_* and the density at GRASS_PAGE_DENSITY_SHIFT
};

/// One atlas rect of a source.
struct PL_SHADER_STRUCT plGrassAtlasGpu
{
  FLOAT4(TexMulAdd);
  FLOAT1(Size);   ///< strand length multiplier
  FLOAT1(Aspect); ///< |TexMulAdd.x / TexMulAdd.y|
  FLOAT2(Padding);
};

/// One grass source: a surface component or one grass type of a painted layer.
struct PL_SHADER_STRUCT plGrassSourceGpu
{
  TRANSFORM(ObjectToWorld);
  TRANSFORM(LastObjectToWorld);
  FLOAT4(Shape);     ///< x length, y width, z texture aspect, w randomness
  FLOAT4(Sim);       ///< x stiffness, y drag, z gravity power, w wind influence
  FLOAT4(Touch);     ///< x touch crush, y touch recovery, z largest object scale, w atlas uniformity
  FLOAT4(Distances); ///< x view, y simulation, z LOD start, w shadow
  UINT4(Counts);     ///< x segments, y billboards, z atlas start, w atlas count
  UINT4(Layout);     ///< x visible base, y visible capacity, z GRASS_SOURCE_FLAG_*, w random seed
  FLOAT4(Blade);     ///< width/length, tilt, bend, side curve
  FLOAT4(Clump);     ///< size, orientation/shape coherence, colour variation, rounded normal strength
  FLOAT4(Detail);    ///< minimum density, strands/m2, ground-cover start distance, maximum patch size (0 disables)
  FLOAT4(MainCamera); ///< xyz simulation/shadow LOD reference
};

#if PL_DISABLED(PLATFORM_SHADER)
static_assert(sizeof(plGrassStrandStatic) == 32);
static_assert(sizeof(plGrassPageGpu) == 16);
static_assert(sizeof(plGrassAtlasGpu) == 32);
static_assert(sizeof(plGrassSourceGpu) == 256);
PL_DEFINE_AS_POD_TYPE(plGrassStrandStatic);
PL_DEFINE_AS_POD_TYPE(plGrassPageGpu);
PL_DEFINE_AS_POD_TYPE(plGrassAtlasGpu);
PL_DEFINE_AS_POD_TYPE(plGrassSourceGpu);
#endif

// Each shader defines the one constants block it uses before including this header; C++ sees all of them. Member
// names carry a per-block prefix because DX12 cbuffer members share the global namespace.
#if PL_DISABLED(PLATFORM_SHADER)
#  define GRASS_GENERATE_CONSTANTS
#  define GRASS_SIMULATE_CONSTANTS
#  define GRASS_CULL_CONSTANTS
#  define GRASS_DRAW_CONSTANTS
#endif

#if defined(GRASS_GENERATE_CONSTANTS)
/// Per dispatch of GrassGenerate.
BEGIN_PUSH_CONSTANTS(plGrassGenerateConstants)
{
  UINT1(GenMode);
  UINT1(GenSourceIndex);
  UINT1(GenGlobalFirst);       ///< global strand index of thread 0
  UINT1(GenCount);             ///< threads with work
  UINT1(GenChunkFirst);        ///< index of thread 0 inside its tile or root chunk
  UINT1(GenUploadOffset);      ///< first uint of the chunk payload in the upload buffer
  UINT1(GenUploadCount);       ///< uints in the upload buffer
  UINT1(GenPoolStrands);       ///< strands the static buffer holds
  INT1(GenTileX);
  INT1(GenTileY);
  UINT1(GenTypeSeed);
  UINT1(GenSampleStride);      ///< power-of-two stride through original per-texel ordinals
  FLOAT1(GenTexelSize);
  FLOAT1(GenBaseHeight);
  UINT1(GenStateSegments);     ///< segments the state buffer holds
  UINT1(GenSourceCount);
}
END_PUSH_CONSTANTS(plGrassGenerateConstants)
#endif

#if defined(GRASS_SIMULATE_CONSTANTS)
/// Per dispatch of GrassSimulate.
BEGIN_PUSH_CONSTANTS(plGrassSimulateConstants)
{
  UINT1(SimActivePageCount);
  UINT1(SimPageCount);
  UINT1(SimSourceCount);
  UINT1(SimStateSegments);
  UINT1(SimSteps);
  FLOAT1(SimStepTime);
  UINT1(SimPoolStrands);
  UINT1(SimPadding0);
}
END_PUSH_CONSTANTS(plGrassSimulateConstants)
#endif

#define GRASS_CULL_FLAG_SHADOW 1u
#define GRASS_CULL_FLAG_OCCLUSION 2u

#if defined(GRASS_CULL_CONSTANTS)
/// Per view of GrassCull.
CONSTANT_BUFFER(plGrassCullConstants, 3)
{
  FLOAT4(CullFrustumPlane0);
  FLOAT4(CullFrustumPlane1);
  FLOAT4(CullFrustumPlane2);
  FLOAT4(CullFrustumPlane3);
  FLOAT4(CullFrustumPlane4);
  FLOAT4(CullFrustumPlane5);

  MAT4(CullHiZViewProjection);

  FLOAT3(CullCameraPosition);     ///< distance and LOD reference of this view
  UINT1(CullPlaneCount);

  FLOAT3(CullMainCameraPosition); ///< shadow distance reference
  UINT1(CullFlags);

  UINT2(CullHiZSize);
  UINT1(CullHiZMipCount);
  UINT1(CullResidentPageCount); ///< pages in the view's GrassViewPages list; the dispatch covers them

  UINT1(CullPageCount);
  UINT1(CullSourceCount);
  UINT1(CullVisibleEntries); ///< entries the visible buffer holds
  UINT1(CullArgsRecords);    ///< records the args buffer holds

  UINT1(CullPoolStrands);
  UINT1(CullInstanceCount);  ///< instance count of every indirect record (2 for stereo views)
  UINT1(CullPadding0);
  UINT1(CullPadding1);
};
#endif

#if defined(GRASS_DRAW_CONSTANTS)
/// Per draw of the grass material.
BEGIN_PUSH_CONSTANTS(plGrassDrawConstants)
{
  UINT1(DrawInstanceDataOffset);
  UINT1(DrawMaterialDataOffset);
  UINT1(DrawSourceIndex);
  UINT1(DrawLod);
  UINT1(DrawSourceCount);
  UINT1(DrawPageCount);
  UINT1(DrawVisibleEntries);
  UINT1(DrawStateSegments);
  UINT1(DrawAtlasCount);
  UINT1(DrawPoolStrands);
  UINT1(DrawDisableProceduralAO);
  UINT1(DrawPadding1);
}
END_PUSH_CONSTANTS(plGrassDrawConstants)
#endif
