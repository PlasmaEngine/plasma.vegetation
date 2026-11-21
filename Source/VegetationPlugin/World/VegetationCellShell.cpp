#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Graphics/Geometry.h>
#include <Foundation/Containers/HashTable.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>
#include <VegetationPlugin/Impostors/VegetationImpostor.h>
#include <VegetationPlugin/World/VegetationCellShell.h>

namespace
{
  struct ShellVoxel
  {
    float m_fArea = 0.0f;
    plVec3 m_vColor = plVec3::MakeZero();
    plVec3 m_vNormal = plVec3::MakeZero();
    float m_fTranslucency = 0.0f;
    float m_fCoverage = 0.0f;
  };

  constexpr plInt32 c_iKeyBias = 1 << 20;

  plUInt64 MakeKey(plInt32 x, plInt32 y, plInt32 z)
  {
    return (static_cast<plUInt64>((x + c_iKeyBias) & 0x1FFFFF) << 42) | (static_cast<plUInt64>((y + c_iKeyBias) & 0x1FFFFF) << 21) | static_cast<plUInt64>((z + c_iKeyBias) & 0x1FFFFF);
  }

  void SplitKey(plUInt64 uiKey, plInt32& out_x, plInt32& out_y, plInt32& out_z)
  {
    out_x = static_cast<plInt32>((uiKey >> 42) & 0x1FFFFF) - c_iKeyBias;
    out_y = static_cast<plInt32>((uiKey >> 21) & 0x1FFFFF) - c_iKeyBias;
    out_z = static_cast<plInt32>(uiKey & 0x1FFFFF) - c_iKeyBias;
  }
} // namespace

plResult plVegetationCellShellBuilder::Build(plArrayPtr<const plVegetationCellShellTree> trees, const plVec3& vOrigin, const plVegetationCellShellSettings& settings, plMeshResourceDescriptor& out_mesh, plVegetationCellShellStats* out_pStats)
{
  const float fSize = plMath::Max(settings.m_fVoxelSize, 0.05f);
  const float fInvSize = 1.0f / fSize;

  plHashTable<plUInt64, ShellVoxel> grid;
  plUInt32 uiTrees = 0;

  // Splat: every proxy voxel lands in the cell voxel holding its centre, with its area at the tree's scale.
  for (const plVegetationCellShellTree& tree : trees)
  {
    if (tree.m_pProxy == nullptr || !tree.m_pProxy->IsValid())
      continue;
    ++uiTrees;

    const float fScale = tree.m_Transform.m_vScale.x;
    const float fAreaScale = fScale * fScale;
    const plVec3 vTint(tree.m_Color.r, tree.m_Color.g, tree.m_Color.b);

    for (const plVegetationVoxelProxy::Voxel& v : tree.m_pProxy->m_Voxels)
    {
      const plVec3 p = tree.m_Transform.TransformPosition(v.m_vCenter) - vOrigin;
      const plUInt64 uiKey = MakeKey(static_cast<plInt32>(plMath::Floor(p.x * fInvSize)), static_cast<plInt32>(plMath::Floor(p.y * fInvSize)), static_cast<plInt32>(plMath::Floor(p.z * fInvSize)));

      const float fArea = v.m_fArea * fAreaScale;
      ShellVoxel& s = grid[uiKey];
      s.m_fArea += fArea;
      s.m_vColor += v.m_vColor.CompMul(vTint) * fArea;
      s.m_vNormal += tree.m_Transform.m_qRotation * v.m_vNormal * fArea;
      s.m_fTranslucency += v.m_fTranslucency * fArea;
    }
  }

  const float fFaceArea = fSize * fSize;
  plUInt32 uiVoxels = 0;
  for (auto it = grid.GetIterator(); it.IsValid(); ++it)
  {
    ShellVoxel& s = it.Value();
    s.m_fCoverage = 1.0f - plMath::Exp(-settings.m_fAreaToCoverage * s.m_fArea / fFaceArea);
    if (s.m_fCoverage < settings.m_fMinCoverage)
      continue;

    ++uiVoxels;
    s.m_vColor /= s.m_fArea;
    s.m_fTranslucency /= s.m_fArea;
    s.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  }

  // Faces: one quad per side whose neighbour does not hide it.
  static const plInt32 s_Offsets[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

  plGeometry geom;
  plUInt32 uiFaces = 0;
  for (auto it = grid.GetIterator(); it.IsValid(); ++it)
  {
    const ShellVoxel& s = it.Value();
    if (s.m_fCoverage < settings.m_fMinCoverage)
      continue;

    plInt32 x, y, z;
    SplitKey(it.Key(), x, y, z);
    const plVec3 vMin = plVec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) * fSize;

    for (plUInt32 f = 0; f < 6; ++f)
    {
      const ShellVoxel* pNeighbour = grid.GetValue(MakeKey(x + s_Offsets[f][0], y + s_Offsets[f][1], z + s_Offsets[f][2]));
      if (pNeighbour != nullptr && pNeighbour->m_fCoverage >= settings.m_fHideCoverage)
        continue;

      const plVec3 vFace(static_cast<float>(s_Offsets[f][0]), static_cast<float>(s_Offsets[f][1]), static_cast<float>(s_Offsets[f][2]));
      plVec3 vNormal = s.m_vNormal + vFace;
      vNormal.NormalizeIfNotZero(vFace).IgnoreResult();

      // The face's corners, counter-clockwise seen from outside.
      const plUInt32 uiAxis = f / 2;
      const bool bPositive = (f & 1) == 0;
      const plUInt32 u = (uiAxis + 1) % 3, v = (uiAxis + 2) % 3;
      plVec3 corners[4];
      for (plUInt32 c = 0; c < 4; ++c)
      {
        plVec3 p = vMin;
        p.GetData()[uiAxis] += bPositive ? fSize : 0.0f;
        p.GetData()[u] += (c == 1 || c == 2) ? fSize : 0.0f;
        p.GetData()[v] += (c >= 2) ? fSize : 0.0f;
        corners[c] = p;
      }

      const plColor color(s.m_vColor.x, s.m_vColor.y, s.m_vColor.z, s.m_fCoverage);
      plUInt32 idx[4];
      for (plUInt32 c = 0; c < 4; ++c)
        idx[c] = geom.AddVertex(corners[c], vNormal, plVec2(s.m_fTranslucency, 0.0f), color);
      geom.AddPolygon(plArrayPtr<plUInt32>(idx), !bPositive);
      ++uiFaces;
    }
  }

  if (out_pStats != nullptr)
  {
    out_pStats->m_uiTrees = uiTrees;
    out_pStats->m_uiVoxels = uiVoxels;
    out_pStats->m_uiFaces = uiFaces;
  }

  if (uiFaces == 0)
    return PL_FAILURE;

  out_mesh.Clear();
  out_mesh.MeshBufferDesc().AddCommonStreams();
  out_mesh.MeshBufferDesc().AddStream(plMeshVertexStreamType::Color0);
  out_mesh.MeshBufferDesc().AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);
  out_mesh.AddSubMesh(out_mesh.MeshBufferDesc().GetPrimitiveCount(), 0, 0);
  out_mesh.ComputeBounds();
  return PL_SUCCESS;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_World_VegetationCellShell);
