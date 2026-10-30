#pragma once

#include <EditorEngineProcessFramework/EngineProcess/ViewRenderSettings.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAsset.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

class plOrbitCamViewWidget;

/// The Grass Type asset window: a live engine-process 3D preview of a grass patch with wind, next to the property grid.
class plGrassTypeAssetDocumentWindow : public plEngineDocumentWindow
{
public:
  plGrassTypeAssetDocumentWindow(plAssetDocument* pDocument);
  ~plGrassTypeAssetDocumentWindow();

  plGrassTypeAssetDocument* GetGrassDocument() const { return static_cast<plGrassTypeAssetDocument*>(GetDocument()); }

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
