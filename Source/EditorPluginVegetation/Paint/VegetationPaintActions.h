#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <GuiFoundation/Action/BaseActions.h>

/// \brief Registers the Vegetation paint tool action and maps it into the scene document's menus and toolbars.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationPaintActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();
  static void MapActions();

  static plActionDescriptorHandle s_hPaintTool;
};
