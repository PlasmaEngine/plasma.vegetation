#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginVegetation/EnginePluginVegetationDLL.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

/// \brief Engine-process side of the Grass Type asset: a world with a 4 m plane grown with this type and a wind source,
/// rendered live in the asset window and for thumbnails.
class PL_ENGINEPLUGINVEGETATION_DLL plGrassTypeContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeContext, plEngineProcessDocumentContext);

public:
  plGrassTypeContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

  const plGrassTypeResourceHandle& GetResource() const { return m_hMainResource; }

  /// \brief Side length of the preview plane in metres.
  static constexpr float PlaneSize = 4.0f;

protected:
  virtual void OnInitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override;
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override;
  virtual bool UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext) override;

private:
  void QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg);
  plBoundingBoxSphere ComputePreviewBounds() const;

  plGameObject* m_pMainObject = nullptr;
  plComponentHandle m_hGrassComponent;
  plComponentHandle m_hWindComponent;
  plGrassTypeResourceHandle m_hMainResource;
  plMeshResourceHandle m_hPlaneMesh;
};
