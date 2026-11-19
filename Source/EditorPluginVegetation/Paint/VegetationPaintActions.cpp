#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/Actions/TransformGizmoActions.h>
#include <EditorPluginVegetation/Paint/VegetationLayerFill.h>
#include <EditorPluginVegetation/Paint/VegetationPaintActions.h>
#include <EditorPluginVegetation/Paint/VegetationPaintEditTool.h>
#include <GuiFoundation/Action/ActionManager.h>
#include <GuiFoundation/Action/ActionMapManager.h>

plActionDescriptorHandle plVegetationPaintActions::s_hPaintTool;
plActionDescriptorHandle plVegetationPaintActions::s_hLayerFill;

void plVegetationPaintActions::RegisterActions()
{
  // No shortcut: V belongs to the visualizer toggle and B to the modeling brush.
  s_hPaintTool = PL_REGISTER_ACTION_1("Gizmo.Mode.Vegetation", plActionScope::Document, "Gizmo", "", plGizmoAction, plGetStaticRTTI<plVegetationPaintEditTool>());

  // Scriptable: runs from action_execute with the scene document; the panel's Fill Layer is the interactive path.
  s_hLayerFill = PL_REGISTER_ACTION_0("Vegetation.Layer.Fill", plActionScope::Document, "Vegetation", "", plVegetationLayerFillAction);

  plVegetationLayerFill::Startup();
}

void plVegetationPaintActions::UnregisterActions()
{
  plVegetationLayerFill::Shutdown();

  plActionManager::UnregisterAction(s_hLayerFill);
  plActionManager::UnregisterAction(s_hPaintTool);
}

void plVegetationPaintActions::MapActions()
{
  // The scene plugin owns these maps; a missing one is a logged degradation, not an assert.
  const char* szMenuBars[] = {"EditorPluginScene_DocumentMenuBar", "EditorPluginScene_Scene2MenuBar"};
  for (const char* szMapping : szMenuBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hPaintTool, "G.Gizmos", 5.6f);
      // Scene > Fill Vegetation Layer repeats the selected layers' last fill.
      pMap->MapAction(s_hLayerFill, "G.Scene", 4.5f);
    }
    else
      plLog::Warning("Action map '{0}' not found - the Vegetation tool will not appear in that menu.", szMapping);
  }

  const char* szToolBars[] = {"EditorPluginScene_DocumentToolBar", "EditorPluginScene_Scene2ToolBar"};
  for (const char* szMapping : szToolBars)
  {
    if (plActionMap* pMap = plActionMapManager::GetActionMap(szMapping))
    {
      pMap->MapAction(s_hPaintTool, "GizmoCategory", 5.6f);
      // Menus build their actions lazily; the toolbar instantiates Fill with the document, so scripts can run it too.
      pMap->MapAction(s_hLayerFill, "GizmoCategory", 5.61f);
    }
    else
      plLog::Warning("Action map '{0}' not found - the Vegetation tool will not appear in that toolbar.", szMapping);
  }
}
