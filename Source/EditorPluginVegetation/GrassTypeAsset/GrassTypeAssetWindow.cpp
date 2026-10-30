#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/DocumentWindow/OrbitCamViewWidget.h>
#include <EditorFramework/InputContexts/OrbitCameraContext.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAssetWindow.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Utilities/AssetFileHeader.h>

plGrassTypeAssetDocumentWindow::plGrassTypeAssetDocumentWindow(plAssetDocument* pDocument)
  : plEngineDocumentWindow(pDocument)
{
  SetMenuBarActions("GrassTypeAssetMenuBar");
  SetToolBarActions("GrassTypeAssetToolBar");

  // 3D View: the engine process renders the grass patch, this widget only hosts and orbits it.
  {
    SetTargetFramerate(30);

    m_ViewConfig.m_Camera.LookAt(plVec3(-4.0f, -3.0f, 2.0f), plVec3(0, 0, 0.3f), plVec3(0, 0, 1));
    m_ViewConfig.ApplyPerspectiveSetting(90);

    m_pViewWidget = new plOrbitCamViewWidget(this, &m_ViewConfig);
    m_pViewWidget->ConfigureRelative(plVec3(0, 0, 0.3f), plVec3(10.0f), plVec3(5, -2, 3), 4.0f);
    AddViewWidget(m_pViewWidget);
    m_pViewWidget->SetToolBarActions("MeshAssetViewToolBar");
  }

  // the shell's properties panel follows the selection
  pDocument->GetSelectionManager()->SetSelection(pDocument->GetObjectManager()->GetRootObject()->GetChildren()[0]);

  GetDocument()->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentWindow::StructureEventHandler, this));

  UpdatePreview();
}

plGrassTypeAssetDocumentWindow::~plGrassTypeAssetDocumentWindow()
{
  GetDocument()->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentWindow::PropertyEventHandler, this));
  GetDocument()->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentWindow::StructureEventHandler, this));

  RestoreResource();
}

void plGrassTypeAssetDocumentWindow::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  PL_IGNORE_UNUSED(e);
  UpdatePreview();
}

void plGrassTypeAssetDocumentWindow::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  if (e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectAdded || e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectRemoved || e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectMoved)
  {
    UpdatePreview();
  }
}

void plGrassTypeAssetDocumentWindow::SendRedrawMsg()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  for (auto pView : m_ViewWidgets)
  {
    pView->SetEnablePicking(false);
    pView->UpdateCameraInterpolation();
    pView->SyncToEngine();
  }

  QueryObjectBBox();
}

void plGrassTypeAssetDocumentWindow::QueryObjectBBox(plInt32 iPurpose /*= 0*/)
{
  plQuerySelectionBBoxMsgToEngine msg;
  msg.m_uiViewID = 0xFFFFFFFF;
  msg.m_iPurpose = iPurpose;
  GetDocument()->SendMessageToEngine(&msg);
}

void plGrassTypeAssetDocumentWindow::InternalRedraw()
{
  plEditorInputContext::UpdateActiveInputContext();
  SendRedrawMsg();
  plEngineDocumentWindow::InternalRedraw();
}

void plGrassTypeAssetDocumentWindow::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
{
  if (pMsg->GetDynamicRTTI()->IsDerivedFrom<plQuerySelectionBBoxResultMsgToEditor>())
  {
    const plQuerySelectionBBoxResultMsgToEditor* pMessage = static_cast<const plQuerySelectionBBoxResultMsgToEditor*>(pMsg);

    if (pMessage->m_vCenter.IsValid() && pMessage->m_vHalfExtents.IsValid())
    {
      m_pViewWidget->SetOrbitVolume(pMessage->m_vCenter, pMessage->m_vHalfExtents.CompMax(plVec3(0.1f)));
    }
    else
    {
      QueryObjectBBox(pMessage->m_iPurpose);
    }

    return;
  }

  plEngineDocumentWindow::ProcessMessageEventHandler(pMsg);
}

void plGrassTypeAssetDocumentWindow::RestoreResource()
{
  plRestoreResourceMsgToEngine msg;
  msg.m_sResourceType = "Grass Type";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}

void plGrassTypeAssetDocumentWindow::UpdatePreview()
{
  if (plEditorEngineProcessConnection::GetSingleton()->IsProcessCrashed())
    return;

  plResourceUpdateMsgToEngine msg;
  msg.m_sResourceType = "Grass Type";

  plStringBuilder tmp;
  msg.m_sResourceID = plConversionUtils::ToString(GetDocument()->GetGuid(), tmp);

  plContiguousMemoryStreamStorage streamStorage;
  plMemoryStreamWriter memoryWriter(&streamStorage);

  // Same layout as the transformed file: path, asset header, descriptor.
  plStringBuilder sAbsFilePath = GetDocument()->GetDocumentPath();
  sAbsFilePath.ChangeFileExtension("plBinGrassType");
  memoryWriter << sAbsFilePath;

  const plUInt64 uiHash = plAssetCurator::GetSingleton()->GetAssetTransformHash(GetDocument()->GetGuid());
  plAssetFileHeader AssetHeader;
  AssetHeader.SetFileHashAndVersion(uiHash, GetDocument()->GetAssetTypeVersion());
  AssetHeader.Write(memoryWriter).AssertSuccess();

  GetGrassDocument()->WriteGrassType(memoryWriter).IgnoreResult();
  msg.m_Data = plArrayPtr<const plUInt8>(streamStorage.GetData(), streamStorage.GetStorageSize32());

  plEditorEngineProcessConnection::GetSingleton()->SendMessage(&msg);
}
