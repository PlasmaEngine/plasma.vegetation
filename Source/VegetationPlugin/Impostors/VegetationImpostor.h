#pragma once

#include <Foundation/Math/Vec2.h>
#include <Foundation/Math/Vec3.h>
#include <Foundation/Types/RefCounted.h>
#include <Texture/Image/Image.h>
#include <VegetationPlugin/VegetationDeclarations.h>

class plStreamWriter;
class plStreamReader;

/// \brief How a foliage composite type draws far away. In the middle distances its trees become part of their cell's
/// merged voxel shell, built from the type's voxel proxy; at the horizon each tree is an impostor, a camera-facing quad
/// that shows the merged mesh as baked from the nearest views of a hemi-octahedral grid, with per-pixel depth, normal
/// and translucency. Both are baked from LOD 0's merged mesh when the type is transformed.
struct PL_VEGETATIONPLUGIN_DLL plVegetationImpostorSettings
{
  /// Metres from the camera beyond which trees draw as their impostor. 0 bakes neither impostor nor voxel proxy.
  float m_fDistance = 280.0f;
  plUInt8 m_uiViews = 8;         ///< Views per side of the hemi-octahedral grid (views * views views in all).
  plUInt16 m_uiResolution = 192; ///< Texels per view and side.
  float m_fProxyVoxelSize = 0.25f; ///< Metres at scale 1: the voxel proxy's grid. Cells re-voxelize it coarser.

  void Save(plStreamWriter& inout_stream) const;
  void Load(plStreamReader& inout_stream);

  /// The settings clamped to what the baker supports: 2..16 views, 32..512 texels in steps of 32.
  plVegetationImpostorSettings GetValidated() const;

  bool operator==(const plVegetationImpostorSettings& other) const = default;
};

/// \brief The view grid of an impostor, shared with the shader (Shaders/Vegetation/VegetationImpostorCommon.h).
///
/// Views look at the tree from the upper hemisphere, in the tree's object space (z up). A direction maps to the square
/// [-1, 1]^2 by the hemi-octahedral mapping; view (i, j) of an n x n grid sits at the grid point i / (n - 1),
/// j / (n - 1) of that square, so the outer views look along the horizon.
namespace plVegetationImpostorMath
{
  /// Upper hemisphere direction (z >= 0, clamped) to the square [-1, 1]^2.
  PL_VEGETATIONPLUGIN_DLL plVec2 HemiOctEncode(const plVec3& vDirection);

  /// The square [-1, 1]^2 to a unit direction with z >= 0.
  PL_VEGETATIONPLUGIN_DLL plVec3 HemiOctDecode(const plVec2& vEncoded);

  /// Unit direction from the tree toward the camera of view (i, j).
  PL_VEGETATIONPLUGIN_DLL plVec3 GetViewDirection(plUInt32 i, plUInt32 j, plUInt32 uiViews);

  /// The image axes of a view looking along -vViewDirection: right and up, both perpendicular to it.
  PL_VEGETATIONPLUGIN_DLL void GetViewBasis(const plVec3& vViewDirection, plVec3& out_vRight, plVec3& out_vUp);

  /// Unit normal to the square [-1, 1]^2 (full octahedral mapping) and back.
  PL_VEGETATIONPLUGIN_DLL plVec2 OctEncodeNormal(const plVec3& vNormal);
  PL_VEGETATIONPLUGIN_DLL plVec3 OctDecodeNormal(const plVec2& vEncoded);
} // namespace plVegetationImpostorMath

