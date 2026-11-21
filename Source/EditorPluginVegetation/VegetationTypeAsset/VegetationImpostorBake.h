#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Status.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

/// \brief Bakes a vegetation type's impostor at transform time, from the transformed outputs of the assets it names.
///
/// LOD 0 must be a foliage composite: its merged mesh (the composite's main output) is read with its finest triangles
/// and drawn by plVegetationImpostorBaker. Each material slot's base colour, base weight and subsurface weight come from
/// the slot's transformed material (the type's override, else the mesh material), following its base materials.
struct PL_EDITORPLUGINVEGETATION_DLL plVegetationImpostorEditorBake
{
  /// Fills the impostor and the voxel proxy cells build their merged shells from, or fails with the reason. A type that
  /// is not a foliage composite, or has impostor distance 0, fails with an empty message: it simply has neither.
  static plStatus Bake(const plVegetationTypeResourceDescriptor& desc, plVegetationImpostorData& out_data, plVegetationVoxelProxy& out_proxy);

  /// The object-space error of the first coarse level of the mesh that is made of voxel cubes (all triangles axis
  /// aligned), 0 when there is none. Where it applies on screen is where trees switch to voxels today.
  static float MeasureVoxelError(const class plMeshResourceDescriptor& mesh);
};
