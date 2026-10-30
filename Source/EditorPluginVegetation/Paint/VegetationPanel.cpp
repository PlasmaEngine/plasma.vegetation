#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/EngineShell/DocumentContent.h>
#include <EditorFramework/EngineShell/DocumentFrame.h>
#include <EditorFramework/EngineShell/EngineShell.h>
#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>
#include <EditorPluginVegetation/Paint/VegetationPaintEditTool.h>
#include <EditorPluginVegetation/Paint/VegetationPanel.h>
#include <EditorPluginVegetation/Paint/VegetationPanelView.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/CommandHistory/CommandHistory.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

namespace
{
  /// The panel in a scene's frame in the engine shell.
  constexpr const char* s_szShellPanelId = "Vegetation";
  constexpr const char* s_szPanelIcon = ":/EditorPluginVegetation/Paint.svg";

  bool IsSceneDocument(const plDocument* pDocument)
  {
    // walk the type chain by name, as for every scene type
    for (const plRTTI* pRtti = pDocument->GetDynamicRTTI(); pRtti != nullptr; pRtti = pRtti->GetParentType())
    {
      if (pRtti->GetTypeName() == "plSceneDocument")
        return true;
    }

    return false;
  }

  /// Brings the panel forward in the document's frame in the engine shell.
  void RaiseShellPanel(const plDocument* pDocument)
  {
    plEditorUiEngineShell* pShell = plEditorUiEngineShell::GetSingleton();
    plEditorUiDocumentFrame* pFrame = pShell != nullptr ? pShell->FindFrame(pDocument) : nullptr;
    if (pFrame == nullptr || pFrame->GetPanels().GetPanelTitle(s_szShellPanelId).IsEmpty())
      return;

    // opening a closed panel makes it the current tab
    plEditorUiDockManager& panels = pFrame->GetPanels();
    if (panels.IsPanelOpen(s_szShellPanelId))
      panels.SetCurrentPanel(s_szShellPanelId);
    else
      panels.OpenPanel(s_szShellPanelId);
  }
} // namespace

plVegetationPanel::plVegetationPanel(plDocument* pDocument)
  : m_pDocument(pDocument)
{
  if (auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameObjectDoc->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plVegetationPanel::GameObjectEventHandler, this));
  }
  m_pDocument->GetObjectManager()->m_StructureEvents.AddEventHandler(plMakeDelegate(&plVegetationPanel::StructureEventHandler, this));
  m_pDocument->GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plVegetationPanel::PropertyEventHandler, this));
  m_pDocument->GetCommandHistory()->m_Events.AddEventHandler(plMakeDelegate(&plVegetationPanel::CommandHistoryEventHandler, this));
  if (auto* pAssetDoc = plDynamicCast<plAssetDocument*>(m_pDocument))
  {
    pAssetDoc->m_ProcessMessageEvent.AddEventHandler(plMakeDelegate(&plVegetationPanel::ProcessMessageEventHandler, this));
  }
  plVegetationBrush::s_Events.AddEventHandler(plMakeDelegate(&plVegetationPanel::BrushEventHandler, this));

  RebuildTypeList();
  RebuildLayerList();
  UpdateTypeDependentUi();
  UpdateStatistics();
}

plVegetationPanel::~plVegetationPanel()
{
  if (auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    pGameObjectDoc->m_GameObjectEvents.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::GameObjectEventHandler, this));
  }
  m_pDocument->GetObjectManager()->m_StructureEvents.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::StructureEventHandler, this));
  m_pDocument->GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::PropertyEventHandler, this));
  m_pDocument->GetCommandHistory()->m_Events.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::CommandHistoryEventHandler, this));
  if (auto* pAssetDoc = plDynamicCast<plAssetDocument*>(m_pDocument))
  {
    pAssetDoc->m_ProcessMessageEvent.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::ProcessMessageEventHandler, this));
  }
  plVegetationBrush::s_Events.RemoveEventHandler(plMakeDelegate(&plVegetationPanel::BrushEventHandler, this));
}

