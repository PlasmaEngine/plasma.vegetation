#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginVegetation/GrassTypeAsset/GrassTypeContext.h>
#include <EnginePluginVegetation/GrassTypeAsset/GrassTypeView.h>
#include <GameEngine/Effects/Wind/SimpleWindComponent.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <VegetationPlugin/Grass/GrassSurfaceComponent.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeContext, 1, plRTTIDefaultAllocator<plGrassTypeContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Grass Type"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassTypeContext::plGrassTypeContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::CreateWorld)
{
}

void plGrassTypeContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg0)
{
  if (auto pMsg = plDynamicCast<const plQuerySelectionBBoxMsgToEngine*>(pMsg0))
  {
    QuerySelectionBBox(pMsg);
    return;
  }

  if (auto pMsg = plDynamicCast<const plSimpleDocumentConfigMsgToEngine*>(pMsg0))
  {
    PL_LOCK(m_pWorld->GetWriteMarker());

    if (pMsg->m_sWhatToDo == "SetWindStrength")
    {
      plSimpleWindComponent* pWind = nullptr;
      if (m_pWorld->TryGetComponent(m_hWindComponent, pWind))
      {
        const plInt32 iStrength = pMsg->m_PayloadValue.ConvertTo<plInt32>();
        static const plWindStrength::Enum strengths[] = {plWindStrength::None, plWindStrength::LightBreeze, plWindStrength::ModerateBreeze, plWindStrength::StrongBreeze};
        const plWindStrength::Enum s = strengths[plMath::Clamp(iStrength, 0, 3)];
        pWind->m_MinWindStrength = s;
        pWind->m_MaxWindStrength = s;
      }
    }

    return;
  }

  plEngineProcessDocumentContext::HandleMessage(pMsg0);
}

void plGrassTypeContext::OnInitialize()
{
  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  // The grass simulation only steps while the world simulates.
  pWorld->SetWorldSimulationEnabled(true);

  // Plane mesh, shared between documents. The surface component reads the CPU copy under the same id.
  {
    const char* szMeshName = "GrassPreviewPlaneMesh";
    m_hPlaneMesh = plResourceManager::GetExistingResource<plMeshResource>(szMeshName);

    if (!m_hPlaneMesh.IsValid() || !plResourceManager::GetExistingResource<plCpuMeshResource>(szMeshName).IsValid())
    {
      plGeometry geom;
      geom.AddRect(plVec2(PlaneSize), 8, 8);
      geom.TriangulatePolygons();
      geom.ComputeTangents();

      plMeshResourceDescriptor md;
      md.MeshBufferDesc().AddCommonStreams();
      md.MeshBufferDesc().AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);
      md.AddSubMesh(md.MeshBufferDesc().GetPrimitiveCount(), 0, 0);
      md.SetMaterial(0, "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }"); // Pattern.plMaterialAsset
      md.ComputeBounds();

      plMeshResourceDescriptor cpuDesc = md;
      plResourceManager::GetOrCreateResource<plCpuMeshResource>(szMeshName, std::move(cpuDesc), szMeshName);
      m_hPlaneMesh = plResourceManager::GetOrCreateResource<plMeshResource>(szMeshName, std::move(md), szMeshName);
    }
  }

  // The plane with grass grown from this document's type.
  {
    plGameObjectDesc obj;
    obj.m_sName.Assign("GrassTypePreview");
    pWorld->CreateObject(obj, m_pMainObject);

    const plTag& tagCastShadows = plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow");
    m_pMainObject->SetTag(tagCastShadows);

    plMeshComponent* pMesh = nullptr;
    plMeshComponent::CreateComponent(m_pMainObject, pMesh);
    pMesh->SetMesh(m_hPlaneMesh);

    plStringBuilder sGuid;
    plConversionUtils::ToString(GetDocumentGuid(), sGuid);
    m_hMainResource = plResourceManager::LoadResource<plGrassTypeResource>(sGuid);

    plGrassSurfaceComponent* pGrass = nullptr;
    m_hGrassComponent = plGrassSurfaceComponent::CreateComponent(m_pMainObject, pGrass);
    pGrass->SetMesh(m_hPlaneMesh);
    pGrass->SetGrassType(m_hMainResource);
  }

  // Wind, so the preview sways; strength is driven by the window's toolbar.
  {
    plGameObjectDesc obj;
    obj.m_sName.Assign("Wind");
    plGameObject* pObj;
    pWorld->CreateObject(obj, pObj);

    plSimpleWindComponent* pWind = nullptr;
    m_hWindComponent = plSimpleWindComponent::CreateComponent(pObj, pWind);
    pWind->m_Deviation = plAngle::MakeFromDegree(30);
    pWind->m_MinWindStrength = plWindStrength::LightBreeze;
    pWind->m_MaxWindStrength = plWindStrength::LightBreeze;
  }
}

plEngineProcessViewContext* plGrassTypeContext::CreateViewContext()
{
  return PL_DEFAULT_NEW(plGrassTypeViewContext, this);
}

void plGrassTypeContext::DestroyViewContext(plEngineProcessViewContext* pContext)
{
  PL_DEFAULT_DELETE(pContext);
}

plBoundingBoxSphere plGrassTypeContext::ComputePreviewBounds() const
{
  float fLength = 1.0f;

  plResourceLock<plGrassTypeResource> pType(m_hMainResource, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pType.GetAcquireResult() == plResourceAcquireResult::Final)
    fLength = pType->GetSettings().m_fLength;

  const float fHalfHeight = plMath::Max(fLength, 0.2f) * 0.5f;
  return plBoundingBoxSphere::MakeFromBox(plBoundingBox::MakeFromCenterAndHalfExtents(plVec3(0, 0, fHalfHeight), plVec3(PlaneSize * 0.5f, PlaneSize * 0.5f, fHalfHeight)));
}

bool plGrassTypeContext::UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext)
{
  plGrassTypeViewContext* pViewContext = static_cast<plGrassTypeViewContext*>(pThumbnailViewContext);
  return pViewContext->UpdateThumbnailCamera(ComputePreviewBounds());
}

void plGrassTypeContext::QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg)
{
  const plBoundingBoxSphere bounds = ComputePreviewBounds();

  const plQuerySelectionBBoxMsgToEngine* msg = static_cast<const plQuerySelectionBBoxMsgToEngine*>(pMsg);

  plQuerySelectionBBoxResultMsgToEditor res;
  res.m_uiViewID = msg->m_uiViewID;
  res.m_iPurpose = msg->m_iPurpose;
  res.m_vCenter = bounds.m_vCenter;
  res.m_vHalfExtents = bounds.m_vBoxHalfExtends;
  res.m_DocumentGuid = pMsg->m_DocumentGuid;

  SendProcessMessage(&res);
}
