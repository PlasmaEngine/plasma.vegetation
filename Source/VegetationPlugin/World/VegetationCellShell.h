#pragma once

#include <Foundation/Containers/DynamicArray.h>
#include <Foundation/Math/Color.h>
#include <Foundation/Math/Transform.h>
#include <Foundation/Types/SharedPtr.h>
#include <VegetationPlugin/VegetationDeclarations.h>

class plVegetationVoxelProxy;
class plMeshResourceDescriptor;

/// \brief One tree of a cell as its merged voxel shell takes it in.
struct plVegetationCellShellTree
{
  plTransform m_Transform;
  plColor m_Color = plColor::White; ///< Instance colour (the type's gradient at the tree's colour seed).
  const plVegetationVoxelProxy* m_pProxy = nullptr;
};

/// \brief Tuning of the merged voxel shell.
struct plVegetationCellShellSettings
{
  float m_fVoxelSize = 0.6f;     ///< Metres: the cell's own voxel grid.
  float m_fMinCoverage = 0.05f;  ///< Voxels with less coverage are left out.
  float m_fHideCoverage = 0.7f;  ///< A face is left out when the voxel in front of it covers at least this much.
  float m_fAreaToCoverage = 1.5f; ///< Coverage = 1 - exp(-k * surface area / voxel face area).
};

struct plVegetationCellShellStats
{
  plUInt32 m_uiTrees = 0;
  plUInt32 m_uiVoxels = 0; ///< Voxels with coverage.
  plUInt32 m_uiFaces = 0;  ///< Faces left after the interior ones were removed.
};

/// \brief Builds the merged voxel shell of a vegetation cell: every tree's voxel proxy splatted into one grid at the
/// cell's voxel size, each voxel's coverage from the surface area it gathered, and only the faces that are not hidden
/// behind a well covered neighbour. The trees of a cell overlap, so most of the forest interior disappears.
///
/// The mesh is in the space of vOrigin (the cell object's position). Vertices carry the voxel's colour and coverage
/// (Color0 rgb and a, linear) and translucency (TexCoord0.x); normals blend the gathered surface normal with the face's.
/// Pure CPU work on the given data: runs on a task.
class PL_VEGETATIONPLUGIN_DLL plVegetationCellShellBuilder
{
public:
  static plResult Build(plArrayPtr<const plVegetationCellShellTree> trees, const plVec3& vOrigin, const plVegetationCellShellSettings& settings, plMeshResourceDescriptor& out_mesh, plVegetationCellShellStats* out_pStats = nullptr);
};
