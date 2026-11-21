#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Containers/HashTable.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/Stream.h>
#include <Foundation/Threading/TaskSystem.h>
#include <Texture/Image/Formats/DdsFileFormat.h>
#include <Texture/Image/ImageConversion.h>
#include <VegetationPlugin/Impostors/VegetationImpostor.h>

//////////////////////////////////////////////////////////////////////////
// Settings

void plVegetationImpostorSettings::Save(plStreamWriter& inout_stream) const
{
  inout_stream << m_fDistance;
  inout_stream << m_uiViews;
  inout_stream << m_uiResolution;
  inout_stream << m_fProxyVoxelSize;
}

void plVegetationImpostorSettings::Load(plStreamReader& inout_stream)
{
  inout_stream >> m_fDistance;
  inout_stream >> m_uiViews;
  inout_stream >> m_uiResolution;
  inout_stream >> m_fProxyVoxelSize;
}

plVegetationImpostorSettings plVegetationImpostorSettings::GetValidated() const
{
  plVegetationImpostorSettings s = *this;
  s.m_fDistance = plMath::Max(s.m_fDistance, 0.0f);
  s.m_uiViews = plMath::Clamp<plUInt8>(s.m_uiViews, 2, 16);
  s.m_uiResolution = plMath::Clamp<plUInt16>(static_cast<plUInt16>((s.m_uiResolution + 16) / 32 * 32), 32, 512);
  s.m_fProxyVoxelSize = plMath::Clamp(s.m_fProxyVoxelSize, 0.05f, 2.0f);
  return s;
}

//////////////////////////////////////////////////////////////////////////
// Voxel proxy

void plVegetationVoxelProxy::Save(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_fVoxelSize;
  inout_stream << m_Voxels.GetCount();
  inout_stream.WriteBytes(m_Voxels.GetData(), m_Voxels.GetCount() * sizeof(Voxel)).IgnoreResult();
}

plResult plVegetationVoxelProxy::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return PL_FAILURE;

  plUInt32 uiCount = 0;
  inout_stream >> m_fVoxelSize;
  inout_stream >> uiCount;
  if (uiCount > 16u * 1024u * 1024u)
    return PL_FAILURE;

  m_Voxels.SetCountUninitialized(uiCount);
  if (inout_stream.ReadBytes(m_Voxels.GetData(), uiCount * sizeof(Voxel)) != uiCount * sizeof(Voxel))
    return PL_FAILURE;
  return PL_SUCCESS;
}

