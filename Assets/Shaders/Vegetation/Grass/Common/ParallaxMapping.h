#pragma once

/// Performs steep parallax occlusion mapping with binary-search refinement.
/// White height values represent the surface and black values the deepest point.
float2 ParallaxOcclusionMapping(Texture2D heightTexture, SamplerState textureSampler, float2 texCoords, float3 viewDirTS, float heightScale, int maxSteps)
{
  const int numSteps = (int)lerp((float)maxSteps, (float)max(maxSteps / 4, 4), abs(viewDirTS.z));
  const float layerDepth = 1.0 / (float)numSteps;
  const float2 deltaUV = (viewDirTS.xy / max(viewDirTS.z, 0.001)) * heightScale / (float)numSteps;

  float currentLayerDepth = 0.0;
  float2 currentUV = texCoords;
  float currentHeight = heightTexture.SampleLevel(textureSampler, currentUV, 0).r;

  [loop] for (int i = 0; i < numSteps && currentLayerDepth < currentHeight; ++i)
  {
    currentUV -= deltaUV;
    currentHeight = heightTexture.SampleLevel(textureSampler, currentUV, 0).r;
    currentLayerDepth += layerDepth;
  }

  float2 previousUV = currentUV + deltaUV;
  float previousLayerDepth = currentLayerDepth - layerDepth;

  [unroll] for (int i = 0; i < 4; ++i)
  {
    const float2 middleUV = (previousUV + currentUV) * 0.5;
    const float middleLayerDepth = (previousLayerDepth + currentLayerDepth) * 0.5;
    const float middleHeight = heightTexture.SampleLevel(textureSampler, middleUV, 0).r;

    if (middleHeight > middleLayerDepth)
    {
      previousUV = middleUV;
      previousLayerDepth = middleLayerDepth;
    }
    else
    {
      currentUV = middleUV;
      currentLayerDepth = middleLayerDepth;
    }
  }

  return currentUV;
}