void plVegetationPanel::SetView(plVegetationPanelView* pView)
{
  m_pView = pView;
  RefreshView();
}

void plVegetationPanel::CollectLayerComponents(const plDocumentObject* pObject, plDynamicArray<const plDocumentObject*>& out_components) const
{
  if (pObject == nullptr)
    return;

  for (const plDocumentObject* pChild : pObject->GetChildren())
  {
    if (pChild->GetType()->GetTypeName() == "plVegetationLayerComponent")
    {
      out_components.PushBack(pChild);
    }

    CollectLayerComponents(pChild, out_components);
  }
}

void plVegetationPanel::RebuildLayerList()
{
  plDynamicArray<const plDocumentObject*> components;
  CollectLayerComponents(m_pDocument->GetObjectManager()->GetRootObject(), components);

  m_LayerNames.Clear();
  m_LayerGuids.Clear();
  m_LayerNames.PushBack("(no layer)");
  m_LayerGuids.PushBack(plUuid());

  int iSelected = 0;
  for (const plDocumentObject* pComponent : components)
  {
    plStringBuilder sName = pComponent->GetParent()->GetTypeAccessor().GetValue("Name").ConvertTo<plString>();
    if (sName.IsEmpty())
      sName = "Vegetation Layer";

    m_LayerNames.PushBack(sName);
    m_LayerGuids.PushBack(pComponent->GetGuid());

    if (pComponent->GetGuid() == plVegetationBrush::s_Settings.m_LayerComponent)
    {
      iSelected = static_cast<int>(m_LayerNames.GetCount()) - 1;
    }
  }

  // A single layer is the obvious target.
  if (iSelected == 0 && components.GetCount() == 1)
  {
    iSelected = 1;
    plVegetationBrush::s_Settings.m_LayerComponent = components[0]->GetGuid();
  }

  m_iLayerIndex = iSelected;

  RefreshView();
}

void plVegetationPanel::RebuildTypeList()
{
  auto& settings = plVegetationBrush::s_Settings;

  // keep selections of types that still exist
  plHybridArray<plVegetationBrushType, 16> previous = settings.m_Types;
  settings.m_Types.Clear();

  // plants first, then grass; the list shows them in two groups
  for (const bool bGrass : {false, true})
  {
    auto assets = plAssetCurator::GetSingleton()->GetKnownSubAssets();
    for (auto it = assets->GetIterator(); it.IsValid(); ++it)
    {
      const plSubAsset& subAsset = it.Value();
      if (subAsset.m_Data.m_sSubAssetsDocumentTypeName != (bGrass ? "Grass Type" : "Vegetation Type") || !subAsset.m_bMainAsset)
        continue;

      plVegetationBrushType& type = settings.m_Types.ExpandAndGetRef();
      plStringBuilder sGuid;
      plConversionUtils::ToString(subAsset.m_Data.m_Guid, sGuid);
      type.m_sGuid = sGuid;
      type.m_sName = subAsset.GetName();
      type.m_bGrass = bGrass;

      for (const auto& prev : previous)
      {
        if (prev.m_sGuid == type.m_sGuid)
        {
          type.m_bSelected = prev.m_bSelected;
          type.m_fWeight = prev.m_fWeight;
        }
      }
    }
  }

  RefreshView();
}

void plVegetationPanel::UpdateTypeDependentUi()
{
  auto& s = plVegetationBrush::s_Settings;

  if (!s.IsPlaceAvailable() && s.m_Mode == plVegetationBrushMode::Place)
  {
    s.m_Mode = plVegetationBrushMode::Paint;
    plVegetationBrush::NotifySettingsChanged();
  }

  RefreshView();
}

const plDocumentObject* plVegetationPanel::GetTargetLayer() const
{
  const plUuid guid = plVegetationBrush::s_Settings.m_LayerComponent;
  if (!guid.IsValid())
    return nullptr;

  const plDocumentObject* pObject = m_pDocument->GetObjectManager()->GetObject(guid);
  if (pObject == nullptr || pObject->GetParent() == nullptr || pObject->GetType()->GetTypeName() != "plVegetationLayerComponent")
    return nullptr;

  return pObject;
}

