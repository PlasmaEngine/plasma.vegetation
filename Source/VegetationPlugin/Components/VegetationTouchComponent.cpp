#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <VegetationPlugin/Components/VegetationTouchComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// clang-format off
PL_BEGIN_COMPONENT_TYPE(plVegetationTouchComponent, 1, plComponentMode::Dynamic)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Radius", m_fRadius)->AddAttributes(new plDefaultValueAttribute(0.8f), new plClampValueAttribute(0.05f, 20.0f), new plSuffixAttribute(" m")),
    PL_MEMBER_PROPERTY("Strength", m_fStrength)->AddAttributes(new plDefaultValueAttribute(1.0f), new plClampValueAttribute(0.0f, 1.0f)),
    PL_MEMBER_PROPERTY("VelocityInfluence", m_fVelocityInfluence)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 2.0f)),
    PL_MEMBER_PROPERTY("Crush", m_fCrush)->AddAttributes(new plDefaultValueAttribute(0.5f), new plClampValueAttribute(0.0f, 1.0f)),
  }
  PL_END_PROPERTIES;
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vegetation"),
    new plSphereVisualizerAttribute("Radius", plColor::LightGreen),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTouchComponent::plVegetationTouchComponent() = default;
plVegetationTouchComponent::~plVegetationTouchComponent() = default;

void plVegetationTouchComponent::SerializeComponent(plWorldWriter& inout_stream) const
{
  SUPER::SerializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s << m_fRadius;
  s << m_fStrength;
  s << m_fVelocityInfluence;
  s << m_fCrush;
}

void plVegetationTouchComponent::DeserializeComponent(plWorldReader& inout_stream)
{
  SUPER::DeserializeComponent(inout_stream);
  auto& s = inout_stream.GetStream();

  s >> m_fRadius;
  s >> m_fStrength;
  s >> m_fVelocityInfluence;
  s >> m_fCrush;
}

void plVegetationTouchComponent::OnActivated()
{
  SUPER::OnActivated();

  m_bHasLastPosition = false;
}

void plVegetationTouchComponent::Update()
{
  plVegetationWorldModule* pModule = GetWorld()->GetOrCreateModule<plVegetationWorldModule>();
  if (pModule == nullptr)
    return;

  const plVec3 vPosition = GetOwner()->GetGlobalPosition();
  const float fDeltaTime = (float)GetWorld()->GetClock().GetTimeDiff().GetSeconds();

  plVec3 vVelocity = plVec3::MakeZero();
  if (m_bHasLastPosition && fDeltaTime > 0.0001f)
  {
    vVelocity = (vPosition - m_vLastPosition) / fDeltaTime * m_fVelocityInfluence;
  }

  m_vLastPosition = vPosition;
  m_bHasLastPosition = true;

  pModule->RegisterTouchSource(vPosition, m_fRadius, m_fStrength, vVelocity);
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Components_VegetationTouchComponent);
