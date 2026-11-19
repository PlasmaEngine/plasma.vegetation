#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>

namespace
{
  template <typename T>
  void WriteToBuffer(const T& msg, plDataBuffer& out_buffer)
  {
    plContiguousMemoryStreamStorage storage;
    plMemoryStreamWriter writer(&storage);
    msg.Write(writer);

    out_buffer.SetCountUninitialized(storage.GetStorageSize32());
    if (storage.GetStorageSize32() > 0)
    {
      plMemoryUtils::Copy(out_buffer.GetData(), storage.GetData(), storage.GetStorageSize32());
    }
  }

  template <typename T>
  plResult ReadFromBuffer(T& msg, const plDataBuffer& buffer)
  {
    if (buffer.IsEmpty())
      return PL_FAILURE;

    plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
    return msg.Read(reader);
  }
} // namespace

void plVegetationStrokeRequest::Write(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 5;
  inout_stream << uiVersion;
  inout_stream << m_uiMode;
  inout_stream << m_LayerGuid;
  inout_stream << m_uiStrokeId;
  inout_stream << m_uiSegment;
  inout_stream << m_vCenter;
  inout_stream << m_vNormal;
  inout_stream << m_fRadius;
  inout_stream << m_fDensity;
  inout_stream << m_fFalloff;
  inout_stream << m_fStrength;
  inout_stream << m_fAlignToNormal;
  inout_stream << m_MaxSlope;
  inout_stream << m_fMinScale;
  inout_stream << m_fMaxScale;
  inout_stream << m_fSink;
  inout_stream << m_fMinSpacing;
  inout_stream << m_uiSeed;
  inout_stream << m_bPreview;
  inout_stream << m_bStrokePreview;
  inout_stream << m_bMirrorOnly;
  inout_stream.WriteArray(m_Types).IgnoreResult();
  inout_stream.WriteArray(m_Weights).IgnoreResult();
  inout_stream.WriteArray(m_GrassTypes).IgnoreResult();
  inout_stream << m_fLength;
  inout_stream << m_fTexelSize;
  inout_stream << m_LayerTransform;

  // version 5
  inout_stream << m_bUseTypeRules;
  inout_stream << m_vFillMin;
  inout_stream << m_vFillMax;
  inout_stream << m_fTreesPerHectare;
  inout_stream << m_bRemoveClearings;
}

plResult plVegetationStrokeRequest::Read(plStreamReader& inout_stream)
{
  // Editor and engine plugin ship together; version 4 requests still read, newer or older ones are ignored.
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion < 4 || uiVersion > 5)
    return PL_FAILURE;

  inout_stream >> m_uiMode;
  inout_stream >> m_LayerGuid;
  inout_stream >> m_uiStrokeId;
  inout_stream >> m_uiSegment;
  inout_stream >> m_vCenter;
  inout_stream >> m_vNormal;
  inout_stream >> m_fRadius;
  inout_stream >> m_fDensity;
  inout_stream >> m_fFalloff;
  inout_stream >> m_fStrength;
  inout_stream >> m_fAlignToNormal;
  inout_stream >> m_MaxSlope;
  inout_stream >> m_fMinScale;
  inout_stream >> m_fMaxScale;
  inout_stream >> m_fSink;
  inout_stream >> m_fMinSpacing;
  inout_stream >> m_uiSeed;
  inout_stream >> m_bPreview;
  inout_stream >> m_bStrokePreview;
  inout_stream >> m_bMirrorOnly;
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Types));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_Weights));
  PL_SUCCEED_OR_RETURN(inout_stream.ReadArray(m_GrassTypes));
  inout_stream >> m_fLength;
  inout_stream >> m_fTexelSize;
  inout_stream >> m_LayerTransform;

  // A version 4 request placed with the brush values only.
  m_bUseTypeRules = false;
  m_vFillMin.SetZero();
  m_vFillMax.SetZero();
  m_fTreesPerHectare = 600.0f;
  m_bRemoveClearings = false;

  if (uiVersion >= 5)
  {
    inout_stream >> m_bUseTypeRules;
    inout_stream >> m_vFillMin;
    inout_stream >> m_vFillMax;
    inout_stream >> m_fTreesPerHectare;
    inout_stream >> m_bRemoveClearings;
  }

  if (m_uiMode > plVegetationBrushMode::Clearing)
    return PL_FAILURE;

  return PL_SUCCESS;
}

