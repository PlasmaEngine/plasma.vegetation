#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <EnginePluginVegetation/GrassTypeAsset/GrassTypeContext.h>
#include <EnginePluginVegetation/GrassTypeAsset/GrassTypeView.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

plGrassTypeViewContext::plGrassTypeViewContext(plGrassTypeContext* pContext)
  : plEngineProcessViewContext(pContext)
{
  m_pGrassContext = pContext;

  m_Camera.SetCameraMode(plCameraMode::PerspectiveFixedFovX, 45.0f, 0.05f, 10000.0f);
  m_Camera.LookAt(plVec3(-4, -3, 2), plVec3::MakeZero(), plVec3(0.0f, 0.0f, 1.0f));
}

plGrassTypeViewContext::~plGrassTypeViewContext() = default;

bool plGrassTypeViewContext::UpdateThumbnailCamera(const plBoundingBoxSphere& bounds)
{
  return !FocusCameraOnObject(m_Camera, bounds, 45.0f, -plVec3(5, -2, 3));
}

plViewHandle plGrassTypeViewContext::CreateView()
{
  plView* pView = nullptr;
  plRenderWorld::CreateView("Grass Type Editor - View", pView);
  pView->SetCameraUsageHint(plCameraUsageHint::EditorView);

  pView->SetRenderPipelineResource(CreateDefaultRenderPipeline());

  plEngineProcessDocumentContext* pDocumentContext = GetDocumentContext();
  pView->SetWorld(pDocumentContext->GetWorld());
  pView->SetCamera(&m_Camera);
  return pView->GetHandle();
}
