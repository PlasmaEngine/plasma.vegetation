#pragma once

// HLSL helpers shared by the grass compute shaders and the grass material. Include after GrassGpuData.h.

#include <Shaders/Vegetation/Grass/GrassConstants.h>

float GrassHashToFloat(uint h)
{
  return float(h >> 8) * (1.0 / 16777216.0);
}

uint GrassPackHalf2(float a, float b)
{
  return f32tof16(a) | (f32tof16(b) << 16);
}

float3 GrassOctDecode(uint packed)
{
  float2 e = float2(float(packed & 0xFFFFu), float(packed >> 16)) / 65535.0 * 2.0 - 1.0;
  float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0)
  {
    float2 folded = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    n.xy = folded;
  }
  float nLength = length(n);
  return nLength > 0.00001 ? n / nLength : float3(0, 0, 1);
}

uint GrassOctEncode(float3 n)
{
  n /= max(abs(n.x) + abs(n.y) + abs(n.z), 0.00001);
  float2 e = n.xy;
  if (n.z < 0.0)
  {
    e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  }
  uint2 q = (uint2)round(saturate(e * 0.5 + 0.5) * 65535.0);
  return q.x | (q.y << 16);
}

float GrassStrandLength(plGrassStrandStatic s)
{
  return f16tof32(s.LengthAngle & 0xFFFFu);
}

float GrassStrandAngle(plGrassStrandStatic s)
{
  return float(s.LengthAngle >> 16) * (6.2831853 / 65535.0);
}

bool GrassStrandIsAlive(plGrassStrandStatic s)
{
  return ((s.AtlasFlags >> 8) & GRASS_STRAND_FLAG_ALIVE) != 0;
}

uint GrassStrandAtlasIndex(plGrassStrandStatic s)
{
  return s.AtlasFlags & 0xFFu;
}

// Billboard b of a strand: rotation around the axis in radians and size relative to the strand. Billboard 0 is
// always the full strand at the tangent.
float GrassBillboardAngle(plGrassStrandStatic s, uint b)
{
  return b == 0 ? 0.0 : GrassHashToFloat(GrassPcgHash(s.Seed ^ (b * 0x9E3779B9u))) * 3.14159265;
}

float GrassBillboardSize(plGrassStrandStatic s, uint b)
{
  return b == 0 ? 1.0 : lerp(0.2, 1.0, GrassHashToFloat(GrassPcgHash(s.Seed + b * 0x85EBCA6Bu)));
}

float3 GrassTransformPosition(Transform t, float3 p)
{
  return mul(TransformToMatrix(t), float4(p, 1.0)).xyz;
}

// Direction transform that keeps normals perpendicular under non-uniform scale: rotation times inverse scale.
float3 GrassTransformAxis(Transform t, float3 n)
{
  float3x3 m = TransformToRotation(t);
  float3 scaleSq = float3(dot(m._m00_m10_m20, m._m00_m10_m20), dot(m._m01_m11_m21, m._m01_m11_m21), dot(m._m02_m12_m22, m._m02_m12_m22));
  float3 d = mul(m, n / max(scaleSq, 1e-10));
  float dLength = length(d);
  return dLength > 1e-8 ? d / dLength : float3(0, 0, 1);
}

// World length of a strand grown along a source-space axis.
float GrassWorldLength(Transform t, float3 axis, float strandLength)
{
  return length(mul(TransformToRotation(t), axis * strandLength));
}

// Growth axis rotated by the billboard's angle, perpendicular to the axis.
float3 GrassBillboardTangent(float3 axis, float tangentAngle, float billboardAngle)
{
  float3 reference = abs(axis.z) < 0.99 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 t0 = normalize(cross(reference, axis));
  float3 t1 = cross(axis, t0);
  float a = tangentAngle + billboardAngle;
  return t0 * cos(a) + t1 * sin(a);
}

uint GrassSegmentsForLod(uint segments, uint lod)
{
  return lod == 0 ? segments : (lod == 1 ? max(1u, (segments + 1u) / 2u) : 1u);
}

uint GrassBillboardsForLod(uint billboards, uint lod)
{
  return lod == 0 ? billboards : (lod == 1 ? min(2u, billboards) : 1u);
}

bool GrassIsBlade(plGrassSourceGpu source)
{
  return (source.Layout.z & GRASS_SOURCE_FLAG_BLADES) != 0;
}

float GrassEffectiveLength(plGrassStrandStatic s, plGrassSourceGpu source)
{
  float scale = GrassIsBlade(source) ? lerp(1.0, lerp(0.65, 1.0, GrassHashToFloat(GrassPcgHash((s.AtlasFlags >> 16) ^ 0x68E31DA4u))), source.Clump.y) : 1.0;
  return GrassStrandLength(s) * scale;
}

float GrassDistanceDensity(plGrassSourceGpu source, float distance)
{
  if ((source.Layout.z & GRASS_SOURCE_FLAG_ADAPTIVE) == 0)
    return 1.0;
  float start = max(source.Distances.z, 1.0);
  return max(source.Detail.x, min(1.0, start * start / max(distance * distance, 1.0)));
}

// Fade individual blades before their deterministic density rank is rejected. Stable across views and frames.
float GrassBladeVisibility(plGrassStrandStatic s, plGrassSourceGpu source, float distance)
{
  float density = GrassDistanceDensity(source, distance);
  float rank = GrassHashToFloat(s.Reserved);
  float thinning = density >= 0.9999 ? 1.0 : saturate((density - rank) / max(density * 0.2, 0.00001));
  float endFade = 1.0 - smoothstep(source.Distances.x * 0.85, max(source.Distances.x, 0.001), distance);
  return thinning * endFade;
}

