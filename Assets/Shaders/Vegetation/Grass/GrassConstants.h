#pragma once

#include <Shaders/Common/Platforms.h>

// Shared by C++ (plGrassCoverageData, the strand pool allocation) and HLSL (strand generation). Everything that decides
// how many strands a coverage texel grows is integer maths, so the CPU allocation and the GPU generation always agree.

#define GRASS_TILE_TEXELS 16
#define GRASS_TILE_TEXEL_COUNT 256
#define GRASS_TEXEL_FLAG_DISCONTINUOUS 1

// Strands per texel at full density are clamped so density * value fits into 32 bits.
#define GRASS_MAX_STRANDS_PER_TEXEL_Q8 0xFFFFFFu

#if PL_ENABLED(PLATFORM_SHADER)
#  define GRASS_UINT uint
#  define GRASS_INT int
#  define GRASS_INLINE
#  define GRASS_FLOOR(x) floor(x)
#  define GRASS_MIN(a, b) min(a, b)
#else
#  define GRASS_UINT plUInt32
#  define GRASS_INT plInt32
#  define GRASS_INLINE inline
#  define GRASS_FLOOR(x) plMath::Floor(x)
#  define GRASS_MIN(a, b) plMath::Min(a, b)
#endif

/// PCG output permutation of one LCG step. Unsigned overflow wraps identically in C++ and HLSL.
GRASS_INLINE GRASS_UINT GrassPcgHash(GRASS_UINT v)
{
  GRASS_UINT state = v * 747796405u + 2891336453u;
  GRASS_UINT word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

/// Seed of one coverage texel. tileX and tileY are the tile's int16 coordinates, texelIndex is y * 16 + x inside the
/// tile and typeSeed is plGrassCoverageData::GetTypeSeed() of the grass type. The low byte drives the stochastic
/// rounding of the strand count; strand attributes should hash further from the result.
GRASS_INLINE GRASS_UINT GrassHash(GRASS_INT tileX, GRASS_INT tileY, GRASS_UINT texelIndex, GRASS_UINT typeSeed)
{
  GRASS_UINT h = GrassPcgHash(typeSeed ^ 0x9E3779B9u);
  h = GrassPcgHash(h ^ (((GRASS_UINT)tileX & 0xFFFFu) | (((GRASS_UINT)tileY & 0xFFFFu) << 16u)));
  h = GrassPcgHash(h ^ texelIndex);
  return h;
}

/// Strands one texel grows at full density, in 1/256 strands. The only float step: one multiply chain and a floor.
/// Compute it once per type and layer on the CPU and hand the integer to the GPU.
GRASS_INLINE GRASS_UINT GrassStrandsPerTexelQ8(float strandsPerM2, float texelSize)
{
  const float fValue = GRASS_FLOOR(((strandsPerM2 * texelSize) * texelSize) * 256.0f);
  if (!(fValue > 0.0f))
    return 0u;
  if (fValue >= 16777215.0f)
    return GRASS_MAX_STRANDS_PER_TEXEL_Q8;
  return (GRASS_UINT)fValue;
}

/// Strand count of one texel from its density (0..255): the expected count in 1/256 strands, stochastically rounded
/// with the low byte of the texel hash. Pure integer maths.
GRASS_INLINE GRASS_UINT GrassStrandCountForTexelQ8(GRASS_UINT density, GRASS_UINT strandsPerTexelQ8, GRASS_UINT hash)
{
  const GRASS_UINT expectedQ8 = (GRASS_MIN(density, 255u) * GRASS_MIN(strandsPerTexelQ8, GRASS_MAX_STRANDS_PER_TEXEL_Q8) + 127u) / 255u;
  return (expectedQ8 + (hash & 0xFFu)) >> 8u;
}

/// Convenience overload for the CPU: GrassStrandsPerTexelQ8 followed by GrassStrandCountForTexelQ8.
GRASS_INLINE GRASS_UINT GrassStrandCountForTexel(GRASS_UINT density, float strandsPerM2, float texelSize, GRASS_UINT hash)
{
  return GrassStrandCountForTexelQ8(density, GrassStrandsPerTexelQ8(strandsPerM2, texelSize), hash);
}

/// Nested power-of-two sampling. The selected original ordinals have their low bits equal to the texel hash.
/// Their bit-reversed ranks are in [0, 1/stride), so changing residency never moves surviving roots.
GRASS_INLINE GRASS_UINT GrassResidentCount(GRASS_UINT count, GRASS_UINT hash, GRASS_UINT stride)
{
  const GRASS_UINT first = hash & (stride - 1u);
  return count > first ? 1u + (count - first - 1u) / stride : 0u;
}

/// Stable Voronoi clump identifier, shared by CPU surface growth and GPU coverage growth.
GRASS_INLINE GRASS_UINT GrassClumpSeed(float x, float y, float size, GRASS_UINT seed)
{
  x /= size;
  y /= size;
  const GRASS_INT cx = (GRASS_INT)GRASS_FLOOR(x);
  const GRASS_INT cy = (GRASS_INT)GRASS_FLOOR(y);
  float best = 100.0f;
  GRASS_UINT result = 0u;
  for (GRASS_INT dy = -1; dy <= 1; ++dy)
  {
    for (GRASS_INT dx = -1; dx <= 1; ++dx)
    {
      const GRASS_UINT h = GrassHash(cx + dx, cy + dy, 0u, seed);
      const float px = (float)(cx + dx) + 0.25f + (float)(h & 65535u) * (0.5f / 65535.0f);
      const float py = (float)(cy + dy) + 0.25f + (float)(h >> 16u) * (0.5f / 65535.0f);
      const float d = (px - x) * (px - x) + (py - y) * (py - y);
      if (d < best)
      {
        best = d;
        result = h;
      }
    }
  }
  return result & 65535u;
}
