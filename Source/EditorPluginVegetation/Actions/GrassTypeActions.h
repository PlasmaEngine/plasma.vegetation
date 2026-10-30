#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAsset.h>
#include <GuiFoundation/Action/BaseActions.h>

/// Toolbar actions of the Grass Type asset window: preview wind strength.
class plGrassTypeActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();
  static void MapActions(plStringView sMapping);

  static plActionDescriptorHandle s_hCategory;
  static plActionDescriptorHandle s_hWindMenu;
  static plActionDescriptorHandle s_hWind[4];
};

class plGrassTypeAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeAction, plButtonAction);

public:
  plGrassTypeAction(const plActionContext& context, const char* szName, plVegetationPreviewWind::Enum wind);
  ~plGrassTypeAction();

  virtual void Execute(const plVariant& value) override;

private:
  void DocumentEventHandler(const plGrassTypeAssetEvent& e);
  void UpdateState();

  plGrassTypeAssetDocument* m_pDocument;
  plVegetationPreviewWind::Enum m_Wind;
};
