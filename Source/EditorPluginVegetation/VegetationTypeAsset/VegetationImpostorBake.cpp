#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationImpostorBake.h>
#include <Foundation/IO/CompressedStreamZstd.h>
#include <Foundation/IO/FileSystem/FileReader.h>
#include <Foundation/Time/Stopwatch.h>
#include <Foundation/Utilities/AssetFileHeader.h>
#include <RendererCore/Meshes/MeshResourceDescriptor.h>

namespace
{
  /// Document type of the engine's Foliage Composite asset; its main output is the merged mesh.
  constexpr plStringView s_sCompositeDocumentType = "Foliage Composite"_plsv;

  /// Where an asset's transform wrote its main output, and its document type. Empty for an unknown asset.
  plString FindAssetOutput(plStringView sAsset, plString* out_pDocumentType = nullptr)
  {
    const plAssetCurator::plLockedSubAsset asset = plAssetCurator::GetSingleton()->FindSubAsset(sAsset);
    if (!asset.isValid() || asset->m_pAssetInfo == nullptr || asset->m_pAssetInfo->m_pDocumentTypeDescriptor == nullptr)
      return plString();

    const plAssetDocumentTypeDescriptor* pType = asset->m_pAssetInfo->m_pDocumentTypeDescriptor;
    if (out_pDocumentType != nullptr)
      *out_pDocumentType = pType->m_sDocumentTypeName;

    const plAssetDocumentManager* pManager = static_cast<const plAssetDocumentManager*>(pType->m_pManager);
    return pManager->GetAbsoluteOutputFileName(pType, asset->m_pAssetInfo->m_Path.GetAbsolutePath(), "");
  }

  struct MaterialValues
  {
    plColor m_BaseColor = plColor(0.8f, 0.8f, 0.8f);
    float m_fBaseWeight = 1.0f;
    float m_fSubsurfaceWeight = 0.0f;
    bool m_bHasBaseColor = false;
    bool m_bHasBaseWeight = false;
    bool m_bHasSubsurfaceWeight = false;
  };

  /// Reads the parameters the bake needs from a transformed material (.plBinMaterial, versions 4-8), then its base
  /// materials for whatever it does not set itself.
  void ReadMaterial(plStringView sMaterial, MaterialValues& inout_values, plUInt32 uiDepth = 0)
  {
    if (sMaterial.IsEmpty() || uiDepth > 8)
      return;

    const plString sFile = FindAssetOutput(sMaterial);
    if (sFile.IsEmpty())
      return;

    plFileReader file;
    if (file.Open(sFile).Failed())
      return;

    plAssetFileHeader header;
    if (header.Read(file).Failed())
      return;

    plUInt8 uiVersion = 0;
    file >> uiVersion;
    if (uiVersion < 4 || uiVersion > 8)
      return;

    plUInt8 uiCompression = 0;
    if (uiVersion >= 6)
      file >> uiCompression;

    plStreamReader* pStream = &file;
    plCompressedStreamReaderZstd zstd;
    if (uiCompression == 1)
    {
      zstd.SetInputStream(&file);
      pStream = &zstd;
    }
    else if (uiCompression != 0)
    {
      return;
    }

    plStreamReader& s = *pStream;
    plStringBuilder sBase, sTemp, sTemp2;
    s >> sBase;
    s >> sTemp; // surface
    s >> sTemp; // shader

    plUInt16 uiCount = 0;
    s >> uiCount; // permutation variables
    for (plUInt16 i = 0; i < uiCount; ++i)
      s >> sTemp >> sTemp2;

    s >> uiCount; // 2D textures
    for (plUInt16 i = 0; i < uiCount; ++i)
      s >> sTemp >> sTemp2;

    s >> uiCount; // cube textures
    for (plUInt16 i = 0; i < uiCount; ++i)
      s >> sTemp >> sTemp2;

    s >> uiCount; // constants
    for (plUInt16 i = 0; i < uiCount; ++i)
    {
      plVariant value;
      s >> sTemp;
      s >> value;

      if (sTemp == "base_color" && !inout_values.m_bHasBaseColor && value.CanConvertTo<plColor>())
      {
        inout_values.m_BaseColor = value.ConvertTo<plColor>();
        inout_values.m_bHasBaseColor = true;
      }
      else if (sTemp == "base_weight" && !inout_values.m_bHasBaseWeight && value.CanConvertTo<float>())
      {
        inout_values.m_fBaseWeight = value.ConvertTo<float>();
        inout_values.m_bHasBaseWeight = true;
      }
      else if (sTemp == "subsurface_weight" && !inout_values.m_bHasSubsurfaceWeight && value.CanConvertTo<float>())
      {
        inout_values.m_fSubsurfaceWeight = value.ConvertTo<float>();
        inout_values.m_bHasSubsurfaceWeight = true;
      }
    }

    // The child's values win; the base fills the rest.
    ReadMaterial(sBase, inout_values, uiDepth + 1);
  }