/// \brief A type's baked impostor: two atlases of views * views tiles, each tile one view of the merged mesh.
///
/// m_Albedo (sRGB): base colour as the materials give it (no instance colour or variation, which the shader adds) and
/// coverage in alpha. Mips keep each tile's coverage under the 0.5 alpha test.
/// m_NormalDepth (linear): object-space normal (octahedral, rg), depth along the view direction (b: 0 = the far side of
/// the bounding sphere, 1 = the near side) and translucency (a: the material's subsurface weight, 0 for bark).
/// Texels nothing covers carry their nearest covered neighbour's values, so filtering never pulls in black.
class PL_VEGETATIONPLUGIN_DLL plVegetationImpostorData : public plRefCounted
{
public:
  plUInt8 m_uiViews = 0;
  plUInt16 m_uiResolution = 0;
  plVec3 m_vCenter = plVec3::MakeZero(); ///< Bounding sphere of the merged mesh, object space at scale 1.
  float m_fRadius = 0.0f;
  plUInt8 m_uiMaterialSlot = 0; ///< Material slot whose lighting parameters the impostor shades with (largest area).
  float m_fVoxelError = 0.0f;   ///< Object-space error of the merged mesh's first voxel level; 0 when it has none.
  plImage m_Albedo;
  plImage m_NormalDepth;

  bool IsValid() const { return m_uiViews >= 2 && m_fRadius > 0.0f && m_Albedo.IsValid() && m_NormalDepth.IsValid(); }

  /// Mip levels both atlases carry: down to tiles of 4 texels.
  static plUInt32 ComputeMipCount(plUInt32 uiResolution);

  void Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

/// \brief Geometry to bake an impostor from: the merged mesh's triangles in object space, with a material slot each.
struct PL_VEGETATIONPLUGIN_DLL plVegetationImpostorBakeMesh
{
  struct Slot
  {
    plColor m_BaseColor = plColor(0.8f, 0.8f, 0.8f); ///< Linear base colour times base weight.
    float m_fTranslucency = 0.0f;                     ///< subsurface_weight of the slot's material.
  };

  plDynamicArray<plVec3> m_Positions;
  plDynamicArray<plVec3> m_Normals; ///< Per vertex; empty for face normals.
  plDynamicArray<plUInt32> m_Indices;
  plDynamicArray<plUInt8> m_TriangleSlots; ///< Per triangle, index into m_Slots.
  plHybridArray<Slot, 4> m_Slots;
};

/// \brief A type's merged mesh as a sparse grid of voxels in object space at scale 1: what each voxel holds of the
/// surface. Cells splat the proxies of their trees into one grid of their own (plVegetationCellShellBuilder).
class PL_VEGETATIONPLUGIN_DLL plVegetationVoxelProxy : public plRefCounted
{
public:
  struct Voxel
  {
    PL_DECLARE_POD_TYPE();

    plVec3 m_vCenter;     ///< Object space.
    float m_fArea;        ///< Surface area inside the voxel, square metres at scale 1.
    plVec3 m_vColor;      ///< Linear base colour, area weighted.
    float m_fTranslucency; ///< Subsurface weight, area weighted.
    plVec3 m_vNormal;     ///< Unit, turned away from the trunk axis (leaves are two-sided).
    float m_fPadding;
  };

  float m_fVoxelSize = 0.0f;
  plDynamicArray<Voxel> m_Voxels;

  bool IsValid() const { return m_fVoxelSize > 0.0f && !m_Voxels.IsEmpty(); }

  void Save(plStreamWriter& inout_stream) const;
  plResult Load(plStreamReader& inout_stream);
};

/// \brief Renders an impostor on the CPU: each view orthographically, at twice the resolution, depth-tested, then
/// averaged to coverage, colour, normal and depth per texel.
class PL_VEGETATIONPLUGIN_DLL plVegetationImpostorBaker
{
public:
  /// Bakes out_data from the mesh. With bCompress the atlases are BC7 (when the image conversion supports it), else
  /// RGBA8. Views bake in parallel.
  static plResult Bake(const plVegetationImpostorBakeMesh& mesh, const plVegetationImpostorSettings& settings, bool bCompress, plVegetationImpostorData& out_data);

  /// Fills out_proxy from the mesh: every triangle is sampled at a quarter voxel spacing and its area, colour, normal and
  /// translucency go to the voxel each sample falls in.
  static plResult BakeVoxelProxy(const plVegetationImpostorBakeMesh& mesh, float fVoxelSize, plVegetationVoxelProxy& out_proxy);
};
