#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>
#include <GuiFoundation/Action/BaseActions.h>

/// Toolbar actions of the Vegetation Type asset window: preview wind strength and a touch push.
class plVegetationTypeActions
{
public:
  static void RegisterActions();
  static void UnregisterActions();
  static void MapActions(plStringView sMapping);

  static plActionDescriptorHandle s_hCategory;
  static plActionDescriptorHandle s_hWindMenu;
  static plActionDescriptorHandle s_hWind[4];
  static plActionDescriptorHandle s_hTouch;
};

class plVegetationTypeAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeAction, plButtonAction);

public:
  enum class ActionType
  {
    PreviewWind,
    Touch,
  };

  plVegetationTypeAction(const plActionContext& context, const char* szName, ActionType type, plVegetationPreviewWind::Enum wind = plVegetationPreviewWind::Light);
  ~plVegetationTypeAction();

  virtual void Execute(const plVariant& value) override;

private:
  void DocumentEventHandler(const plVegetationTypeAssetEvent& e);
  void UpdateState();

  plVegetationTypeAssetDocument* m_pDocument;
  ActionType m_Type;
  plVegetationPreviewWind::Enum m_Wind;
};
