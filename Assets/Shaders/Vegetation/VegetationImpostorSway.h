#pragma once

// Rigid sway of a whole impostor tree: the slow oscillation of the trunk bend in Shaders/Vegetation/VegetationWind.h,
// as a translation of the tree at its centre height. Without the steady lean, so a still impostor stands where its
// merged mesh stands. The vertex shader moves the quad with it and the pixel shader the reconstructed tree.

#include <Shaders/Wind/WindField.h>

float3 ImpostorSway(float3 pivot, float3 up, float centerHeight, float bendStiffness, bool bPreviousFrame)
{
  const float3 samplePos = pivot + up * 2.0;
  const float3 wind = bPreviousFrame ? SampleWindFieldPrev(samplePos) : SampleWindField(samplePos);
  const float speed = length(wind);

  [branch]
  if (speed < 0.0001)
    return float3(0, 0, 0);

  const float time = bPreviousFrame ? WindTime - WindDeltaTime : WindTime;
  const float phase = WindHash13(pivot) * 6.2831853;
  const float response = 1.0 - saturate(bendStiffness);
  const float angle = atan(speed * 0.045 * response) * 0.2 * response * sin(time * 0.9 + phase);
  return (wind / speed) * (angle * max(centerHeight, 0.0));
}
