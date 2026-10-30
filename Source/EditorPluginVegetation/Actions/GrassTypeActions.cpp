#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/Actions/GrassTypeActions.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plActionDescriptorHandle plGrassTypeActions::s_hCategory;
plActionDescriptorHandle plGrassTypeActions::s_hWindMenu;
plActionDescriptorHandle plGrassTypeActions::s_hWind[4];

void plGrassTypeActions::RegisterActions()
{
  s_hCategory = PL_REGISTER_CATEGORY("GrassTypeCategory");

  s_hWindMenu = PL_REGISTER_MENU_WITH_ICON("GrassType.Wind.Menu", ":/EditorPluginVegetation/Wind.svg");
  s_hWind[0] = PL_REGISTER_ACTION_1("GrassType.Wind.Off", plActionScope::Document, "Grass Type", "", plGrassTypeAction, plVegetationPreviewWind::Off);
  s_hWind[1] = PL_REGISTER_ACTION_1("GrassType.Wind.Light", plActionScope::Document, "Grass Type", "", plGrassTypeAction, plVegetationPreviewWind::Light);
  s_hWind[2] = PL_REGISTER_ACTION_1("GrassType.Wind.Moderate", plActionScope::Document, "Grass Type", "", plGrassTypeAction, plVegetationPreviewWind::Moderate);
  s_hWind[3] = PL_REGISTER_ACTION_1("GrassType.Wind.Strong", plActionScope::Document, "Grass Type", "", plGrassTypeAction, plVegetationPreviewWind::Strong);
}

void plGrassTypeActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hCategory);
  plActionManager::UnregisterAction(s_hWindMenu);

  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_hWind); ++i)
    plActionManager::UnregisterAction(s_hWind[i]);
}

void plGrassTypeActions::MapActions(plStringView sMapping)
{
  plActionMap* pMap = plActionMapManager::GetActionMap(sMapping);
  PL_ASSERT_DEV(pMap != nullptr, "The given mapping ('{0}') does not exist, mapping the actions failed!", sMapping);

  pMap->MapAction(s_hCategory, "", 11.0f);

  const char* szSubPath = "GrassTypeCategory";
  pMap->MapAction(s_hWindMenu, szSubPath, 1.0f);

  plStringBuilder sSubPath(szSubPath, "/GrassType.Wind.Menu");
  for (plUInt32 i = 0; i < PL_ARRAY_SIZE(s_hWind); ++i)
    pMap->MapAction(s_hWind[i], sSubPath, i + 1.0f);
}

plGrassTypeAction::plGrassTypeAction(const plActionContext& context, const char* szName, plVegetationPreviewWind::Enum wind)
  : plButtonAction(context, szName, false, "")
{
  m_Wind = wind;

  m_pDocument = const_cast<plGrassTypeAssetDocument*>(static_cast<const plGrassTypeAssetDocument*>(context.m_pDocument));
  m_pDocument->m_Events.AddEventHandler(plMakeDelegate(&plGrassTypeAction::DocumentEventHandler, this));

  UpdateState();
}

plGrassTypeAction::~plGrassTypeAction()
{
  m_pDocument->m_Events.RemoveEventHandler(plMakeDelegate(&plGrassTypeAction::DocumentEventHandler, this));
}

void plGrassTypeAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);
  m_pDocument->SetPreviewWind(m_Wind);
}

void plGrassTypeAction::DocumentEventHandler(const plGrassTypeAssetEvent& e)
{
  if (e.m_Type == plGrassTypeAssetEvent::Type::PreviewWindChanged)
  {
    UpdateState();
  }
}

void plGrassTypeAction::UpdateState()
{
  SetCheckable(true);
  SetChecked(m_pDocument->GetPreviewWind() == m_Wind);
}
