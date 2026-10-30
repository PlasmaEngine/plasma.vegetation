#pragma once

#include <Core/Interfaces/VegetationWorldModule.h>
#include <Core/ResourceManager/ResourceHandle.h>
#include <VegetationPlugin/VegetationPluginDLL.h>

using plVegetationTypeResourceHandle = plTypedResourceHandle<class plVegetationTypeResource>;
using plGrassTypeResourceHandle = plTypedResourceHandle<class plGrassTypeResource>;

/// \brief Side length of a vegetation cell in metres. Matches the procedural placement tile granularity.
constexpr float plVegetationCellSize = 32.0f;

/// \brief Most LOD meshes one type can carry (plus an impostor slot handled separately).
constexpr plUInt32 plVegetationMaxLods = 4;