plResult plVegetationImpostorBaker::BakeVoxelProxy(const plVegetationImpostorBakeMesh& mesh, float fVoxelSize, plVegetationVoxelProxy& out_proxy)
{
  out_proxy.m_Voxels.Clear();
  out_proxy.m_fVoxelSize = plMath::Max(fVoxelSize, 0.01f);
  if (mesh.m_Positions.IsEmpty() || mesh.m_Indices.GetCount() < 3)
    return PL_FAILURE;

  const float fSize = out_proxy.m_fVoxelSize;
  const float fInvSize = 1.0f / fSize;

  // Normals turn away from the trunk axis: the bounding box centre line.
  plBoundingBox box = plBoundingBox::MakeInvalid();
  for (const plVec3& p : mesh.m_Positions)
    box.ExpandToInclude(p);
  const plVec3 vAxis = box.GetCenter();

  struct Accum
  {
    float m_fArea = 0.0f;
    plVec3 m_vColor = plVec3::MakeZero();
    plVec3 m_vNormal = plVec3::MakeZero();
    float m_fTranslucency = 0.0f;
  };
  plHashTable<plUInt64, Accum> grid;

  auto Key = [&](const plVec3& p) -> plUInt64
  {
    const plInt64 x = static_cast<plInt64>(plMath::Floor(p.x * fInvSize)) + (1 << 20);
    const plInt64 y = static_cast<plInt64>(plMath::Floor(p.y * fInvSize)) + (1 << 20);
    const plInt64 z = static_cast<plInt64>(plMath::Floor(p.z * fInvSize)) + (1 << 20);
    return (static_cast<plUInt64>(x & 0x1FFFFF) << 42) | (static_cast<plUInt64>(y & 0x1FFFFF) << 21) | static_cast<plUInt64>(z & 0x1FFFFF);
  };

  // Samples at about a quarter voxel apart, so a triangle smaller than a voxel still lands in one.
  const float fSpacing = fSize * 0.25f;
  const plUInt32 uiTriangles = mesh.m_Indices.GetCount() / 3;
  for (plUInt32 t = 0; t < uiTriangles; ++t)
  {
    const plVec3& a = mesh.m_Positions[mesh.m_Indices[t * 3 + 0]];
    const plVec3& b = mesh.m_Positions[mesh.m_Indices[t * 3 + 1]];
    const plVec3& c = mesh.m_Positions[mesh.m_Indices[t * 3 + 2]];
    plVec3 n = (b - a).CrossRH(c - a);
    const float fArea = n.GetLength() * 0.5f;
    if (fArea <= 0.0f)
      continue;
    n /= fArea * 2.0f;

    const plUInt8 uiSlot = t < mesh.m_TriangleSlots.GetCount() ? mesh.m_TriangleSlots[t] : 0;
    const plVegetationImpostorBakeMesh::Slot slot = uiSlot < mesh.m_Slots.GetCount() ? mesh.m_Slots[uiSlot] : plVegetationImpostorBakeMesh::Slot();
    const plVec3 vColor(slot.m_BaseColor.r, slot.m_BaseColor.g, slot.m_BaseColor.b);

    const float fEdge = plMath::Max((b - a).GetLength(), (c - a).GetLength(), (c - b).GetLength());
    const plUInt32 uiSteps = plMath::Clamp(static_cast<plUInt32>(fEdge / fSpacing) + 1, 1u, 64u);
    const plUInt32 uiSamples = uiSteps * (uiSteps + 1) / 2;
    const float fSampleArea = fArea / uiSamples;

    for (plUInt32 i = 0; i < uiSteps; ++i)
    {
      for (plUInt32 j = 0; j < uiSteps - i; ++j)
      {
        const float u = (i + 1.0f / 3.0f) / uiSteps;
        const float v = (j + 1.0f / 3.0f) / uiSteps;
        const plVec3 p = a + (b - a) * u + (c - a) * v;

        plVec3 vOutward = p - vAxis;
        vOutward.z *= 0.25f;
        const plVec3 vNormal = n.Dot(vOutward) < 0.0f ? -n : n;

        Accum& acc = grid[Key(p)];
        acc.m_fArea += fSampleArea;
        acc.m_vColor += vColor * fSampleArea;
        acc.m_vNormal += vNormal * fSampleArea;
        acc.m_fTranslucency += slot.m_fTranslucency * fSampleArea;
      }
    }
  }

  out_proxy.m_Voxels.Reserve(grid.GetCount());
  for (auto it = grid.GetIterator(); it.IsValid(); ++it)
  {
    const plUInt64 k = it.Key();
    const Accum& acc = it.Value();
    if (acc.m_fArea <= 0.0f)
      continue;

    plVegetationVoxelProxy::Voxel& v = out_proxy.m_Voxels.ExpandAndGetRef();
    const float x = static_cast<float>(static_cast<plInt64>((k >> 42) & 0x1FFFFF) - (1 << 20));
    const float y = static_cast<float>(static_cast<plInt64>((k >> 21) & 0x1FFFFF) - (1 << 20));
    const float z = static_cast<float>(static_cast<plInt64>(k & 0x1FFFFF) - (1 << 20));
    v.m_vCenter = plVec3(x + 0.5f, y + 0.5f, z + 0.5f) * fSize;
    v.m_fArea = acc.m_fArea;
    v.m_vColor = acc.m_vColor / acc.m_fArea;
    v.m_fTranslucency = acc.m_fTranslucency / acc.m_fArea;
    v.m_vNormal = acc.m_vNormal;
    v.m_vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
    v.m_fPadding = 0.0f;
  }

  return out_proxy.m_Voxels.IsEmpty() ? PL_FAILURE : PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Math, mirrored by Shaders/Vegetation/VegetationImpostorCommon.h

plVec2 plVegetationImpostorMath::HemiOctEncode(const plVec3& vDirection)
{
  plVec3 d = vDirection;
  d.z = plMath::Max(d.z, 0.0f);
  const float fSum = plMath::Max(plMath::Abs(d.x) + plMath::Abs(d.y) + d.z, 1.0e-6f);
  const float px = d.x / fSum;
  const float py = d.y / fSum;
  return plVec2(px + py, px - py);
}

plVec3 plVegetationImpostorMath::HemiOctDecode(const plVec2& vEncoded)
{
  const float px = (vEncoded.x + vEncoded.y) * 0.5f;
  const float py = (vEncoded.x - vEncoded.y) * 0.5f;
  const float pz = plMath::Max(1.0f - plMath::Abs(px) - plMath::Abs(py), 0.0f);
  plVec3 d(px, py, pz);
  d.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  return d;
}

plVec3 plVegetationImpostorMath::GetViewDirection(plUInt32 i, plUInt32 j, plUInt32 uiViews)
{
  const float fScale = 2.0f / static_cast<float>(plMath::Max(uiViews, 2u) - 1);
  return HemiOctDecode(plVec2(i * fScale - 1.0f, j * fScale - 1.0f));
}

void plVegetationImpostorMath::GetViewBasis(const plVec3& vViewDirection, plVec3& out_vRight, plVec3& out_vUp)
{
  // Up is world up projected into the view plane; the view straight down uses +y instead.
  const plVec3 vReference = plMath::Abs(vViewDirection.z) > 0.999f ? plVec3(0, 1, 0) : plVec3(0, 0, 1);
  out_vRight = vReference.CrossRH(vViewDirection);
  out_vRight.NormalizeIfNotZero(plVec3(1, 0, 0)).IgnoreResult();
  out_vUp = vViewDirection.CrossRH(out_vRight);
}

plVec2 plVegetationImpostorMath::OctEncodeNormal(const plVec3& vNormal)
{
  const float fSum = plMath::Max(plMath::Abs(vNormal.x) + plMath::Abs(vNormal.y) + plMath::Abs(vNormal.z), 1.0e-6f);
  plVec2 p(vNormal.x / fSum, vNormal.y / fSum);
  if (vNormal.z < 0.0f)
  {
    const plVec2 q = p;
    p.x = (1.0f - plMath::Abs(q.y)) * (q.x >= 0.0f ? 1.0f : -1.0f);
    p.y = (1.0f - plMath::Abs(q.x)) * (q.y >= 0.0f ? 1.0f : -1.0f);
  }
  return p;
}

plVec3 plVegetationImpostorMath::OctDecodeNormal(const plVec2& vEncoded)
{
  plVec3 n(vEncoded.x, vEncoded.y, 1.0f - plMath::Abs(vEncoded.x) - plMath::Abs(vEncoded.y));
  const float t = plMath::Max(-n.z, 0.0f);
  n.x += n.x >= 0.0f ? -t : t;
  n.y += n.y >= 0.0f ? -t : t;
  n.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
  return n;
}

//////////////////////////////////////////////////////////////////////////
// Data

namespace
{
  constexpr plUInt8 s_uiImpostorDataVersion = 1;

  void WriteAtlas(plStreamWriter& inout_stream, const plImage& image)
  {
    plDefaultMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    plDdsFileFormat dds;
    if (dds.WriteImage(writer, image, "dds").Failed())
    {
      inout_stream << static_cast<plUInt32>(0);
      return;
    }

    inout_stream << storage.GetStorageSize32();
    storage.CopyToStream(inout_stream).IgnoreResult();
  }

  plResult ReadAtlas(plStreamReader& inout_stream, plImage& out_image)
  {
    plUInt32 uiSize = 0;
    inout_stream >> uiSize;
    if (uiSize == 0 || uiSize > 512u * 1024u * 1024u)
      return PL_FAILURE;

    plDefaultMemoryStreamStorage storage;
    storage.ReadAll(inout_stream, uiSize);
    if (storage.GetStorageSize32() != uiSize)
      return PL_FAILURE;

    plMemoryStreamReader reader(&storage);
    plDdsFileFormat dds;
    return dds.ReadImage(reader, out_image, "dds");
  }
} // namespace

plUInt32 plVegetationImpostorData::ComputeMipCount(plUInt32 uiResolution)
{
  plUInt32 uiMips = 1;
  while ((uiResolution >> uiMips) >= 4 && ((uiResolution >> uiMips) << uiMips) == uiResolution)
    ++uiMips;
  return uiMips;
}

void plVegetationImpostorData::Save(plStreamWriter& inout_stream) const
{
  inout_stream << s_uiImpostorDataVersion;
  inout_stream << m_uiViews;
  inout_stream << m_uiResolution;
  inout_stream << m_vCenter;
  inout_stream << m_fRadius;
  inout_stream << m_uiMaterialSlot;
  inout_stream << m_fVoxelError;
  WriteAtlas(inout_stream, m_Albedo);
  WriteAtlas(inout_stream, m_NormalDepth);
}

plResult plVegetationImpostorData::Load(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != s_uiImpostorDataVersion)
    return PL_FAILURE;

  inout_stream >> m_uiViews;
  inout_stream >> m_uiResolution;
  inout_stream >> m_vCenter;
  inout_stream >> m_fRadius;
  inout_stream >> m_uiMaterialSlot;
  inout_stream >> m_fVoxelError;
  PL_SUCCEED_OR_RETURN(ReadAtlas(inout_stream, m_Albedo));
  PL_SUCCEED_OR_RETURN(ReadAtlas(inout_stream, m_NormalDepth));
  return PL_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////
// Baker

namespace
{
  /// One texel of a view after the 2x2 resolve, before mips.
  struct BakedTexel
  {
    plVec3 m_vColor = plVec3::MakeZero(); ///< linear
    plVec3 m_vNormal = plVec3(0, 0, 1);   ///< object space, unit
    float m_fDepth = 0.0f;                ///< -1 far side .. 1 near side of the sphere
    float m_fTranslucency = 0.0f;
    float m_fCoverage = 0.0f;
  };

  struct ViewSample
  {
    float m_fDepth = -plMath::MaxValue<float>();
    plUInt32 m_uiTriangle = plInvalidIndex;
    float m_fB1 = 0.0f;
    float m_fB2 = 0.0f;
  };

  /// Rasterizes the mesh orthographically for one view into a (2 * resolution)^2 sample grid and resolves it to
  /// resolution^2 texels.
  void BakeView(const plVegetationImpostorBakeMesh& mesh, const plVec3& vCenter, float fRadius, const plVec3& vView, plUInt32 uiResolution, plDynamicArray<BakedTexel>& out_texels)
  {
    plVec3 vRight, vUp;
    plVegetationImpostorMath::GetViewBasis(vView, vRight, vUp);

    const plUInt32 uiSize = uiResolution * 2;
    const float fHalf = uiSize * 0.5f;
    plDynamicArray<ViewSample> samples;
    samples.SetCount(uiSize * uiSize);

    // Vertex positions in sample space: x right, y down, z toward the viewer (in sphere radii).
    plDynamicArray<plVec3> projected;
    projected.SetCountUninitialized(mesh.m_Positions.GetCount());
    const float fInvRadius = 1.0f / fRadius;
    for (plUInt32 v = 0; v < mesh.m_Positions.GetCount(); ++v)
    {
      const plVec3 d = (mesh.m_Positions[v] - vCenter) * fInvRadius;
      projected[v].Set((d.Dot(vRight) + 1.0f) * fHalf, (1.0f - d.Dot(vUp)) * fHalf, d.Dot(vView));
    }

    const plUInt32 uiTriangles = mesh.m_Indices.GetCount() / 3;
    for (plUInt32 t = 0; t < uiTriangles; ++t)
    {
      const plVec3& a = projected[mesh.m_Indices[t * 3 + 0]];
      const plVec3& b = projected[mesh.m_Indices[t * 3 + 1]];
      const plVec3& c = projected[mesh.m_Indices[t * 3 + 2]];

      const float fArea = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (plMath::Abs(fArea) < 1.0e-12f)
        continue;
      const float fInvArea = 1.0f / fArea;

      // Sample centres at integer + 0.5, both windings (leaves are two-sided).
      const plInt32 iMinX = plMath::Max(static_cast<plInt32>(plMath::Floor(plMath::Min(a.x, b.x, c.x) - 0.5f)), 0);
      const plInt32 iMaxX = plMath::Min(static_cast<plInt32>(plMath::Ceil(plMath::Max(a.x, b.x, c.x) - 0.5f)), static_cast<plInt32>(uiSize) - 1);
      const plInt32 iMinY = plMath::Max(static_cast<plInt32>(plMath::Floor(plMath::Min(a.y, b.y, c.y) - 0.5f)), 0);
      const plInt32 iMaxY = plMath::Min(static_cast<plInt32>(plMath::Ceil(plMath::Max(a.y, b.y, c.y) - 0.5f)), static_cast<plInt32>(uiSize) - 1);

      for (plInt32 y = iMinY; y <= iMaxY; ++y)
      {
        const float py = y + 0.5f;
        for (plInt32 x = iMinX; x <= iMaxX; ++x)
        {
          const float px = x + 0.5f;
          const float w1 = ((px - a.x) * (c.y - a.y) - (py - a.y) * (c.x - a.x)) * fInvArea;
          const float w2 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * fInvArea;
          const float w0 = 1.0f - w1 - w2;
          if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
            continue;

          const float z = w0 * a.z + w1 * b.z + w2 * c.z;
          ViewSample& s = samples[y * uiSize + x];
          if (z > s.m_fDepth)
          {
            s.m_fDepth = z;
            s.m_uiTriangle = t;
            s.m_fB1 = w1;
            s.m_fB2 = w2;
          }
        }
      }
    }

    // Resolve 2x2 samples per texel.
    out_texels.Clear();
    out_texels.SetCount(uiResolution * uiResolution);
    const bool bVertexNormals = mesh.m_Normals.GetCount() == mesh.m_Positions.GetCount();

    for (plUInt32 y = 0; y < uiResolution; ++y)
    {
      for (plUInt32 x = 0; x < uiResolution; ++x)
      {
        BakedTexel& texel = out_texels[y * uiResolution + x];
        plUInt32 uiHits = 0;
        plVec3 vColor = plVec3::MakeZero();
        plVec3 vNormal = plVec3::MakeZero();
        float fDepth = 0.0f;
        float fTranslucency = 0.0f;

        for (plUInt32 s = 0; s < 4; ++s)
        {
          const ViewSample& sample = samples[(y * 2 + (s >> 1)) * uiSize + x * 2 + (s & 1)];
          if (sample.m_uiTriangle == plInvalidIndex)
            continue;

          const plUInt32 t = sample.m_uiTriangle;
          const plUInt32 i0 = mesh.m_Indices[t * 3 + 0];
          const plUInt32 i1 = mesh.m_Indices[t * 3 + 1];
          const plUInt32 i2 = mesh.m_Indices[t * 3 + 2];

          plVec3 n;
          if (bVertexNormals)
            n = mesh.m_Normals[i0] * (1.0f - sample.m_fB1 - sample.m_fB2) + mesh.m_Normals[i1] * sample.m_fB1 + mesh.m_Normals[i2] * sample.m_fB2;
          else
            n = (mesh.m_Positions[i1] - mesh.m_Positions[i0]).CrossRH(mesh.m_Positions[i2] - mesh.m_Positions[i0]);
          n.NormalizeIfNotZero(vView).IgnoreResult();
          // Two-sided: the side the view sees.
          if (n.Dot(vView) < 0.0f)
            n = -n;

          const plUInt8 uiSlot = t < mesh.m_TriangleSlots.GetCount() ? mesh.m_TriangleSlots[t] : 0;
          const plVegetationImpostorBakeMesh::Slot slot = uiSlot < mesh.m_Slots.GetCount() ? mesh.m_Slots[uiSlot] : plVegetationImpostorBakeMesh::Slot();

          vColor += plVec3(slot.m_BaseColor.r, slot.m_BaseColor.g, slot.m_BaseColor.b);
          vNormal += n;
          fDepth += plMath::Clamp(sample.m_fDepth, -1.0f, 1.0f);
          fTranslucency += slot.m_fTranslucency;
          ++uiHits;
        }

        if (uiHits == 0)
          continue;

        const float fInv = 1.0f / uiHits;
        texel.m_vColor = vColor * fInv;
        vNormal.NormalizeIfNotZero(vView).IgnoreResult();
        texel.m_vNormal = vNormal;
        texel.m_fDepth = fDepth * fInv;
        texel.m_fTranslucency = fTranslucency * fInv;
        texel.m_fCoverage = uiHits * 0.25f;
      }
    }

    // Empty texels take their nearest covered neighbour's values (coverage stays 0), so bilinear filtering and mips
    // never blend in black or a depth from behind the sphere.
    plDynamicArray<plUInt8> filled;
    filled.SetCount(uiResolution * uiResolution);
    for (plUInt32 i = 0; i < out_texels.GetCount(); ++i)
      filled[i] = out_texels[i].m_fCoverage > 0.0f ? 1 : 0;

    for (plUInt32 uiPass = 0; uiPass < 8; ++uiPass)
    {
      plDynamicArray<plUInt8> next = filled;
      bool bChanged = false;
      for (plUInt32 y = 0; y < uiResolution; ++y)
      {
        for (plUInt32 x = 0; x < uiResolution; ++x)
        {
          const plUInt32 i = y * uiResolution + x;
          if (filled[i])
            continue;

          plVec3 vColor = plVec3::MakeZero(), vNormal = plVec3::MakeZero();
          float fDepth = 0.0f, fTranslucency = 0.0f;
          plUInt32 uiCount = 0;
          for (plInt32 dy = -1; dy <= 1; ++dy)
          {
            for (plInt32 dx = -1; dx <= 1; ++dx)
            {
              const plInt32 nx = static_cast<plInt32>(x) + dx, ny = static_cast<plInt32>(y) + dy;
              if (nx < 0 || ny < 0 || nx >= static_cast<plInt32>(uiResolution) || ny >= static_cast<plInt32>(uiResolution))
                continue;
              const plUInt32 n = ny * uiResolution + nx;
              if (!filled[n])
                continue;
              vColor += out_texels[n].m_vColor;
              vNormal += out_texels[n].m_vNormal;
              fDepth += out_texels[n].m_fDepth;
              fTranslucency += out_texels[n].m_fTranslucency;
              ++uiCount;
            }
          }

          if (uiCount == 0)
            continue;

          const float fInv = 1.0f / uiCount;
          out_texels[i].m_vColor = vColor * fInv;
          vNormal.NormalizeIfNotZero(vView).IgnoreResult();
          out_texels[i].m_vNormal = vNormal;
          out_texels[i].m_fDepth = fDepth * fInv;
          out_texels[i].m_fTranslucency = fTranslucency * fInv;
          next[i] = 1;
          bChanged = true;
        }
      }
      filled = next;
      if (!bChanged)
        break;
    }

    // Texels nothing reaches keep the far side of the sphere and face the view.
    for (plUInt32 i = 0; i < out_texels.GetCount(); ++i)
    {
      if (!filled[i])
      {
        out_texels[i].m_vNormal = vView;
        out_texels[i].m_fDepth = -1.0f;
      }
    }
  }

  plUInt8 ToUnorm8(float f)
  {
    return static_cast<plUInt8>(plMath::Clamp(f * 255.0f + 0.5f, 0.0f, 255.0f));
  }

  /// Box-filters a tile from the level above; coverage is rescaled so the share of texels passing the 0.5 alpha test
  /// stays the coverage of the full-resolution tile.
  void DownsampleTile(const plDynamicArray<BakedTexel>& source, plUInt32 uiSourceSize, float fTargetCoverage, plDynamicArray<BakedTexel>& out_target)
  {
    const plUInt32 uiSize = uiSourceSize / 2;
    out_target.Clear();
    out_target.SetCount(uiSize * uiSize);

    for (plUInt32 y = 0; y < uiSize; ++y)
    {
      for (plUInt32 x = 0; x < uiSize; ++x)
      {
        BakedTexel& t = out_target[y * uiSize + x];
        plVec3 vColor = plVec3::MakeZero(), vNormal = plVec3::MakeZero();
        float fDepth = 0.0f, fTranslucency = 0.0f, fCoverage = 0.0f, fWeight = 0.0f;
        for (plUInt32 s = 0; s < 4; ++s)
        {
          const BakedTexel& src = source[(y * 2 + (s >> 1)) * uiSourceSize + x * 2 + (s & 1)];
          // Covered texels dominate the colour; empty ones still carry their dilated values.
          const float w = src.m_fCoverage + 0.01f;
          vColor += src.m_vColor * w;
          vNormal += src.m_vNormal * w;
          fDepth += src.m_fDepth * w;
          fTranslucency += src.m_fTranslucency * w;
          fWeight += w;
          fCoverage += src.m_fCoverage;
        }

        const float fInv = 1.0f / fWeight;
        t.m_vColor = vColor * fInv;
        vNormal.NormalizeIfNotZero(plVec3(0, 0, 1)).IgnoreResult();
        t.m_vNormal = vNormal;
        t.m_fDepth = fDepth * fInv;
        t.m_fTranslucency = fTranslucency * fInv;
        t.m_fCoverage = fCoverage * 0.25f;
      }
    }

    // Find the scale that keeps the alpha-tested share at the target coverage.
    auto PassingShare = [&](float fScale)
    {
      plUInt32 uiPass = 0;
      for (const BakedTexel& t : out_target)
        uiPass += t.m_fCoverage * fScale >= 0.5f ? 1 : 0;
      return static_cast<float>(uiPass) / out_target.GetCount();
    };

    float fLow = 0.0f, fHigh = 4.0f;
    for (plUInt32 i = 0; i < 12; ++i)
    {
      const float fMid = (fLow + fHigh) * 0.5f;
      if (PassingShare(fMid) < fTargetCoverage)
        fLow = fMid;
      else
        fHigh = fMid;
    }

    for (BakedTexel& t : out_target)
      t.m_fCoverage = plMath::Clamp(t.m_fCoverage * fHigh, 0.0f, 1.0f);
  }
} // namespace

plResult plVegetationImpostorBaker::Bake(const plVegetationImpostorBakeMesh& mesh, const plVegetationImpostorSettings& settings, bool bCompress, plVegetationImpostorData& out_data)
{
  const plVegetationImpostorSettings s = settings.GetValidated();
  if (mesh.m_Positions.IsEmpty() || mesh.m_Indices.GetCount() < 3)
    return PL_FAILURE;

  // Bounding sphere: centre of the box, radius to the farthest vertex.
  plBoundingBox box = plBoundingBox::MakeInvalid();
  for (const plVec3& p : mesh.m_Positions)
    box.ExpandToInclude(p);
  const plVec3 vCenter = box.GetCenter();
  float fRadiusSq = 0.0f;
  for (const plVec3& p : mesh.m_Positions)
    fRadiusSq = plMath::Max(fRadiusSq, (p - vCenter).GetLengthSquared());
  const float fRadius = plMath::Sqrt(fRadiusSq) * 1.01f;
  if (fRadius <= 0.0f)
    return PL_FAILURE;

  const plUInt32 uiViews = s.m_uiViews;
  const plUInt32 uiRes = s.m_uiResolution;
  const plUInt32 uiMips = plVegetationImpostorData::ComputeMipCount(uiRes);
  const plUInt32 uiAtlas = uiViews * uiRes;

  // Slot with the most area: its material shades the impostor.
  {
    plHybridArray<double, 8> area;
    area.SetCount(plMath::Max(mesh.m_Slots.GetCount(), 1u));
    for (plUInt32 t = 0; t < mesh.m_Indices.GetCount() / 3; ++t)
    {
      const plUInt8 uiSlot = t < mesh.m_TriangleSlots.GetCount() ? mesh.m_TriangleSlots[t] : 0;
      if (uiSlot >= area.GetCount())
        continue;
      const plVec3& a = mesh.m_Positions[mesh.m_Indices[t * 3 + 0]];
      const plVec3& b = mesh.m_Positions[mesh.m_Indices[t * 3 + 1]];
      const plVec3& c = mesh.m_Positions[mesh.m_Indices[t * 3 + 2]];
      area[uiSlot] += (b - a).CrossRH(c - a).GetLength();
    }
    out_data.m_uiMaterialSlot = 0;
    for (plUInt32 i = 1; i < area.GetCount(); ++i)
    {
      if (area[i] > area[out_data.m_uiMaterialSlot])
        out_data.m_uiMaterialSlot = static_cast<plUInt8>(i);
    }
  }

  plImageHeader header;
  header.SetWidth(uiAtlas);
  header.SetHeight(uiAtlas);
  header.SetNumMipLevels(uiMips);
  header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM_SRGB);
  plImage albedo;
  albedo.ResetAndAlloc(header);
  header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM);
  plImage normalDepth;
  normalDepth.ResetAndAlloc(header);

  // Each view writes its own tile of every mip, so views bake in parallel. The task captures one pointer (the delegate
  // stores small captures only).
  struct BakeContext
  {
    const plVegetationImpostorBakeMesh* m_pMesh;
    plVec3 m_vCenter;
    float m_fRadius;
    plUInt32 m_uiViews;
    plUInt32 m_uiRes;
    plUInt32 m_uiMips;
    plImage* m_pAlbedo;
    plImage* m_pNormalDepth;
  };
  const BakeContext context{&mesh, vCenter, fRadius, uiViews, uiRes, uiMips, &albedo, &normalDepth};
  const BakeContext* pContext = &context;

  plTaskSystem::ParallelForIndexed(0u, uiViews * uiViews, [pContext](plUInt32 uiStart, plUInt32 uiEnd)
    {
      const plVegetationImpostorBakeMesh& mesh = *pContext->m_pMesh;
      const plVec3 vCenter = pContext->m_vCenter;
      const float fRadius = pContext->m_fRadius;
      const plUInt32 uiViews = pContext->m_uiViews;
      const plUInt32 uiRes = pContext->m_uiRes;
      const plUInt32 uiMips = pContext->m_uiMips;
      plImage& albedo = *pContext->m_pAlbedo;
      plImage& normalDepth = *pContext->m_pNormalDepth;

      plDynamicArray<BakedTexel> level, next;
      for (plUInt32 v = uiStart; v < uiEnd; ++v)
      {
        const plUInt32 i = v % uiViews;
        const plUInt32 j = v / uiViews;
        const plVec3 vView = plVegetationImpostorMath::GetViewDirection(i, j, uiViews);
        BakeView(mesh, vCenter, fRadius, vView, uiRes, level);

        float fCoverage = 0.0f;
        for (const BakedTexel& t : level)
          fCoverage += t.m_fCoverage >= 0.5f ? 1.0f : 0.0f;
        fCoverage /= level.GetCount();

        for (plUInt32 uiMip = 0; uiMip < uiMips; ++uiMip)
        {
          const plUInt32 uiTile = uiRes >> uiMip;
          if (uiMip > 0)
          {
            DownsampleTile(level, uiTile * 2, fCoverage, next);
            level.Swap(next);
          }

          for (plUInt32 y = 0; y < uiTile; ++y)
          {
            for (plUInt32 x = 0; x < uiTile; ++x)
            {
              const BakedTexel& t = level[y * uiTile + x];
              const plUInt32 ax = i * uiTile + x;
              const plUInt32 ay = j * uiTile + y;

              plUInt8* pA = albedo.GetPixelPointer<plUInt8>(uiMip, 0, 0, ax, ay);
              const plVec3 srgb = plColor::LinearToGamma(t.m_vColor);
              pA[0] = ToUnorm8(srgb.x);
              pA[1] = ToUnorm8(srgb.y);
              pA[2] = ToUnorm8(srgb.z);
              pA[3] = ToUnorm8(t.m_fCoverage);

              plUInt8* pB = normalDepth.GetPixelPointer<plUInt8>(uiMip, 0, 0, ax, ay);
              const plVec2 n = plVegetationImpostorMath::OctEncodeNormal(t.m_vNormal);
              pB[0] = ToUnorm8(n.x * 0.5f + 0.5f);
              pB[1] = ToUnorm8(n.y * 0.5f + 0.5f);
              pB[2] = ToUnorm8(t.m_fDepth * 0.5f + 0.5f);
              pB[3] = ToUnorm8(t.m_fTranslucency);
            }
          }
        }
      }
    });

  out_data.m_uiViews = static_cast<plUInt8>(uiViews);
  out_data.m_uiResolution = static_cast<plUInt16>(uiRes);
  out_data.m_vCenter = vCenter;
  out_data.m_fRadius = fRadius;

  if (bCompress && plImageConversion::IsConvertible(plImageFormat::R8G8B8A8_UNORM_SRGB, plImageFormat::BC7_UNORM_SRGB) && plImageConversion::IsConvertible(plImageFormat::R8G8B8A8_UNORM, plImageFormat::BC7_UNORM))
  {
    if (plImageConversion::Convert(albedo, out_data.m_Albedo, plImageFormat::BC7_UNORM_SRGB).Succeeded() && plImageConversion::Convert(normalDepth, out_data.m_NormalDepth, plImageFormat::BC7_UNORM).Succeeded())
      return PL_SUCCESS;

    plLog::Warning("Impostor atlases could not be compressed; they stay uncompressed.");
  }

  out_data.m_Albedo.ResetAndMove(std::move(albedo));
  out_data.m_NormalDepth.ResetAndMove(std::move(normalDepth));
  return PL_SUCCESS;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Impostors_VegetationImpostor);
