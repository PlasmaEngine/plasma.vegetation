#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/IO/MemoryStream.h>
#include <Foundation/IO/StringDeduplicationContext.h>
#include <Foundation/Math/Random.h>
#include <Foundation/Types/ScopeExit.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>

void plVegetationLayerData::Serialize(plStreamWriter& inout_stream) const
{
  const plUInt8 uiVersion = 1;
  inout_stream << uiVersion;

  inout_stream << m_Entries.GetCount();
  for (const auto& entry : m_Entries)
  {
    inout_stream << entry.m_sType;
    inout_stream << entry.m_Instances.GetCount();

    for (const auto& inst : entry.m_Instances)
    {
      inout_stream << inst.m_vPosition;
      inout_stream << inst.m_qRotation;
      inout_stream << inst.m_fScale;
      inout_stream << inst.m_uiColorSeed;
    }
  }
}

plResult plVegetationLayerData::Deserialize(plStreamReader& inout_stream)
{
  m_Entries.Clear();

  plUInt8 uiVersion = 0;
  inout_stream >> uiVersion;
  if (uiVersion != 1)
    return PL_FAILURE;

  plUInt32 uiEntries = 0;
  inout_stream >> uiEntries;

  for (plUInt32 e = 0; e < uiEntries; ++e)
  {
    auto& entry = m_Entries.ExpandAndGetRef();
    inout_stream >> entry.m_sType;

    plUInt32 uiCount = 0;
    inout_stream >> uiCount;
    entry.m_Instances.SetCountUninitialized(uiCount);

    for (plUInt32 i = 0; i < uiCount; ++i)
    {
      auto& inst = entry.m_Instances[i];
      inout_stream >> inst.m_vPosition;
      inout_stream >> inst.m_qRotation;
      inout_stream >> inst.m_fScale;
      inout_stream >> inst.m_uiColorSeed;
    }
  }

  return PL_SUCCESS;
}

void plVegetationLayerData::ToBuffer(plDataBuffer& out_buffer) const
{
  // This blob owns its strings, independently of a surrounding scene/prefab serialization context.
  auto* pContext = plStringDeduplicationWriteContext::GetContext();
  if (pContext != nullptr)
    pContext->SetActive(false);
  PL_SCOPE_EXIT(if (pContext != nullptr) pContext->SetActive(true));

  plContiguousMemoryStreamStorage storage;
  plMemoryStreamWriter writer(&storage);
  Serialize(writer);

  out_buffer.SetCountUninitialized(storage.GetStorageSize32());
  if (storage.GetStorageSize32() > 0)
  {
    plMemoryUtils::Copy(out_buffer.GetData(), storage.GetData(), storage.GetStorageSize32());
  }
}

plResult plVegetationLayerData::FromBuffer(const plDataBuffer& buffer)
{
  m_Entries.Clear();

  if (buffer.IsEmpty())
    return PL_SUCCESS;

  auto* pContext = plStringDeduplicationReadContext::GetContext();
  if (pContext != nullptr)
    pContext->SetActive(false);
  PL_SCOPE_EXIT(if (pContext != nullptr) pContext->SetActive(true));

  plRawMemoryStreamReader reader(buffer.GetData(), buffer.GetCount());
  return Deserialize(reader);
}

plVegetationLayerEntry& plVegetationLayerData::GetOrAddEntry(plStringView sType)
{
  for (auto& entry : m_Entries)
  {
    if (entry.m_sType == sType)
      return entry;
  }

  auto& entry = m_Entries.ExpandAndGetRef();
  entry.m_sType = sType;
  return entry;
}

const plVegetationLayerEntry* plVegetationLayerData::FindEntry(plStringView sType) const
{
  for (const auto& entry : m_Entries)
  {
    if (entry.m_sType == sType)
      return &entry;
  }

  return nullptr;
}

plUInt32 plVegetationLayerData::GetInstanceCount() const
{
  plUInt32 uiCount = 0;
  for (const auto& entry : m_Entries)
  {
    uiCount += entry.m_Instances.GetCount();
  }
  return uiCount;
}

void plVegetationLayerData::Append(const plVegetationLayerData& other)
{
  for (const auto& otherEntry : other.m_Entries)
  {
    auto& entry = GetOrAddEntry(otherEntry.m_sType);
    entry.m_Instances.PushBackRange(otherEntry.m_Instances);
  }
}

plUInt32 plVegetationLayerData::RemoveInSphere(const plVec3& vCenter, float fRadius, float fFalloff, float fStrength, plArrayPtr<const plString> types, plUInt32 uiSeed, const plVec3& vAxisScale)
{
  plRandom rng;
  rng.Initialize(uiSeed);

  const float fInner = fRadius * (1.0f - plMath::Clamp(fFalloff, 0.0f, 1.0f));
  plUInt32 uiRemoved = 0;

  for (auto& entry : m_Entries)
  {
    if (!types.IsEmpty())
    {
      bool bMatch = false;
      for (const plString& sType : types)
      {
        if (sType == entry.m_sType)
        {
          bMatch = true;
          break;
        }
      }

      if (!bMatch)
        continue;
    }

    for (plUInt32 i = entry.m_Instances.GetCount(); i > 0; --i)
    {
      const float fDistance = (entry.m_Instances[i - 1].m_vPosition - vCenter).CompMul(vAxisScale).GetLength();
      if (fDistance > fRadius)
        continue;

      float fChance = fStrength;
      if (fDistance > fInner && fRadius > fInner)
      {
        fChance *= 1.0f - (fDistance - fInner) / (fRadius - fInner);
      }

      if (rng.FloatZeroToOneExclusive() < fChance)
      {
        entry.m_Instances.RemoveAtAndSwap(i - 1);
        ++uiRemoved;
      }
    }
  }

  Compact();
  return uiRemoved;
}

void plVegetationLayerData::Compact()
{
  for (plUInt32 e = m_Entries.GetCount(); e > 0; --e)
  {
    if (m_Entries[e - 1].m_Instances.IsEmpty())
    {
      m_Entries.RemoveAtAndCopy(e - 1);
    }
  }
}