float GrassBladeAngle(plGrassStrandStatic s, plGrassSourceGpu source)
{
  float angle = GrassStrandAngle(s);
  float clumpAngle = float(s.AtlasFlags >> 16) * (6.2831853 / 65535.0);
  return angle + atan2(sin(clumpAngle - angle), cos(clumpAngle - angle)) * source.Clump.y;
}

float GrassGroundCoverBlend(plGrassSourceGpu source, float distance)
{
  if (!GrassIsBlade(source) || source.Detail.w <= 0.0)
    return 0.0;
  float start = max(source.Detail.z, source.Distances.z * 3.0);
  return smoothstep(start, start + max(start * 0.5, 1.0), distance);
}

// Cubic Bezier in the blade's rest frame. Controls stay inside the length sphere; the tip is shared by every LOD.
void GrassBladeCurve(float t, float3 axis, float3 facing, float3 side, plGrassStrandStatic s, plGrassSourceGpu source,
  out float3 position, out float3 derivative)
{
  float shape = lerp(0.7, 1.0, GrassHashToFloat(GrassPcgHash(s.AtlasFlags >> 16)));
  shape = lerp(1.0, shape, source.Clump.y);
  float tilt = source.Blade.y * shape;
  float bend = source.Blade.z * shape;
  float lateral = source.Blade.w * (GrassHashToFloat(GrassPcgHash(s.Seed)) * 2.0 - 1.0);
  float3 p1 = normalize(axis + facing * tilt) / 3.0;
  float3 p2 = normalize(axis + facing * (tilt + bend * 0.5) + side * lateral) * (2.0 / 3.0);
  float3 p3 = normalize(axis + facing * (tilt + bend) + side * lateral);
  float u = 1.0 - t;
  position = 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
  derivative = 3.0 * u * u * p1 + 6.0 * u * t * (p2 - p1) + 3.0 * t * t * (p3 - p2);
}

// State segment address in bytes; ~0 when out of range.
uint GrassStateAddress(plGrassPageGpu page, uint strandInPage, uint segments, uint segment, uint stateSegments)
{
  if (page.StateBase == 0xFFFFFFFFu)
    return 0xFFFFFFFFu;
  uint index = page.StateBase + strandInPage * segments + segment;
  return index < stateSegments ? index * GRASS_STATE_SEGMENT_BYTES : 0xFFFFFFFFu;
}

// The same analytic pose initializes newly simulated pages and animates non-simulated pages. Inputs can come from
// either frame's wind field; the caller owns that temporal choice.
float3 GrassAnalyticOffset(float3 axis, float worldLength, float fraction, plGrassStrandStatic s,
  plGrassSourceGpu source, float3 wind, float time, float4 touch)
{
  float3 rel = axis * (worldLength * fraction);
  float heightFactor = fraction * fraction;
  wind *= source.Sim.w;
  float speed = length(wind);
  float3 windDir = speed > 0.0001 ? wind / speed : float3(1, 0, 0);
  float phase = GrassHashToFloat(s.Seed) * 6.2831853;
  float response = 1.0 - saturate(source.Sim.x) * 0.5;
  float sway = 1.0 + 0.3 * sin(time * 1.7 + phase);
  float angle = atan(speed * 0.08 * response) * sway * heightFactor;
  float3 bendAxis = cross(axis, windDir);
  float axisLength = length(bendAxis);
  if (axisLength > 0.0001)
  {
    bendAxis /= axisLength;
    rel = rel * cos(angle) + cross(bendAxis, rel) * sin(angle) + bendAxis * dot(bendAxis, rel) * (1.0 - cos(angle));
  }
  float strength = saturate(source.Touch.x);
  rel += float3(touch.xy, 0.0) * (strength * heightFactor * 0.9);
  rel -= axis * (touch.z * strength * worldLength * fraction * 0.6);
  return rel;
}

void GrassUnpackState(uint3 raw, out float3 current, out float3 previous)
{
  current = float3(f16tof32(raw.x & 0xFFFFu), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFFu));
  previous = float3(f16tof32(raw.y >> 16), f16tof32(raw.z & 0xFFFFu), f16tof32(raw.z >> 16));
}

uint3 GrassPackState(float3 current, float3 previous)
{
  return uint3(GrassPackHalf2(current.x, current.y), GrassPackHalf2(current.z, previous.x), GrassPackHalf2(previous.y, previous.z));
}

// Frame-start pose of a state segment, stored at GRASS_STATE_FRAME_OFFSET.
float3 GrassUnpackFramePose(uint2 raw)
{
  return float3(f16tof32(raw.x & 0xFFFFu), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFFu));
}

uint2 GrassPackFramePose(float3 pose)
{
  return uint2(GrassPackHalf2(pose.x, pose.y), GrassPackHalf2(pose.z, 0.0));
}

// Keeps a segment end on the far side of the plane through the segment base perpendicular to the rest axis, at the
// segment length.
float3 GrassConstrain(float3 position, float3 base, float3 axis, float segmentLength)
{
  float3 direction = position - base;
  direction -= axis * min(0.0, dot(direction, axis));
  float directionLength = length(direction);
  direction = directionLength > 1e-6 ? direction / directionLength : axis;
  return base + direction * segmentLength;
}
