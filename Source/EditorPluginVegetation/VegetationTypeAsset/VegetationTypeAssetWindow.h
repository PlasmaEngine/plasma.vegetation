#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plOrbitCamViewWidget;

/// The Vegetation Type asset window: a live engine-process 3D preview of the plant with wind, next to the property grid.
class plVegetationTypeAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plVegetationTypeAssetDocumentWindow(plAssetDocument* pDocument);
  ~plVegetationTypeAssetDocumentWindow();

  plVegetationTypeAssetDocument* GetVegetationDocument() const { return static_cast<plVegetationTypeAssetDocument*>(GetDocument()); }

protected:
  virtual void InternalRedraw() override;
  virtual void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg) override;

private:
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void SendRedrawMsg();
  void QueryObjectBBox(plInt32 iPurpose = 0);
  void UpdatePreview();
  void RestoreResource();

  plEngineViewConfig m_ViewConfig;
  plOrbitCamViewWidget* m_pViewWidget = nullptr;
};