float plVegetationPanel::GetStrandsPerSquareMeter(plStringView sGrassType) const
{
  // Non-blocking: until the type has loaded, the estimate uses the Grass Type default.
  plGrassTypeResourceHandle hType = plResourceManager::LoadResource<plGrassTypeResource>(sGrassType);
  plResourceManager::PreloadResource(hType);

  plResourceLock<plGrassTypeResource> pType(hType, plResourceAcquireMode::PointerOnly);
  if (pType.GetPointer() != nullptr && pType->GetLoadingState() == plResourceState::Loaded)
  {
    return pType->GetSettings().m_fStrandsPerSquareMeter;
  }

  return plGrassSettings().m_fStrandsPerSquareMeter;
}

void plVegetationPanel::UpdateStatistics()
{
  plStringBuilder sText;

  const plDocumentObject* pComponent = GetTargetLayer();

  if (pComponent == nullptr)
  {
    sText = "No layer selected.";
  }
  else
  {
    plVegetationLayerData data;
    const plVariant value = pComponent->GetTypeAccessor().GetValue("Data");
    if (value.IsA<plDataBuffer>())
    {
      data.FromBuffer(value.Get<plDataBuffer>()).IgnoreResult();
    }

    plGrassCoverageData coverage;
    const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
    const plUInt32 uiCoverageBytes = grass.IsA<plDataBuffer>() ? grass.Get<plDataBuffer>().GetCount() : 0;
    if (grass.IsA<plDataBuffer>() && coverage.FromBuffer(grass.Get<plDataBuffer>()).Failed())
    {
      coverage.Clear();
    }

    plUInt64 uiStrands = 0;
    for (const plGrassCoverageEntry& entry : coverage.m_Entries)
    {
      uiStrands += coverage.CountStrands(entry, GetStrandsPerSquareMeter(entry.m_sType));
    }

    sText.SetFormat("Plant instances: {}\nPlant types: {}\nPlant blob: {} KB\nCoverage tiles: {}\nPainted area: {} m\xC2\xB2\nEst. strands: {}\nCoverage blob: {} KB",
      data.GetInstanceCount(), data.m_Entries.GetCount(), value.IsA<plDataBuffer>() ? value.Get<plDataBuffer>().GetCount() / 1024 : 0,
      coverage.GetTileCount(), plArgF(coverage.GetPaintedArea(), 0), uiStrands, plArgF(uiCoverageBytes / 1024.0, 1));
  }

  m_sStatistics = sText;

  RefreshView();
}

bool plVegetationPanel::CanResnap() const
{
  return GetTargetLayer() != nullptr && m_uiResnapRequest == 0;
}

bool plVegetationPanel::CanClearCoverage() const
{
  return GetTargetLayer() != nullptr;
}

plUInt32 plVegetationPanel::GetInstanceCount(plStringView sTypeGuid) const
{
  const plDocumentObject* pComponent = GetTargetLayer();
  if (pComponent == nullptr)
    return 0;

  const plVariant value = pComponent->GetTypeAccessor().GetValue("Data");
  if (!value.IsA<plDataBuffer>())
    return 0;

  plVegetationLayerData data;
  if (data.FromBuffer(value.Get<plDataBuffer>()).Failed())
    return 0;

  for (const plVegetationLayerEntry& entry : data.m_Entries)
  {
    if (entry.m_sType == sTypeGuid)
      return entry.m_Instances.GetCount();
  }

  return 0;
}

bool plVegetationPanel::IsToolActive() const
{
  auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);
  return pGameObjectDoc != nullptr && pGameObjectDoc->IsActiveEditTool(plGetStaticRTTI<plVegetationPaintEditTool>());
}

