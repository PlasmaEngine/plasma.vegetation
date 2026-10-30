#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <EnginePluginVegetation/VegetationTypeAsset/VegetationTypeContext.h>
#include <EnginePluginVegetation/VegetationTypeAsset/VegetationTypeView.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

plVegetationTypeViewContext::plVegetationTypeViewContext(plVegetationTypeContext* pContext)
  : plEngineProcessViewContext(pContext)
{
  m_pVegetationContext = pContext;

  m_Camera.SetCameraMode(plCameraMode::PerspectiveFixedFovX, 45.0f, 0.05f, 10000.0f);
  m_Camera.LookAt(plVec3(1, 1, 1), plVec3::MakeZero(), plVec3(0.0f, 0.0f, 1.0f));
}

plVegetationTypeViewContext::~plVegetationTypeViewContext() = default;

bool plVegetationTypeViewContext::UpdateThumbnailCamera(const plBoundingBoxSphere& bounds)
{
  return !FocusCameraOnObject(m_Camera, bounds, 45.0f, -plVec3(5, -2, 3));
}

plViewHandle plVegetationTypeViewContext::CreateView()
{
  plView* pView = nullptr;
  plRenderWorld::CreateView("Vegetation Type Editor - View", pView);
  pView->SetCameraUsageHint(plCameraUsageHint::EditorView);

  pView->SetRenderPipelineResource(CreateDefaultRenderPipeline());

  plEngineProcessDocumentContext* pDocumentContext = GetDocumentContext();
  pView->SetWorld(pDocumentContext->GetWorld());
  pView->SetCamera(&m_Camera);
  return pView->GetHandle();
}
