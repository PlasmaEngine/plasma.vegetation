#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorFramework/Actions/AssetActions.h>
#include <EditorFramework/Actions/ProjectActions.h>
#include <EditorPluginVegetation/Actions/GrassTypeActions.h>
#include <EditorPluginVegetation/Actions/VegetationTypeActions.h>
#include <EditorPluginVegetation/Paint/VegetationPaintActions.h>
#include <EditorPluginVegetation/Paint/VegetationPanel.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAssetObjects.h>
#include <EditorUi/Widgets.h>
#include <Foundation/Configuration/Plugin.h>
#include <GuiFoundation/Action/ActionMapManager.h>
#include <GuiFoundation/Action/CommandHistoryActions.h>
#include <GuiFoundation/Action/DocumentActions.h>
#include <GuiFoundation/Action/StandardMenus.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>

namespace
{
  /// The package's icon folder: the icons under their ids (":/EditorPluginVegetation/Paint.svg" is
  /// "EditorIcons/EditorPluginVegetation/Paint.svg"). The plugin binary lives in <package>/Bin/<configuration>.
  plString s_sIconFolder;

  void RegisterIconFolder()
  {
    plStringBuilder sPlugin, sCopy;
    plPlugin::GetPluginPaths("plEditorPluginVegetation", sPlugin, sCopy, 0);

    plStringBuilder sFolder = sPlugin.GetFileDirectory();
    sFolder.AppendPath("..", "..", "EditorIcons");
    sFolder.MakeCleanPath();

    s_sIconFolder = sFolder;
    plEditorUi::AddIconFolder(s_sIconFolder);
  }

  void UnregisterIconFolder()
  {
    plEditorUi::RemoveIconFolder(s_sIconFolder);
    s_sIconFolder.Clear();
  }
} // namespace

PL_PLUGIN_ON_LOADED()
{
  RegisterIconFolder();

  plVegetationTypeActions::RegisterActions();

  // Vegetation Type asset window
  {
    plActionMapManager::RegisterActionMap("VegetationTypeAssetMenuBar").IgnoreResult();
    plStandardMenus::MapActions("VegetationTypeAssetMenuBar", plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
    plProjectActions::MapActions("VegetationTypeAssetMenuBar");
    plDocumentActions::MapMenuActions("VegetationTypeAssetMenuBar");
    plAssetActions::MapMenuActions("VegetationTypeAssetMenuBar");
    plCommandHistoryActions::MapActions("VegetationTypeAssetMenuBar");

    plActionMapManager::RegisterActionMap("VegetationTypeAssetToolBar").IgnoreResult();
    plDocumentActions::MapToolbarActions("VegetationTypeAssetToolBar");
    plCommandHistoryActions::MapActions("VegetationTypeAssetToolBar", "");
    plAssetActions::MapToolBarActions("VegetationTypeAssetToolBar", true);
    plVegetationTypeActions::MapActions("VegetationTypeAssetToolBar");
  }

  plGrassTypeActions::RegisterActions();

  // Grass Type asset window
  {
    plActionMapManager::RegisterActionMap("GrassTypeAssetMenuBar").IgnoreResult();
    plStandardMenus::MapActions("GrassTypeAssetMenuBar", plStandardMenuTypes::Default | plStandardMenuTypes::Edit);
    plProjectActions::MapActions("GrassTypeAssetMenuBar");
    plDocumentActions::MapMenuActions("GrassTypeAssetMenuBar");
    plAssetActions::MapMenuActions("GrassTypeAssetMenuBar");
    plCommandHistoryActions::MapActions("GrassTypeAssetMenuBar");

    plActionMapManager::RegisterActionMap("GrassTypeAssetToolBar").IgnoreResult();
    plDocumentActions::MapToolbarActions("GrassTypeAssetToolBar");
    plCommandHistoryActions::MapActions("GrassTypeAssetToolBar", "");
    plAssetActions::MapToolBarActions("GrassTypeAssetToolBar", true);
    plGrassTypeActions::MapActions("GrassTypeAssetToolBar");
  }

  // Scene paint tool and its dock panel
  plVegetationPaintActions::RegisterActions();
  plVegetationPaintActions::MapActions();
  plVegetationPanelManager::Startup();
}

PL_PLUGIN_ON_UNLOADED()
{
  plVegetationPanelManager::Shutdown();
  plVegetationPaintActions::UnregisterActions();
  plGrassTypeActions::UnregisterActions();
  plVegetationTypeActions::UnregisterActions();

  UnregisterIconFolder();
}
