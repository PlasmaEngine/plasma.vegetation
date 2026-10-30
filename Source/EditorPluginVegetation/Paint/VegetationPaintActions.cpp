#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/Actions/TransformGizmoActions.h>
#include <EditorPluginVegetation/Paint/VegetationPaintActions.h>
#include <EditorPluginVegetation/Paint/VegetationPaintEditTool.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

plActionDescriptorHandle plVegetationPaintActions::s_hPaintTool;

void plVegetationPaintActions::RegisterActions()
{
  // No shortcut: V belongs to the visualizer toggle and B to the modeling brush.
  s_hPaintTool = PL_REGISTER_ACTION_1("Gizmo.Mode.Vegetation", plActionScope::Document, "Gizmo", "", plGizmoAction, plGetStaticRTTI<plVegetationPaintEditTool>());
}

void plVegetationPaintActions::UnregisterActions()
{
  plActionManager::UnregisterAction(s_hPaintTool);
}

void plVegetationPaintActions::MapActions()
{
  // The scene plugin owns these maps; a missing one is a logged degradation, not an assert.
  const char* szMenuBars[] = {"EditorPluginScene_DocumentMenuBar", "EditorPluginScene_Scene2MenuBar"};
  for (const char* szMapping : szMenuBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
      pMap->MapAction(s_hPaintTool, "G.Gizmos", 5.6f);
    else
      plLog::Warning("Action map '{0}' not found - the Vegetation tool will not appear in that menu.", szMapping);
  }

  const char* szToolBars[] = {"EditorPluginScene_DocumentToolBar", "EditorPluginScene_Scene2ToolBar"};
  for (const char* szMapping : szToolBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
      pMap->MapAction(s_hPaintTool, "GizmoCategory", 5.6f);
    else
      plLog::Warning("Action map '{0}' not found - the Vegetation tool will not appear in that toolbar.", szMapping);
  }
}