void plVegetationPanel::ToggleTool()
{
  // the button reads "Vegetation Tool active" while the tool is on; clicking it leaves the tool
  if (auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument))
  {
    if (IsToolActive())
      pGameObjectDoc->SetActiveEditTool(nullptr);
    else
      pGameObjectDoc->SetActiveEditTool(plGetStaticRTTI<plVegetationPaintEditTool>());
  }
}

void plVegetationPanel::OnTypesChanged()
{
  UpdateTypeDependentUi();
  plVegetationBrush::NotifySettingsChanged();
}

void plVegetationPanel::RefreshView()
{
  if (m_pView != nullptr)
    m_pView->Refresh();
}

void plVegetationPanel::OnLayerChanged(int iIndex)
{
  if (iIndex < 0 || iIndex >= static_cast<int>(m_LayerGuids.GetCount()))
    return;

  m_iLayerIndex = iIndex;
  plVegetationBrush::s_Settings.m_LayerComponent = m_LayerGuids[iIndex];
  UpdateStatistics();
  plVegetationBrush::NotifySettingsChanged();
}

void plVegetationPanel::OnNewLayerClicked()
{
  plCommandHistory* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Add Vegetation Layer");

  plUuid objectGuid = plUuid::MakeUuid();
  {
    plAddObjectCommand cmd;
    cmd.m_pType = plGetStaticRTTI<plGameObject>();
    cmd.m_sParentProperty = "Children";
    cmd.m_Index = -1;
    cmd.m_NewObjectGuid = objectGuid;
    if (pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
      return;
    }
  }
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = objectGuid;
    cmd.m_sProperty = "Name";
    cmd.m_NewValue = "Vegetation Layer";
    pHistory->AddCommand(cmd).IgnoreResult();
  }

  plUuid componentGuid = plUuid::MakeUuid();
  {
    plAddObjectCommand cmd;
    cmd.m_pType = plRTTI::FindTypeByName("plVegetationLayerComponent");
    cmd.m_sParentProperty = "Components";
    cmd.m_Parent = objectGuid;
    cmd.m_Index = -1;
    cmd.m_NewObjectGuid = componentGuid;
    if (cmd.m_pType == nullptr || pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
      return;
    }
  }
  {
    // new layers use the texel size last chosen on a layer
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = componentGuid;
    cmd.m_sProperty = "GrassTexelSize";
    cmd.m_NewValue = plGrassCoverageData::ClampTexelSize(plVegetationBrush::s_Settings.m_fLastTexelSize);
    pHistory->AddCommand(cmd).IgnoreResult();
  }

  pHistory->FinishTransaction();

  plVegetationBrush::s_Settings.m_LayerComponent = componentGuid;
  RebuildLayerList();
  UpdateStatistics();
  plVegetationBrush::NotifySettingsChanged();
}

void plVegetationPanel::OnRefreshTypesClicked()
{
  RebuildTypeList();
  UpdateTypeDependentUi();
  UpdateStatistics();
}

void plVegetationPanel::OnResnapClicked()
{
  const plDocumentObject* pComponent = GetTargetLayer();
  auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);
  if (pComponent == nullptr || pGameObjectDoc == nullptr || m_uiResnapRequest != 0)
    return;

  const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
  if (!grass.IsA<plDataBuffer>() || grass.Get<plDataBuffer>().IsEmpty())
    return;

  static plUInt32 s_uiNextRequest = 0;
  if (++s_uiNextRequest == 0)
    ++s_uiNextRequest;

  plVegetationResnapRequest request;
  request.m_LayerGuid = pComponent->GetGuid();
  request.m_uiRequestId = s_uiNextRequest;
  request.m_LayerTransform = pGameObjectDoc->GetGlobalTransform(pComponent->GetParent());
  request.m_Coverage = grass.Get<plDataBuffer>();

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = plVegetationResnapRequest::s_szWhatToDo;
  plDataBuffer buffer;
  request.ToBuffer(buffer);
  msg.m_PayloadValue = buffer;

  if (pGameObjectDoc->SendMessageToEngine(&msg))
  {
    m_uiResnapRequest = request.m_uiRequestId;
    UpdateStatistics();
  }
}