  /// Whether every triangle of a meshlet faces along an axis: the faces of voxel cubes.
  bool IsVoxelMeshlet(const plMeshResourceDescriptor& mesh, const plMeshResourceDescriptor::Meshlet& meshlet)
  {
    const plMeshBufferResourceDescriptor& mb = mesh.MeshBufferDesc();
    const plArrayPtr<const plVec3> positions = mb.GetPositionData();
    const plArrayPtr<const plUInt8> indexData = mb.GetIndexBufferData();
    const bool b32 = mb.Uses32BitIndices();
    const plUInt32 uiIndexCount = indexData.GetCount() / (b32 ? 4 : 2);

    auto Index = [&](plUInt32 i) -> plUInt32
    { return b32 ? reinterpret_cast<const plUInt32*>(indexData.GetPtr())[i] : reinterpret_cast<const plUInt16*>(indexData.GetPtr())[i]; };

    if (meshlet.m_uiPrimitiveCount == 0)
      return false;

    for (plUInt32 t = meshlet.m_uiFirstPrimitive; t < meshlet.m_uiFirstPrimitive + meshlet.m_uiPrimitiveCount; ++t)
    {
      if (t * 3 + 2 >= uiIndexCount)
        return false;

      const plUInt32 a = Index(t * 3), b = Index(t * 3 + 1), c = Index(t * 3 + 2);
      if (a >= positions.GetCount() || b >= positions.GetCount() || c >= positions.GetCount())
        return false;

      plVec3 n = (positions[b] - positions[a]).CrossRH(positions[c] - positions[a]);
      if (n.NormalizeIfNotZero(plVec3::MakeZero()).Failed())
        continue;

      const float fMax = plMath::Max(plMath::Abs(n.x), plMath::Abs(n.y), plMath::Abs(n.z));
      if (fMax < 0.999f)
        return false;
    }

    return true;
  }
} // namespace

float plVegetationImpostorEditorBake::MeasureVoxelError(const plMeshResourceDescriptor& mesh)
{
  float fError = 0.0f;
  for (const plMeshResourceDescriptor::Meshlet& meshlet : mesh.GetMeshlets())
  {
    if (meshlet.m_uiLodLevel == 0 || (fError > 0.0f && meshlet.m_fLodError >= fError))
      continue;

    if (IsVoxelMeshlet(mesh, meshlet))
      fError = meshlet.m_fLodError;
  }

  return fError;
}

