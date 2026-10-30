#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Messages/TransformChangedMessage.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plVegetationLayerComponent, 2, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Data", GetData, SetData)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY_READ_ONLY("InstanceCount", GetInstanceCount),
    PL_ACCESSOR_PROPERTY("GrassCoverage", GetGrassCoverageBlob, SetGrassCoverageBlob)->AddAttributes(new plHiddenAttribute()),
    PL_ACCESSOR_PROPERTY("GrassTexelSize", GetGrassTexelSize, SetGrassTexelSize)->AddAttributes(new plDefaultValueAttribute(plGrassCoverageData::DefaultTexelSize), new plClampValueAttribute(plGrassCoverageData::MinTexelSize, plGrassCoverageData::MaxTexelSize), new plSuffixAttribute(" m")),
    PL_ACCESSOR_PROPERTY_READ_ONLY("GrassTileCount", GetGrassTileCount),
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
  Unregister();
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
    return;

  plVegetationWorldModule* pModule = GetWorld()->GetOrCreateModule<plVegetationWorldModule>();
  if (pModule == nullptr)
    return;

  const plTransform ownerTransform = GetOwner()->GetGlobalTransform();
  const float fOwnerScale = ownerTransform.m_vScale.GetLength() / plMath::Sqrt(3.0f);

  plDynamicArray<plVegetationInstanceDesc> descs;

  for (const auto& entry : m_Parsed.m_Entries)
  {
    if (entry.m_sType.IsEmpty() || entry.m_Instances.IsEmpty())
      continue;

    descs.Clear();
    descs.Reserve(entry.m_Instances.GetCount());

    for (const auto& inst : entry.m_Instances)
    {
      plVegetationInstanceDesc& desc = descs.ExpandAndGetRef();
      desc.m_vPosition = ownerTransform.TransformPosition(inst.m_vPosition);
      desc.m_qRotation = ownerTransform.m_qRotation * inst.m_qRotation;
      desc.m_fScale = inst.m_fScale * fOwnerScale;
      desc.m_uiColorSeed = inst.m_uiColorSeed;
    }

    const plVegetationTileHandle hTile = pModule->AddInstances(entry.m_sType, descs, plVegetationSource::Painted);
    if (!hTile.IsInvalidated())
    {
      m_Tiles.PushBack(hTile);
    }
  }
}

void plVegetationLayerComponent::Unregister()
{
  if (m_Tiles.IsEmpty())
    return;

  if (plVegetationWorldModule* pModule = GetWorld()->GetModule<plVegetationWorldModule>())
  {
    for (plVegetationTileHandle hTile : m_Tiles)
    {
      pModule->RemoveTile(hTile);
    }
  }

  m_Tiles.Clear();
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationLayerComponent);