void plVegetationPanel::OnClearCoverageClicked()
{
  const plDocumentObject* pComponent = GetTargetLayer();
  if (pComponent == nullptr)
    return;

  const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
  if (!grass.IsA<plDataBuffer>() || grass.Get<plDataBuffer>().IsEmpty())
    return;

  plCommandHistory* pHistory = m_pDocument->GetCommandHistory();
  pHistory->StartTransaction("Clear Coverage");

  plSetObjectPropertyCommand cmd;
  cmd.m_Object = pComponent->GetGuid();
  cmd.m_sProperty = "GrassCoverage";
  cmd.m_NewValue = plDataBuffer();

  if (pHistory->AddCommand(cmd).Failed())
  {
    pHistory->CancelTransaction();
    return;
  }

  pHistory->FinishTransaction();
  UpdateStatistics();
}

void plVegetationPanel::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
{
  const auto* pConfig = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg);
  if (pConfig == nullptr || pConfig->m_sWhatToDo != plVegetationResnapResult::s_szWhatToDo || !pConfig->m_PayloadValue.IsA<plDataBuffer>())
    return;

  plVegetationResnapResult result;
  if (result.FromBuffer(pConfig->m_PayloadValue.Get<plDataBuffer>()).Failed() || result.m_uiRequestId != m_uiResnapRequest)
    return;

  m_uiResnapRequest = 0;

  const plDocumentObject* pComponent = m_pDocument->GetObjectManager()->GetObject(result.m_LayerGuid);
  if (pComponent != nullptr && result.m_uiResnappedTexels > 0)
  {
    plCommandHistory* pHistory = m_pDocument->GetCommandHistory();
    pHistory->StartTransaction("Resnap Coverage");

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = result.m_LayerGuid;
    cmd.m_sProperty = "GrassCoverage";
    cmd.m_NewValue = result.m_Coverage;

    if (pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
    }
    else
    {
      pHistory->FinishTransaction();
    }
  }

  plLog::Info("Vegetation: resnapped {} coverage texels.", result.m_uiResnappedTexels);
  UpdateStatistics();
}

void plVegetationPanel::CommandHistoryEventHandler(const plCommandHistoryEvent& e)
{
  if (e.m_Type == plCommandHistoryEvent::Type::BeforeTransactionEnded)
  {
    ResampleChangedLayers();
  }
  else if (e.m_Type == plCommandHistoryEvent::Type::TransactionCanceled || e.m_Type == plCommandHistoryEvent::Type::TransactionEnded)
  {
    m_TexelSizeChanged.Clear();
  }
}

void plVegetationPanel::ResampleChangedLayers()
{
  if (m_TexelSizeChanged.IsEmpty())
    return;

  // Runs inside the still open transaction, so the texel size and the resampled coverage undo together.
  plHybridArray<plUuid, 4> layers = m_TexelSizeChanged;
  m_TexelSizeChanged.Clear();

  plCommandHistory* pHistory = m_pDocument->GetCommandHistory();

  for (const plUuid& guid : layers)
  {
    const plDocumentObject* pComponent = m_pDocument->GetObjectManager()->GetObject(guid);
    if (pComponent == nullptr)
      continue;

    const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
    if (!grass.IsA<plDataBuffer>() || grass.Get<plDataBuffer>().IsEmpty())
      continue;

    plGrassCoverageData coverage;
    if (coverage.FromBuffer(grass.Get<plDataBuffer>()).Failed())
      continue;

    const plVariant texelSize = pComponent->GetTypeAccessor().GetValue("GrassTexelSize");
    const float fTexelSize = plGrassCoverageData::ClampTexelSize(texelSize.CanConvertTo<float>() ? texelSize.ConvertTo<float>() : plGrassCoverageData::DefaultTexelSize);
    if (plMath::Abs(coverage.m_fTexelSize - fTexelSize) < 0.00001f)
      continue;

    coverage.Resample(fTexelSize);

    plDataBuffer buffer;
    coverage.ToBuffer(buffer);

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = guid;
    cmd.m_sProperty = "GrassCoverage";
    cmd.m_NewValue = buffer;
    pHistory->AddCommand(cmd).IgnoreResult();
  }
}