bool plVegetationStrokeRequest::ApplyClearingStamp(plVegetationLayerData& inout_data) const
{
  // Clearings are discs in the layer's xy plane; the world-space brush disc is an ellipse there under a non-uniform
  // scale, measured with the per-axis scale like Erase.
  const plTransform inv = m_LayerTransform.GetInverse();
  const plVec3 vLocalCenter = inv.TransformPosition(m_vCenter);
  const plVec2 vCenter = vLocalCenter.GetAsVec2();
  const plVec2 vAxisScale = m_LayerTransform.m_vScale.Abs().GetAsVec2().CompMax(plVec2(0.0001f));
  const float fRadius = plMath::Max(m_fRadius, 0.05f);

  if (m_bRemoveClearings)
    return inout_data.RemoveClearings(vCenter, fRadius, vAxisScale) > 0;

  bool bChanged = inout_data.AddClearing(vCenter, fRadius / (0.5f * (vAxisScale.x + vAxisScale.y)));

  const float fRadiusSq = fRadius * fRadius;
  for (auto& entry : inout_data.m_Entries)
  {
    for (plUInt32 i = entry.m_Instances.GetCount(); i > 0; --i)
    {
      if ((entry.m_Instances[i - 1].m_vPosition.GetAsVec2() - vCenter).CompMul(vAxisScale).GetLengthSquared() <= fRadiusSq)
      {
        entry.m_Instances.RemoveAtAndSwap(i - 1);
        bChanged = true;
      }
    }
  }

  inout_data.Compact();
  return bChanged;
}

void plVegetationStrokeRequest::ToBuffer(plDataBuffer& out_buffer) const
{
  WriteToBuffer(*this, out_buffer);
}

plResult plVegetationStrokeRequest::FromBuffer(const plDataBuffer& buffer)
{
  return ReadFromBuffer(*this, buffer);
}

void plVegetationStrokeResult::Write(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 2;
  inout_stream << uiVersion;
  inout_stream << m_LayerGuid;
  inout_stream << m_uiStrokeId;
  inout_stream << m_uiSegment;
  m_Added.Serialize(inout_stream);
  m_GrassDelta.Serialize(inout_stream);
}

plResult plVegetationStrokeResult::Read(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 2)
    return PL_FAILURE;

  inout_stream >> m_LayerGuid;
  inout_stream >> m_uiStrokeId;
  inout_stream >> m_uiSegment;
  PL_SUCCEED_OR_RETURN(m_Added.Deserialize(inout_stream));
  return m_GrassDelta.Deserialize(inout_stream);
}

void plVegetationStrokeResult::ToBuffer(plDataBuffer& out_buffer) const
{
  WriteToBuffer(*this, out_buffer);
}

plResult plVegetationStrokeResult::FromBuffer(const plDataBuffer& buffer)
{
  return ReadFromBuffer(*this, buffer);
}

void plVegetationResnapRequest::Write(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_LayerGuid;
  inout_stream << m_uiRequestId;
  inout_stream << m_LayerTransform;
  inout_stream.WriteArray(m_Coverage).IgnoreResult();
}

plResult plVegetationResnapRequest::Read(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return PL_FAILURE;

  inout_stream >> m_LayerGuid;
  inout_stream >> m_uiRequestId;
  inout_stream >> m_LayerTransform;
  return inout_stream.ReadArray(m_Coverage);
}

void plVegetationResnapRequest::ToBuffer(plDataBuffer& out_buffer) const
{
  WriteToBuffer(*this, out_buffer);
}

plResult plVegetationResnapRequest::FromBuffer(const plDataBuffer& buffer)
{
  return ReadFromBuffer(*this, buffer);
}

void plVegetationResnapResult::Write(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;
  inout_stream << m_LayerGuid;
  inout_stream << m_uiRequestId;
  inout_stream << m_uiResnappedTexels;
  inout_stream.WriteArray(m_Coverage).IgnoreResult();
}

plResult plVegetationResnapResult::Read(plStreamReader& inout_stream)
{
  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return PL_FAILURE;

  inout_stream >> m_LayerGuid;
  inout_stream >> m_uiRequestId;
  inout_stream >> m_uiResnappedTexels;
  return inout_stream.ReadArray(m_Coverage);
}

void plVegetationResnapResult::ToBuffer(plDataBuffer& out_buffer) const
{
  WriteToBuffer(*this, out_buffer);
}

plResult plVegetationResnapResult::FromBuffer(const plDataBuffer& buffer)
{
  return ReadFromBuffer(*this, buffer);
}
