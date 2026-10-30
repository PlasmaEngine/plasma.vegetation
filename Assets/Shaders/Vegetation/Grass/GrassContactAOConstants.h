#pragma once
#include <Shaders/Common/ConstantBufferMacros.h>

BEGIN_PUSH_CONSTANTS(plGrassContactAOConstants)
{
  UINT1(ContactPage);
  FLOAT1(ContactRadius);
  FLOAT1(ContactHeight);
  FLOAT1(ContactStrength);
  FLOAT1(ContactDistance);
  FLOAT3(ContactPadding);
}
END_PUSH_CONSTANTS(plGrassContactAOConstants)
