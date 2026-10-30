#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <Core/Graphics/Geometry.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EnginePluginVegetation/VegetationTypeAsset/VegetationTypeContext.h>
#include <EnginePluginVegetation/VegetationTypeAsset/VegetationTypeView.h>
#include <GameEngine/Effects/Wind/SimpleWindComponent.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <VegetationPlugin/Components/VegetationInstanceComponent.h>
#include <VegetationPlugin/World/VegetationWorldModule.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeContext, 1, plRTTIDefaultAllocator<plVegetationTypeContext>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_CONSTANT_PROPERTY("DocumentType", (const char*) "Vegetation Type"),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTypeContext::plVegetationTypeContext()
  : plEngineProcessDocumentContext(plEngineProcessDocumentContextFlags::CreateWorld)
{
}

void plVegetationTypeContext::HandleMessage(const plEditorEngineDocumentMsg* pMsg0)
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
    else if (pMsg->m_sWhatToDo == "Touch")
    {
      // A one-off push through the plant from the front, like a character walking into it.
      if (plVegetationWorldModule* pModule = m_pWorld->GetOrCreateModule<plVegetationWorldModule>())
      {
        pModule->RegisterTouchSource(plVec3(-0.4f, 0.0f, 0.3f), 0.8f, 1.0f, plVec3(3.0f, 0.0f, 0.0f));
      }
    }

    return;
  }

  plEngineProcessDocumentContext::HandleMessage(pMsg0);
}

void plVegetationTypeContext::OnInitialize()
{
  auto pWorld = m_pWorld;
  PL_LOCK(pWorld->GetWriteMarker());

  // The plant: one instance of this document's type at the origin.
  {
    plGameObjectDesc obj;
    obj.m_sName.Assign("VegetationTypePreview");
    obj.m_bDynamic = true;
    pWorld->CreateObject(obj, m_pMainObject);

    const plTag& tagCastShadows = plTagRegistry::GetGlobalRegistry().RegisterTag("CastShadow");
    m_pMainObject->SetTag(tagCastShadows);

    plVegetationInstanceComponent* pInstance = nullptr;
    m_hInstanceComponent = plVegetationInstanceComponent::CreateComponent(m_pMainObject, pInstance);

    plStringBuilder sGuid;
    plConversionUtils::ToString(GetDocumentGuid(), sGuid);
    m_hMainResource = plResourceManager::LoadResource<plVegetationTypeResource>(sGuid);
    pInstance->SetType(m_hMainResource);
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

  // Ground disc, shared between documents.
  {
    const char* szMeshName = "VegetationPreviewGroundMesh";
    m_hPreviewMeshResource = plResourceManager::GetExistingResource<plMeshResource>(szMeshName);

    if (!m_hPreviewMeshResource.IsValid())
    {
      const char* szMeshBufferName = "VegetationPreviewGroundMeshBuffer";
      plMeshBufferResourceHandle hMeshBuffer = plResourceManager::GetExistingResource<plMeshBufferResource>(szMeshBufferName);

      if (!hMeshBuffer.IsValid())
      {
        plGeometry::GeoOptions opt;
        opt.m_Transform = plMat4::MakeTranslation(plVec3(0, 0, -0.05f));

        plGeometry geom;
        geom.AddCylinder(8.0f, 7.9f, 0.05f, 0.05f, true, true, 32, opt);
        geom.TriangulatePolygons();
        geom.ComputeTangents();

        plMeshBufferResourceDescriptor desc;
        desc.AddCommonStreams();
        desc.AllocateStreamsFromGeometry(geom, plGALPrimitiveTopology::Triangles);
        hMeshBuffer = plResourceManager::GetOrCreateResource<plMeshBufferResource>(szMeshBufferName, std::move(desc), szMeshBufferName);
      }

      {
        plResourceLock<plMeshBufferResource> pMeshBuffer(hMeshBuffer, plResourceAcquireMode::AllowLoadingFallback);
        plMeshResourceDescriptor md;
        md.UseExistingMeshBuffer(hMeshBuffer);
        md.AddSubMesh(pMeshBuffer->GetPrimitiveCount(), 0, 0);
        md.SetMaterial(0, "{ 1c47ee4c-0379-4280-85f5-b8cda61941d2 }"); // Pattern.plMaterialAsset
        md.ComputeBounds();
        m_hPreviewMeshResource = plResourceManager::GetOrCreateResource<plMeshResource>(szMeshName, std::move(md), pMeshBuffer->GetResourceDescription());
      }
    }

    plGameObjectDesc obj;
    obj.m_sName.Assign("VegetationGround");
    plGameObject* pObj;
    pWorld->CreateObject(obj, pObj);

    plMeshComponent* pMesh;
    plMeshComponent::CreateComponent(pObj, pMesh);
    pMesh->SetMesh(m_hPreviewMeshResource);
  }
}

plEngineProcessViewContext* plVegetationTypeContext::CreateViewContext()
{
  return PL_DEFAULT_NEW(plVegetationTypeViewContext, this);
}

void plVegetationTypeContext::DestroyViewContext(plEngineProcessViewContext* pContext)
{
  PL_DEFAULT_DELETE(pContext);
}

plBoundingBoxSphere plVegetationTypeContext::ComputePreviewBounds() const
{
  plBoundingBoxSphere bounds = plBoundingBoxSphere::MakeFromCenterExtents(plVec3(0, 0, 1), plVec3(1.0f), 1.5f);

  plResourceLock<plVegetationTypeResource> pType(m_hMainResource, plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pType.GetAcquireResult() != plResourceAcquireResult::Final || pType->GetLodCount() == 0)
    return bounds;

  plResourceLock<plMeshResource> pMesh(pType->GetLodMesh(0), plResourceAcquireMode::BlockTillLoaded_NeverFail);
  if (pMesh.GetAcquireResult() != plResourceAcquireResult::Final || !pMesh->GetBounds().IsValid())
    return bounds;

  return pMesh->GetBounds();
}

bool plVegetationTypeContext::UpdateThumbnailViewContext(plEngineProcessViewContext* pThumbnailViewContext)
{
  plVegetationTypeViewContext* pViewContext = static_cast<plVegetationTypeViewContext*>(pThumbnailViewContext);
  return pViewContext->UpdateThumbnailCamera(ComputePreviewBounds());
}

void plVegetationTypeContext::QuerySelectionBBox(const plEditorEngineDocumentMsg* pMsg)
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