void plVegetationPanel::GameObjectEventHandler(const plGameObjectEvent& e)
{
  if (e.m_Type != plGameObjectEvent::Type::ActiveEditToolChanged)
    return;

  auto* pGameObjectDoc = plDynamicCast<plGameObjectDocument*>(m_pDocument);
  if (pGameObjectDoc == nullptr)
    return;

  // the toolbar button activated the tool: bring the panel forward so its settings are at hand
  if (pGameObjectDoc->IsActiveEditTool(plGetStaticRTTI<plVegetationPaintEditTool>()))
  {
    RaiseShellPanel(m_pDocument);
    RebuildLayerList();
    UpdateStatistics();
  }

  // the tool button shows whether the tool is active
  RefreshView();
}

void plVegetationPanel::BrushEventHandler(const plVegetationBrushEvent& e)
{
  switch (e.m_Type)
  {
    case plVegetationBrushEvent::Type::SettingsChanged:
      RefreshView();
      break;
    case plVegetationBrushEvent::Type::StrokeApplied:
      UpdateStatistics();
      break;
  }
}

void plVegetationPanel::StructureEventHandler(const plDocumentObjectStructureEvent& e)
{
  if (e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectAdded || e.m_EventType == plDocumentObjectStructureEvent::Type::AfterObjectRemoved)
  {
    if (e.m_pObject != nullptr && e.m_pObject->GetType()->GetTypeName() == "plVegetationLayerComponent")
    {
      RebuildLayerList();
      UpdateStatistics();
    }
  }
}

void plVegetationPanel::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_pObject == nullptr)
    return;

  if (e.m_sProperty == "GrassTexelSize" && e.m_pObject->GetType()->GetTypeName() == "plVegetationLayerComponent")
  {
    const plCommandHistory* pHistory = m_pDocument->GetCommandHistory();
    if (!pHistory->IsInUndoRedo() && pHistory->IsInTransaction())
    {
      if (!m_TexelSizeChanged.Contains(e.m_pObject->GetGuid()))
        m_TexelSizeChanged.PushBack(e.m_pObject->GetGuid());

      if (e.m_NewValue.CanConvertTo<float>())
        plVegetationBrush::s_Settings.m_fLastTexelSize = e.m_NewValue.ConvertTo<float>();
    }
  }

  if (e.m_pObject->GetGuid() == plVegetationBrush::s_Settings.m_LayerComponent && (e.m_sProperty == "Data" || e.m_sProperty == "GrassCoverage" || e.m_sProperty == "GrassTexelSize"))
  {
    UpdateStatistics();
  }
}

//////////////////////////////////////////////////////////////////////////

void plVegetationPanelManager::Startup()
{
  plEditorUiDocumentContentProvider::RegisterExtraPanels(plGetStaticRTTI<plGameObjectDocument>(), &plVegetationPanelManager::AddShellPanel);
}

void plVegetationPanelManager::Shutdown()
{
  plEditorUiDocumentContentProvider::UnregisterExtraPanels(&plVegetationPanelManager::AddShellPanel);
}

void plVegetationPanelManager::AddShellPanel(plEditorUiDocumentFrame& ref_frame)
{
  if (!IsSceneDocument(ref_frame.GetDocument()))
    return;

  // the frame owns the content, which owns the panel's logic
  if (ref_frame.AddContent({s_szShellPanelId, "Vegetation"}, PL_DEFAULT_NEW(plVegetationPanelView, ref_frame.GetDocument())) == nullptr)
    return;

  // a tab beside the properties; the properties stay in front
  plEditorUiDockManager& panels = ref_frame.GetPanels();
  panels.SetPanelIcon(s_szShellPanelId, s_szPanelIcon);
  panels.DockPanel(s_szShellPanelId, "Properties", plEditorUiDockSide::Tab);
  panels.SetCurrentPanel("Properties");
}