plStatus plVegetationImpostorEditorBake::Bake(const plVegetationTypeResourceDescriptor& desc, plVegetationImpostorData& out_data, plVegetationVoxelProxy& out_proxy)
{
  if (desc.m_Impostor.m_fDistance <= 0.0f || desc.m_Lods.IsEmpty())
    return plStatus(PL_FAILURE, "");

  plString sDocumentType;
  const plString sMeshFile = FindAssetOutput(desc.m_Lods[0].m_sMesh, &sDocumentType);
  if (sMeshFile.IsEmpty() || sDocumentType != s_sCompositeDocumentType)
    return plStatus(PL_FAILURE, "");

  plStopwatch timer;

  plMeshResourceDescriptor mesh;
  if (mesh.Load(sMeshFile).Failed())
    return plStatus(plFmt("The impostor could not be baked: the foliage composite '{}' could not be read. Transform it first.", desc.m_Lods[0].m_sMesh));

  const plMeshBufferResourceDescriptor& mb = mesh.MeshBufferDesc();
  const plArrayPtr<const plVec3> positions = mb.GetPositionData();
  const plArrayPtr<const plUInt8> indexData = mb.GetIndexBufferData();
  const bool b32 = mb.Uses32BitIndices();

  plVegetationImpostorBakeMesh bake;
  bake.m_Positions = positions;
  bake.m_Normals.SetCountUninitialized(mb.GetVertexCount());
  for (plUInt32 v = 0; v < mb.GetVertexCount(); ++v)
    bake.m_Normals[v] = mb.GetNormal(v);

  // The sub-mesh ranges are the finest level; coarser levels follow them in the index buffer and are left out.
  plUInt32 uiMaxSlot = 0;
  for (const plMeshResourceDescriptor::SubMesh& sub : mesh.GetSubMeshes())
  {
    uiMaxSlot = plMath::Max(uiMaxSlot, sub.m_uiMaterialIndex);
    for (plUInt32 t = sub.m_uiFirstPrimitive; t < sub.m_uiFirstPrimitive + sub.m_uiPrimitiveCount; ++t)
    {
      for (plUInt32 k = 0; k < 3; ++k)
      {
        const plUInt32 i = t * 3 + k;
        bake.m_Indices.PushBack(b32 ? reinterpret_cast<const plUInt32*>(indexData.GetPtr())[i] : reinterpret_cast<const plUInt16*>(indexData.GetPtr())[i]);
      }
      bake.m_TriangleSlots.PushBack(static_cast<plUInt8>(plMath::Min(sub.m_uiMaterialIndex, 255u)));
    }
  }

  // Slot colours as the vegetation shader sees them: base colour times base weight; translucency from subsurface weight.
  bake.m_Slots.SetCount(plMath::Min(uiMaxSlot + 1, 256u));
  for (plUInt32 s = 0; s < bake.m_Slots.GetCount(); ++s)
  {
    plString sMaterial;
    if (s < desc.m_Materials.GetCount() && !desc.m_Materials[s].IsEmpty())
      sMaterial = desc.m_Materials[s];
    else if (s < mesh.GetMaterials().GetCount())
      sMaterial = mesh.GetMaterials()[s].m_sPath;

    MaterialValues values;
    ReadMaterial(sMaterial, values);
    bake.m_Slots[s].m_BaseColor = values.m_BaseColor * values.m_fBaseWeight;
    bake.m_Slots[s].m_BaseColor.a = 1.0f;
    bake.m_Slots[s].m_fTranslucency = plMath::Clamp(values.m_fSubsurfaceWeight, 0.0f, 1.0f);
  }

  if (plVegetationImpostorBaker::Bake(bake, desc.m_Impostor, true, out_data).Failed())
    return plStatus(plFmt("The impostor of '{}' could not be baked: the merged mesh has no triangles.", desc.m_Lods[0].m_sMesh));

  out_data.m_fVoxelError = MeasureVoxelError(mesh);

  if (plVegetationImpostorBaker::BakeVoxelProxy(bake, desc.m_Impostor.GetValidated().m_fProxyVoxelSize, out_proxy).Failed())
    return plStatus(plFmt("The voxel proxy of '{}' could not be baked.", desc.m_Lods[0].m_sMesh));

  // Where trees reach the voxel levels today: the first voxel error projected to 1 px on a 1080 px tall 60 degree view.
  const float fVoxelDistance = out_data.m_fVoxelError * 1080.0f / (2.0f * plMath::Tan(plAngle::MakeFromDegree(30.0f)));
  plLog::Info("Baked impostor and voxel proxy: {} triangles, {}x{} views of {} px, radius {} m, {} proxy voxels of {} m, first voxel level error {} m (voxels from about {} m at 1080p, 60 deg, 1 px), {} s.", bake.m_Indices.GetCount() / 3, out_data.m_uiViews, out_data.m_uiViews, out_data.m_uiResolution, plArgF(out_data.m_fRadius, 2), out_proxy.m_Voxels.GetCount(), plArgF(out_proxy.m_fVoxelSize, 2), plArgF(out_data.m_fVoxelError, 3), plArgF(fVoxelDistance, 0), plArgF(timer.GetRunningTotal().GetSeconds(), 2));
  return plStatus(PL_SUCCESS);
}
