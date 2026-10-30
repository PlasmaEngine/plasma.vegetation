#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/Actions/VegetationTypeActions.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plActionDescriptorHandle plVegetationTypeActions::s_hCategory;
plActionDescriptorHandle plVegetationTypeActions::s_hWindMenu;
plActionDescriptorHandle plVegetationTypeActions::s_hWind[4];
plActionDescriptorHandle plVegetationTypeActions::s_hTouch;

void plVegetationTypeActions::RegisterActions()
{
  s_hCategory = PL_REGISTER_CATEGORY("VegetationTypeCategory");

  s_hWindMenu = PL_REGISTER_MENU_WITH_ICON("VegetationType.Wind.Menu", ":/EditorPluginVegetation/Wind.svg");
  s_hWind[0] = PL_REGISTER_ACTION_2("VegetationType.Wind.Off", plActionScope::Document, "Vegetation Type", "", plVegetationTypeAction, plVegetationTypeAction::ActionType::PreviewWind, plVegetationPreviewWind::Off);
  s_hWind[1] = PL_REGISTER_ACTION_2("VegetationType.Wind.Light", plActionScope::Document, "Vegetation Type", "", plVegetationTypeAction, plVegetationTypeAction::ActionType::PreviewWind, plVegetationPreviewWind::Light);
  s_hWind[2] = PL_REGISTER_ACTION_2("VegetationType.Wind.Moderate", plActionScope::Document, "Vegetation Type", "", plVegetationTypeAction, plVegetationTypeAction::ActionType::PreviewWind, plVegetationPreviewWind::Moderate);
  s_hWind[3] = PL_REGISTER_ACTION_2("VegetationType.Wind.Strong", plActionScope::Document, "Vegetation Type", "", plVegetationTypeAction, plVegetationTypeAction::ActionType::PreviewWind, plVegetationPreviewWind::Strong);

  s_hTouch = PL_REGISTER_ACTION_1("VegetationType.Touch", plActionScope::Document, "Vegetation Type", "", plVegetationTypeAction, plVegetationTypeAction::ActionType::Touch);
}

void plVegetationTypeActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategory);
  plActionManager::UnregisterAction(s_hWindMenu);

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_hWind); ++i)
    plActionManager::UnregisterAction(s_hWind[i]);

  plActionManager::UnregisterAction(s_hTouch);
}

void plVegetationTypeActions::MapActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  pMap->MapAction(s_hCategory, "", 11.0f);

  const char* szSubPath = "VegetationTypeCategory";
  pMap->MapAction(s_hWindMenu, szSubPath, 1.0f);

  plStringBuilder sSubPath(szSubPath, "/VegetationType.Wind.Menu");
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_hWind); ++i)
    pMap->MapAction(s_hWind[i], sSubPath, i + 1.0f);

  pMap->MapAction(s_hTouch, szSubPath, 2.0f);
}

plVegetationTypeAction::plVegetationTypeAction(const plActionContext& context, const char* szName, ActionType type, plVegetationPreviewWind::Enum wind)
  : plButtonAction(context, szName, false, "")
{
  m_Type = type;
  m_Wind = wind;

  m_pDocument = const_cast<plVegetationTypeAssetDocument*>(static_cast<const plVegetationTypeAssetDocument*>(context.m_pDocument));
  m_pDocument->m_Events.AddEventHandler(plMakeDelegate(&plVegetationTypeAction::DocumentEventHandler, this));

  if (m_Type == ActionType::Touch)
  {
    SetIconPath(":/EditorPluginVegetation/Touch.svg");
  }

  UpdateState();
}

plVegetationTypeAction::~plVegetationTypeAction()
{
  m_pDocument->m_Events.RemoveEventHandler(plMakeDelegate(&plVegetationTypeAction::DocumentEventHandler, this));
}

void plVegetationTypeAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);

  if (m_Type == ActionType::PreviewWind)
  {
    m_pDocument->SetPreviewWind(m_Wind);
  }
  else if (m_Type == ActionType::Touch)
  {
    m_pDocument->TriggerTouch();
  }
}

void plVegetationTypeAction::DocumentEventHandler(const plVegetationTypeAssetEvent& e)
{
  if (e.m_Type == plVegetationTypeAssetEvent::Type::PreviewWindChanged)
  {
    UpdateState();
  }
}

void plVegetationTypeAction::UpdateState()
{
  if (m_Type == ActionType::PreviewWind)
  {
    SetCheckable(true);
    SetChecked(m_pDocument->GetPreviewWind() == m_Wind);
  }
}
