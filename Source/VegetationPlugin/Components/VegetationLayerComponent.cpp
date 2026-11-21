#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Messages/TransformChangedMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// clang-format off
PL_BEGIN_STATIC_REFLECTED_TYPE(plVegetationFillSpecies, plNoBase, 1, plRTTIDefaultAllocator<plVegetationFillSpecies>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Type", m_sType)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Vegetation_Type;CompatibleAsset_Grass_Type")),
    PL_MEMBER_PROPERTY("Weight", m_fWeight)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 100.0f)),
  }
  PL_END_PROPERTIES;
}
PL_END_STATIC_REFLECTED_TYPE;

PL_BEGIN_COMPONENT_TYPE(plVegetationLayerComponent, 2, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Data", GetData, SetData)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY_READ_ONLY("InstanceCount", GetInstanceCount),
    PL_ACCESSOR_PROPERTY("GrassCoverage", GetGrassCoverageBlob, SetGrassCoverageBlob)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY("GrassTexelSize", GetGrassTexelSize, SetGrassTexelSize)->AddAttributes(new plDefaultValueAttribute(plGrassCoverageData::DefaultTexelSize), new plClampValueAttribute(plGrassCoverageData::MinTexelSize, plGrassCoverageData::MaxTexelSize), new plSuffixAttribute(" m")),
    PL_ACCESSOR_PROPERTY_READ_ONLY("GrassTileCount", GetGrassTileCount),
    PL_ARRAY_MEMBER_PROPERTY("FillSpecies", m_FillSpecies)->AddAttributes(new plGroupAttribute("Fill")),
    PL_MEMBER_PROPERTY("FillTreesPerHectare", m_fFillTreesPerHectare)->AddAttributes(new plDefaultValueAttribute(600.0f), new plClampValueAttribute(0.0f, 100000.0f)),
    PL_MEMBER_PROPERTY("FillGrassDensity", m_fFillGrassDensity)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("FillGrassLength", m_fFillGrassLength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("FillGrassMaxSlope", m_FillGrassMaxSlope)->AddAttributes(new plDefaultValueAttribute(plAngle::MakeFromDegree(40.0f)), new plClampValueAttribute(plAngle::MakeFromDegree(0.0f), plAngle::MakeFromDegree(90.0f))),
    PL_MEMBER_PROPERTY("FillCenter", m_vFillCenter),
    PL_MEMBER_PROPERTY("FillExtent", m_vFillExtent)->AddAttributes(new plClampValueAttribute(plVec2(0.0f), plVec2(100000.0f)), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("FillSeed", m_uiFillSeed)->AddAttributes(new plDefaultValueAttribute(1)),
    PL_MEMBER_PROPERTY("FillReplace", m_bFillReplace),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_MESSAGEHANDLERS
  {
    PL_MESSAGE_HANDLER(plMsgTransformChanged, OnMsgTransformChanged),
  }
  PL_END_MESSAGEHANDLERS;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vegetation"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationLayerComponent::plVegetationLayerComponent() = default;
plVegetationLayerComponent::~plVegetationLayerComponent() = default;

void plVegetationLayerComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s.WriteArray(m_Data).IgnoreResult();

  // version 2
  s.WriteArray(m_GrassCoverage).IgnoreResult();
  s << m_fGrassTexelSize;
}

void plVegetationLayerComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  const plUInt32 uiVersion = inout_stream.GetComponentTypeVersion(GetStaticRTTI());
  auto& s = inout_stream.GetStream();

  s.ReadArray(m_Data).IgnoreResult();
  m_Parsed.FromBuffer(m_Data).IgnoreResult();

  m_GrassCoverage.Clear();
  m_fGrassTexelSize = plGrassCoverageData::DefaultTexelSize;

  if (uiVersion >= 2)
  {
    s.ReadArray(m_GrassCoverage).IgnoreResult();
    s >> m_fGrassTexelSize;
    m_fGrassTexelSize = plGrassCoverageData::ClampTexelSize(m_fGrassTexelSize);
  }

  if (m_GrassParsed.FromBuffer(m_GrassCoverage).Failed())
  {
    m_GrassParsed.Clear();
  }
}

void plVegetationLayerComponent::OnActivated()
{
  SUPER::OnActivated();

  GetOwner()->EnableStaticTransformChangesNotifications();
  m_bDirty = true;
}

void plVegetationLayerComponent::Update()
{
  // The world module grows painted grass; it must exist even for layers without instances.
  if (!m_GrassParsed.IsEmpty())
    GetWorld()->GetOrCreateModule<plVegetationWorldModule>();

  if (!m_bDirty)
    return;

  m_bDirty = false;
  Register();
}

void plVegetationLayerComponent::OnDeactivated()
{
  Unregister();

  SUPER::OnDeactivated();
}

void plVegetationLayerComponent::SetData(const plDataBuffer& data)
{
  m_Data = data;
  m_Parsed.FromBuffer(m_Data).IgnoreResult();

  m_bStrokePreview = false;
  m_bDirty = true;
}

void plVegetationLayerComponent::SetGrassCoverageBlob(const plDataBuffer& data)
{
  m_GrassCoverage = data;

  plGrassCoverageData parsed;
  if (parsed.FromBuffer(m_GrassCoverage).Failed())
  {
    parsed = plGrassCoverageData();
  }

  m_GrassParsed.AssignAndTrackChanges(parsed);
  m_bStrokePreview = false;
}

void plVegetationLayerComponent::SetStrokePreview(const plVegetationLayerData& data, const plGrassCoverageData& grass)
{
  m_bStrokePreview = true;

  m_Parsed = data;
  m_GrassParsed.AssignAndTrackChanges(grass);

  m_bDirty = true;
}

void plVegetationLayerComponent::ClearStrokePreview()
{
  if (!m_bStrokePreview)
    return;

  m_bStrokePreview = false;

  if (m_Parsed.FromBuffer(m_Data).Failed())
  {
    m_Parsed = plVegetationLayerData();
  }

  plGrassCoverageData parsed;
  if (parsed.FromBuffer(m_GrassCoverage).Failed())
  {
    parsed = plGrassCoverageData();
  }

  m_GrassParsed.AssignAndTrackChanges(parsed);

  m_bDirty = true;
}

void plVegetationLayerComponent::SetGrassTexelSize(float fTexelSize)
{
  // The coverage keeps the grid stored in its blob; the editor resamples the blob in the same transaction that changes
  // this property.
  m_fGrassTexelSize = plGrassCoverageData::ClampTexelSize(fTexelSize);
}

void plVegetationLayerComponent::OnMsgTransformChanged(plMsgTransformChanged& msg)
{
  PL_IGNORE_UNUSED(msg);

  m_bDirty = true;
}

void plVegetationLayerComponent::Register()
{
  if (m_Parsed.IsEmpty())
  {
    Unregister();
    return;
  }

  plVegetationWorldModule* pModule = GetWorld()->GetOrCreateModule<plVegetationWorldModule>();
  if (pModule == nullptr)
    return;

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const float fOwnerScale = ownerTransform.m_vScale.GetLength() / plMath::Sqrt(3.0f);

  // The owner transform is part of every chunk's content: moving the layer re-registers all of it.
  const plUInt64 uiTransformHash = plHashingUtils::xxHash64(&ownerTransform, sizeof(plTransform));

  struct Bucket
  {
    plUInt64 m_uiKey = 0;
    plUInt32 m_uiEntry = 0;
    plUInt64 m_uiHash = 0;
    plDynamicArray<plVegetationInstanceDesc> m_Descs;
  };

  plDynamicArray<Bucket> buckets;
  plHashTable<plUInt64, plUInt32> bucketOfKey;

  for (plUInt32 e = 0; e < m_Parsed.m_Entries.GetCount(); ++e)
  {
    const auto& entry = m_Parsed.m_Entries[e];
    if (entry.m_sType.IsEmpty() || entry.m_Instances.IsEmpty())
      continue;

    const plUInt64 uiTypeHash = plHashingUtils::StringHash(entry.m_sType.GetView());

    for (const auto& inst : entry.m_Instances)
    {
      const plVec3 vPosition = ownerTransform.TransformPosition(inst.m_vPosition);

      // the world module's cell of the instance
      const plInt32 iCell[2] = {static_cast<plInt32>(plMath::Floor(vPosition.x / plVegetationCellSize)), static_cast<plInt32>(plMath::Floor(vPosition.y / plVegetationCellSize))};
      const plUInt64 uiKey = plHashingUtils::xxHash64(iCell, sizeof(iCell), uiTypeHash);

      plUInt32 uiBucket = 0;
      if (!bucketOfKey.TryGetValue(uiKey, uiBucket))
      {
        uiBucket = buckets.GetCount();
        bucketOfKey.Insert(uiKey, uiBucket);

        Bucket& bucket = buckets.ExpandAndGetRef();
        bucket.m_uiKey = uiKey;
        bucket.m_uiEntry = e;
        bucket.m_uiHash = uiTransformHash ^ uiTypeHash;
      }

      Bucket& bucket = buckets[uiBucket];

      // position, rotation and scale are contiguous; the padding after the colour seed is not hashed
      bucket.m_uiHash = plHashingUtils::xxHash64(&inst.m_vPosition, sizeof(plVec3) + sizeof(plQuat) + sizeof(float), bucket.m_uiHash);
      bucket.m_uiHash = plHashingUtils::xxHash64(&inst.m_uiColorSeed, sizeof(plUInt8), bucket.m_uiHash);

      plVegetationInstanceDesc& desc = bucket.m_Descs.ExpandAndGetRef();
      desc.m_vPosition = vPosition;
      desc.m_qRotation = ownerTransform.m_qRotation * inst.m_qRotation;
      desc.m_fScale = inst.m_fScale * fOwnerScale;
      desc.m_uiColorSeed = inst.m_uiColorSeed;
    }
  }

  // chunks that are gone or changed leave first
  plHybridArray<plUInt64, 64> removed;
  for (auto it = m_Chunks.GetIterator(); it.IsValid(); ++it)
  {
    plUInt32 uiBucket = 0;
    if (bucketOfKey.TryGetValue(it.Key(), uiBucket) && buckets[uiBucket].m_uiHash == it.Value().m_uiContentHash)
      continue;

    pModule->RemoveTile(it.Value().m_hTile);
    removed.PushBack(it.Key());
  }

  for (plUInt64 uiKey : removed)
    m_Chunks.Remove(uiKey);

  for (const Bucket& bucket : buckets)
  {
    if (m_Chunks.Contains(bucket.m_uiKey))
      continue;

    const plVegetationTileHandle hTile = pModule->AddInstances(m_Parsed.m_Entries[bucket.m_uiEntry].m_sType, bucket.m_Descs, plVegetationSource::Painted);
    if (hTile.IsInvalidated())
      continue;

    Chunk& chunk = m_Chunks[bucket.m_uiKey];
    chunk.m_uiContentHash = bucket.m_uiHash;
    chunk.m_hTile = hTile;
  }
}

void plVegetationLayerComponent::Unregister()
{
  if (m_Chunks.IsEmpty())
    return;

  if (plVegetationWorldModule* pModule = GetWorld()->GetModule<plVegetationWorldModule>())
  {
    for (auto it = m_Chunks.GetIterator(); it.IsValid(); ++it)
    {
      pModule->RemoveTile(it.Value().m_hTile);
    }
  }

  m_Chunks.Clear();
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationLayerComponent);
