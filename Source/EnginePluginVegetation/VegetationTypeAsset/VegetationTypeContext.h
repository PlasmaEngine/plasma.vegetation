#pragma once

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessDocumentContext.h>
#include <EnginePluginVegetation/EnginePluginVegetationDLL.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

/// \brief Engine-process side of the Vegetation Type asset: a world with one instance of the type, a wind source and
/// a ground disc, rendered live in the asset window and for thumbnails.
class PL_ENGINEPLUGINVEGETATION_DLL plVegetationTypeContext : public plEngineProcessDocumentContext
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeContext, plEngineProcessDocumentContext);

public:
  plVegetationTypeContext();

  virtual void HandleMessage(const plEditorEngineDocumentMsg* pMsg) override;

  const plVegetationTypeResourceHandle& GetResource() const { return m_hMainResource; }

protected:
  virtual void OnInitialize() override;

  virtual plEngineProcessViewContext* CreateViewContext() override;
  virtual void DestroyViewContext(plEngineProcessViewContext* pContext) override;
  virtual bool UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext) override;

private:
  void QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg);
  plBoundingBoxSphere ComputePreviewBounds() const;

  plGameObject* m_pMainObject = nullptr;
  plComponentHandle m_hInstanceComponent;
  plComponentHandle m_hWindComponent;
  plVegetationTypeResourceHandle m_hMainResource;
  plMeshResourceHandle m_hPreviewMeshResource;
};
