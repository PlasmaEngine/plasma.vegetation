#pragma once

// View grid of a vegetation impostor, mirrored by plVegetationImpostorMath (VegetationImpostor.h). Directions are in the
// tree's object space, z up, from the tree toward the viewer; the hemi-octahedral mapping folds the upper hemisphere into
// [-1, 1]^2, and view (i, j) of an n x n grid sits at the grid point (i, j) / (n - 1) of that square.

float2 ImpostorHemiOctEncode(float3 d)
{
  d.z = max(d.z, 0.0);
  float3 p = d / max(abs(d.x) + abs(d.y) + d.z, 1.0e-6);
  return float2(p.x + p.y, p.x - p.y);
}

float3 ImpostorHemiOctDecode(float2 e)
{
  float2 p = float2(e.x + e.y, e.x - e.y) * 0.5;
  float z = max(1.0 - abs(p.x) - abs(p.y), 0.0);
  return normalize(float3(p, z));
}

float3 ImpostorViewDirection(float2 grid, float views)
{
  return ImpostorHemiOctDecode(grid * (2.0 / (views - 1.0)) - 1.0);
}

// Right and up of the view image; up is world up projected into the view plane, +y for the view straight down.
void ImpostorViewBasis(float3 w, out float3 right, out float3 up)
{
  float3 reference = abs(w.z) > 0.999 ? float3(0, 1, 0) : float3(0, 0, 1);
  right = normalize(cross(reference, w));
  up = cross(w, right);
}

float3 ImpostorOctDecodeNormal(float2 e)
{
  float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
  float t = saturate(-n.z);
  n.x += n.x >= 0.0 ? -t : t;
  n.y += n.y >= 0.0 ? -t : t;
  return normalize(n);
}

// The three views around a direction and their weights: the grid cell the direction falls in, split into two triangles.
void ImpostorSelectViews(float3 localView, float views, out float2 grid[3], out float weights[3])
{
  float2 f = (ImpostorHemiOctEncode(localView) * 0.5 + 0.5) * (views - 1.0);
  float2 cell = clamp(floor(f), 0.0, views - 2.0);
  float2 fr = saturate(f - cell);

  if (fr.x + fr.y < 1.0)
  {
    grid[0] = cell;
    grid[1] = cell + float2(1, 0);
    grid[2] = cell + float2(0, 1);
    weights[0] = 1.0 - fr.x - fr.y;
    weights[1] = fr.x;
    weights[2] = fr.y;
  }
  else
  {
    grid[0] = cell + float2(1, 1);
    grid[1] = cell + float2(0, 1);
    grid[2] = cell + float2(1, 0);
    weights[0] = fr.x + fr.y - 1.0;
    weights[1] = 1.0 - fr.x;
    weights[2] = 1.0 - fr.y;
  }
}
