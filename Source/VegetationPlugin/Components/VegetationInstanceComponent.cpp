#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/Messages/TransformChangedMessage.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <VegetationPlugin/Components/VegetationInstanceComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plVegetationInstanceComponent, 1, plComponentMode::Static)
{
  PL_BEGIN_PROPERTIES
  {
    PL_ACCESSOR_PROPERTY("Type", GetTypeFile, SetTypeFile)->AddAttributes(new plAssetBrowserAttribute("CompatibleAsset_Vegetation_Type")),
    PL_ACCESSOR_PROPERTY("ColorSeed", GetColorSeed, SetColorSeed)->AddAttributes(new plDefaultValueAttribute(0), new plClampValueAttribute(0, 255)),
    PL_ACCESSOR_PROPERTY("CastShadows", GetCastShadows, SetCastShadows)->AddAttributes(new plDefaultValueAttribute(true)),
    PL_ACCESSOR_PROPERTY("TouchBending", GetTouchBending, SetTouchBending)->AddAttributes(new plDefaultValueAttribute(true)),
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

plVegetationInstanceComponent::plVegetationInstanceComponent() = default;
plVegetationInstanceComponent::~plVegetationInstanceComponent() = default;

void plVegetationInstanceComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_hType;
  s << m_uiColorSeed;
  s << m_bCastShadows;
  s << m_bTouchBending;
}

void plVegetationInstanceComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_hType;
  s >> m_uiColorSeed;
  s >> m_bCastShadows;
  s >> m_bTouchBending;
}

void plVegetationInstanceComponent::OnActivated()
{
  SUPER::OnActivated();

  GetOwner()->EnableStaticTransformChangesNotifications();
  m_bDirty = true;
}

void plVegetationInstanceComponent::Update()
{
  if (!m_bDirty)
    return;

  m_bDirty = false;
  Unregister();
  Register();
}

void plVegetationInstanceComponent::OnDeactivated()
{
  Unregister();

  SUPER::OnDeactivated();
}

void plVegetationInstanceComponent::SetTypeFile(const char* szFile)
{
  plVegetationTypeResourceHandle hType;

  if (!plStringUtils::IsNullOrEmpty(szFile))
  {
    hType = plResourceManager::LoadResource<plVegetationTypeResource>(szFile);
  }

  SetType(hType);
}

const char* plVegetationInstanceComponent::GetTypeFile() const
{
  if (!m_hType.IsValid())
    return "";

  return m_hType.GetResourceID();
}

void plVegetationInstanceComponent::SetType(const plVegetationTypeResourceHandle& hType)
{
  if (m_hType == hType)
    return;

  m_hType = hType;

  m_bDirty = true;
}

void plVegetationInstanceComponent::SetColorSeed(plUInt8 uiSeed)
{
  m_uiColorSeed = uiSeed;

  m_bDirty = true;
}

void plVegetationInstanceComponent::SetCastShadows(bool b)
{
  m_bCastShadows = b;

  m_bDirty = true;
}

void plVegetationInstanceComponent::SetTouchBending(bool b)
{
  m_bTouchBending = b;

  m_bDirty = true;
}

void plVegetationInstanceComponent::OnMsgTransformChanged(plMsgTransformChanged& msg)
{
  PL_IGNORE_UNUSED(msg);

  m_bDirty = true;
}

void plVegetationInstanceComponent::Register()
{
  if (!m_hType.IsValid())
    return;

  plVegetationWorldModule* pModule = GetWorld()->GetOrCreateModule<plVegetationWorldModule>();
  if (pModule == nullptr)
    return;

  const plTransform t = GetOwner()->GetGlobalTransform();

  plVegetationInstanceDesc desc;
  desc.m_vPosition = t.m_vPosition;
  desc.m_qRotation = t.m_qRotation;
  desc.m_fScale = t.m_vScale.GetLength() / plMath::Sqrt(3.0f);
  desc.m_uiColorSeed = m_uiColorSeed;
  desc.m_Flags = plVegetationInstanceFlags::Hero;
  if (!m_bCastShadows)
    desc.m_Flags.Add(plVegetationInstanceFlags::NoShadow);
  if (!m_bTouchBending)
    desc.m_Flags.Add(plVegetationInstanceFlags::NoTouch);

  m_hTile = pModule->AddInstances(m_hType, plMakeArrayPtr(&desc, 1), plVegetationSource::Placed);
}

void plVegetationInstanceComponent::Unregister()
{
  if (m_hTile.IsInvalidated())
    return;

  if (plVegetationWorldModule* pModule = GetWorld()->GetModule<plVegetationWorldModule>())
  {
    pModule->RemoveTile(m_hTile);
  }

  m_hTile.Invalidate();
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationInstanceComponent);
