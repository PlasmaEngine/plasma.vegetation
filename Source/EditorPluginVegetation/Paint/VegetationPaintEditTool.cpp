#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorFramework/DocumentWindow/EngineDocumentWindow.h>
#include <EditorFramework/DocumentWindow/GameObjectDocumentWindow.h>
#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>
#include <EditorPluginVegetation/Paint/VegetationPaintEditTool.h>
#include <EditorFramework/DocumentWindow/EngineViewWidget.h>
#include <Foundation/Time/Time.h>
#include <GuiFoundation/PropertyGrid/ManipulatorManager.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationPaintEditTool, 1, plRTTIDefaultAllocator<plVegetationPaintEditTool>)
PL_END_DYNAMIC_REFLECTED_TYPE;

plVegetationPaintEditTool::plVegetationPaintEditTool()
{
  m_BrushContext.SetTool(this);
  RollPlaceSeed();
}

plVegetationPaintEditTool::~plVegetationPaintEditTool()
{
  if (GetDocument() != nullptr)
  {
    GetDocument()->m_GameObjectEvents.RemoveEventHandler(plMakeDelegate(&plVegetationPaintEditTool::GameObjectEventHandler, this));
    GetDocument()->m_ProcessMessageEvent.RemoveEventHandler(plMakeDelegate(&plVegetationPaintEditTool::ProcessMessageEventHandler, this));
    plManipulatorManager::GetSingleton()->m_Events.RemoveEventHandler(plMakeDelegate(&plVegetationPaintEditTool::ManipulatorManagerEventHandler, this));
    plVegetationBrush::s_Events.RemoveEventHandler(plMakeDelegate(&plVegetationPaintEditTool::BrushEventHandler, this));
  }
}

plEditorInputContext* plVegetationPaintEditTool::GetEditorInputContextOverride()
{
  if (IsActive())
    return &m_BrushContext;

  return nullptr;
}

plEditToolSupportedSpaces plVegetationPaintEditTool::GetSupportedSpaces() const
{
  return plEditToolSupportedSpaces::WorldSpaceOnly;
}

bool plVegetationPaintEditTool::GetSupportsMoveParentOnly() const
{
  return false;
}

void plVegetationPaintEditTool::OnConfigured()
{
  GetDocument()->m_GameObjectEvents.AddEventHandler(plMakeDelegate(&plVegetationPaintEditTool::GameObjectEventHandler, this));
  GetDocument()->m_ProcessMessageEvent.AddEventHandler(plMakeDelegate(&plVegetationPaintEditTool::ProcessMessageEventHandler, this));
  plManipulatorManager::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plVegetationPaintEditTool::ManipulatorManagerEventHandler, this));
  plVegetationBrush::s_Events.AddEventHandler(plMakeDelegate(&plVegetationPaintEditTool::BrushEventHandler, this));

  m_BrushContext.SetOwner(GetWindow(), nullptr);
}

void plVegetationPaintEditTool::OnActiveChanged(bool bIsActive)
{
  if (!bIsActive && m_bStrokeActive)
  {
    EndStroke();
  }

  UpdateGizmoState();

  // The place ghost is translucent; with transparent picking off the brush cursor cannot land on the ghost itself.
  SetPickTransparent(!bIsActive);

  if (bIsActive)
  {
    m_BrushContext.UpdateStatusBarText(GetWindow());
  }
  else
  {
    HidePlacePreview();
    DropStrokePreview();
    GetWindow()->SetPermanentStatusBarMsg("");
  }

  SendOverlay();
}

void plVegetationPaintEditTool::SendOverlay()
{
  plUuid layer;
  if (IsActive())
  {
    if (const plDocumentObject* pComponent = GetLayerComponentObject())
      layer = pComponent->GetGuid();
  }

  if (layer == m_OverlayLayer)
    return;

  m_OverlayLayer = layer;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = plVegetationOverlayMsg::s_szWhatToDo;
  msg.m_PayloadValue = layer;
  GetDocument()->SendMessageToEngine(&msg);
}

void plVegetationPaintEditTool::SetPickTransparent(bool bEnable)
{
  for (plEngineViewWidget* pView : GetWindow()->GetViewWidgets())
  {
    pView->SetPickTransparent(bEnable);
  }
}

void plVegetationPaintEditTool::BrushEventHandler(const plVegetationBrushEvent& e)
{
  if (e.m_Type != plVegetationBrushEvent::Type::SettingsChanged || !IsActive())
    return;

  // the target layer may have changed
  SendOverlay();

  // mode, palette or scale range changed: the ghost follows, or goes away outside place mode
  if (plVegetationBrush::s_Settings.m_Mode == plVegetationBrushMode::Place)
  {
    if (m_bPreviewShown)
      UpdatePlacePreview(m_vPreviewCenter, m_vPreviewNormal);
  }
  else
  {
    HidePlacePreview();
  }
}

void plVegetationPaintEditTool::UpdateGizmoState()
{
  plManipulatorManager::GetSingleton()->HideActiveManipulator(GetDocument(), GetDocument()->GetActiveEditTool() != nullptr);

  m_BrushContext.SetVisible(IsActive());
  m_BrushContext.SetTransformation(plTransform::MakeIdentity());
}

void plVegetationPaintEditTool::GameObjectEventHandler(const plGameObjectEvent& e)
{
  if (e.m_Type == plGameObjectEvent::Type::ActiveEditToolChanged)
  {
    UpdateGizmoState();
  }
}

void plVegetationPaintEditTool::ManipulatorManagerEventHandler(const plManipulatorManagerEvent& e)
{
  if (!IsActive())
    return;

  if (e.m_pDocument == GetDocument() && e.m_pManipulator != nullptr && e.m_pSelection != nullptr && !e.m_pSelection->IsEmpty() && !e.m_bHideManipulators)
  {
    GetDocument()->SetActiveEditTool(nullptr);
  }
}

void plVegetationPaintEditTool::Deactivate()
{
  GetDocument()->SetActiveEditTool(nullptr);
}

const plDocumentObject* plVegetationPaintEditTool::GetLayerComponentObject() const
{
  return GetLayerComponentObject(plVegetationBrush::s_Settings.m_LayerComponent);
}

const plDocumentObject* plVegetationPaintEditTool::GetLayerComponentObject(const plUuid& layer) const
{
  if (!layer.IsValid())
    return nullptr;

  const plDocumentObject* pObject = GetDocument()->GetObjectManager()->GetObject(layer);
  if (pObject == nullptr || pObject->GetParent() == nullptr)
    return nullptr;

  if (pObject->GetType()->GetTypeName() != "plVegetationLayerComponent")
    return nullptr;

  return pObject;
}

bool plVegetationPaintEditTool::LoadLayerData(plVegetationLayerData& out_data, plGrassCoverageData& out_grass, plTransform& out_layerTransform, plUuid& out_layer) const
{
  const plDocumentObject* pComponent = GetLayerComponentObject();
  if (pComponent == nullptr)
    return false;

  out_layer = pComponent->GetGuid();

  const plVariant value = pComponent->GetTypeAccessor().GetValue("Data");
  if (value.IsA<plDataBuffer>())
  {
    out_data.FromBuffer(value.Get<plDataBuffer>()).IgnoreResult();
  }
  else
  {
    out_data = plVegetationLayerData();
  }

  const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
  if (!grass.IsA<plDataBuffer>() || out_grass.FromBuffer(grass.Get<plDataBuffer>()).Failed())
  {
    out_grass.Clear();
  }

  // The layer's Grass Texel Size is the grid new texels land on.
  const plVariant texelSize = pComponent->GetTypeAccessor().GetValue("GrassTexelSize");
  const float fTexelSize = plGrassCoverageData::ClampTexelSize(texelSize.CanConvertTo<float>() ? texelSize.ConvertTo<float>() : plGrassCoverageData::DefaultTexelSize);
  if (out_grass.IsEmpty())
  {
    out_grass.m_fTexelSize = fTexelSize;
  }
  else
  {
    out_grass.Resample(fTexelSize);
  }

  out_layerTransform = GetDocument()->GetGlobalTransform(pComponent->GetParent());
  return true;
}

void plVegetationPaintEditTool::BeginStroke(plVegetationBrushMode::Enum mode)
{
  if (m_bStrokeActive)
    EndStroke();

  // A previous stroke still waiting for engine results commits what has arrived before this one reloads the layer;
  // its late results carry the old stroke id and are dropped.
  if (m_bCommitPending)
  {
    m_bCommitPending = false;
    CommitStroke();
  }

  if (!LoadLayerData(m_StrokeData, m_StrokeGrass, m_LayerTransform, m_StrokeLayer))
  {
    plLog::Warning("Vegetation: no target layer selected. Pick or create a layer in the Vegetation panel.");
    return;
  }

  SendOverlay();

  m_bStrokeActive = true;
  m_bCommitPending = false;
  m_bStrokeDirty = false;
  m_bStrokeGrassDirty = false;
  m_StrokeMode = mode;
  ++m_uiStrokeId;
  m_uiNextSegment = 0;
  m_uiPendingSegments = 0;
}

void plVegetationPaintEditTool::StrokeSegment(const plVec3& vCenter, const plVec3& vNormal)
{
  if (!m_bStrokeActive)
    return;

  const auto& s = plVegetationBrush::s_Settings;

  switch (m_StrokeMode)
  {
    case plVegetationBrushMode::Paint:
      if (s.GetSelectedPlantCount() == 0 && s.GetSelectedGrassCount() == 0)
      {
        plLog::Warning("Vegetation: select at least one type in the Vegetation panel to paint.");
        return;
      }
      SendSegment(m_StrokeMode, vCenter, vNormal);
      break;

    case plVegetationBrushMode::Erase:
    case plVegetationBrushMode::Thin:
    {
      // Editor side: the data is right here, only the disc has to be moved into layer space. With nothing checked
      // both kinds are affected; otherwise each kind only by its checked types. The same stamp goes to the engine,
      // which repeats it on the stroke preview so the removal shows up right away.
      plVegetationStrokeRequest request;
      FillRequest(request, m_StrokeMode, vCenter, vNormal);
      request.m_uiSeed = plHashingUtils::xxHash32(&vCenter, sizeof(plVec3)) ^ m_uiStrokeId;
      request.m_bStrokePreview = true;
      request.m_bMirrorOnly = true;

      const bool bNoneChecked = request.m_Types.IsEmpty() && request.m_GrassTypes.IsEmpty();

      // The world-space brush sphere is an axis-aligned ellipsoid in layer space: the per-axis layer scale maps layer
      // offsets back to world lengths for the distance test.
      const plTransform inv = m_LayerTransform.GetInverse();
      const plVec3 vLocalCenter = inv.TransformPosition(vCenter);
      const plVec3 vAxisScale = m_LayerTransform.m_vScale.Abs().CompMax(plVec3(0.0001f));

      if (bNoneChecked || !request.m_Types.IsEmpty())
      {
        if (m_StrokeData.RemoveInSphere(vLocalCenter, request.m_fRadius, request.m_fFalloff, request.GetPlantRemovalStrength(), request.m_Types, request.m_uiSeed, vAxisScale) > 0)
        {
          m_bStrokeDirty = true;
        }
      }

      if (bNoneChecked || !request.m_GrassTypes.IsEmpty())
      {
        plGrassCoverageStamp stamp;
        stamp.m_vCenter.Set(vLocalCenter.x, vLocalCenter.y);
        stamp.m_fRadius = request.m_fRadius;
        stamp.m_vAxisScale = vAxisScale.GetAsVec2();
        stamp.m_fFalloff = request.m_fFalloff;
        stamp.m_fDensity = request.m_fDensity;
        stamp.m_fStrength = request.m_fStrength;

        const plUInt32 uiChanged = m_StrokeMode == plVegetationBrushMode::Erase ? m_StrokeGrass.EraseInDisc(request.m_GrassTypes, stamp) : m_StrokeGrass.ThinInDisc(request.m_GrassTypes, stamp);
        if (uiChanged > 0)
        {
          m_bStrokeGrassDirty = true;
        }
      }

      m_bStrokePreviewSent = true;
      SendRequest(request);
      break;
    }

    default:
      break;
  }
}

void plVegetationPaintEditTool::FillRequest(plVegetationStrokeRequest& out_request, plVegetationBrushMode::Enum mode, const plVec3& vCenter, const plVec3& vNormal) const
{
  const auto& s = plVegetationBrush::s_Settings;

  out_request.m_uiMode = mode;
  out_request.m_LayerGuid = m_bStrokeActive ? m_StrokeLayer : s.m_LayerComponent;
  out_request.m_uiStrokeId = m_uiStrokeId;
  out_request.m_vCenter = vCenter;
  out_request.m_vNormal = vNormal;
  out_request.m_fRadius = s.m_fRadius;
  out_request.m_fDensity = s.m_fDensity;
  out_request.m_fFalloff = s.m_fFalloff;
  out_request.m_fStrength = s.m_fStrength;
  out_request.m_fAlignToNormal = s.m_bAlignToNormal ? s.m_fAlignToNormal : 0.0f;
  out_request.m_MaxSlope = s.m_MaxSlope;
  out_request.m_fMinScale = s.m_fMinScale;
  out_request.m_fMaxScale = s.m_fMaxScale;
  out_request.m_fSink = s.m_fSink;
  out_request.m_fMinSpacing = s.m_fMinSpacing;
  s.FillRequestTypes(out_request.m_Types, out_request.m_Weights);

  // Place stamps plants only; Paint needs the grass types to sample, Erase and Thin to mirror the removal.
  if (mode != plVegetationBrushMode::Place)
  {
    s.FillRequestGrassTypes(out_request.m_GrassTypes);
  }
  out_request.m_fLength = s.m_fLength;
  out_request.m_fTexelSize = m_StrokeGrass.m_fTexelSize;
  out_request.m_LayerTransform = m_LayerTransform;
}

void plVegetationPaintEditTool::SendRequest(const plVegetationStrokeRequest& request)
{
  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = plVegetationStrokeRequest::s_szWhatToDo;
  plDataBuffer buffer;
  request.ToBuffer(buffer);
  msg.m_PayloadValue = buffer;

  GetDocument()->SendMessageToEngine(&msg);
}

void plVegetationPaintEditTool::RollPlaceSeed()
{
  // Independent of the cursor position on purpose: the ghost keeps its yaw and scale while the mouse moves and only
  // re-rolls after a plant was placed.
  m_uiPlaceSeed = plHashingUtils::xxHash32(&m_uiPlaceSeed, sizeof(m_uiPlaceSeed), static_cast<plUInt32>(plTime::Now().GetNanoseconds()));
  if (m_uiPlaceSeed == 0)
    m_uiPlaceSeed = 1;
}

void plVegetationPaintEditTool::SendSegment(plVegetationBrushMode::Enum mode, const plVec3& vCenter, const plVec3& vNormal)
{
  plVegetationStrokeRequest request;
  FillRequest(request, mode, vCenter, vNormal);
  request.m_uiSegment = m_uiNextSegment++;
  request.m_uiSeed = mode == plVegetationBrushMode::Place ? m_uiPlaceSeed : plHashingUtils::xxHash32(&vCenter, sizeof(plVec3)) ^ (m_uiStrokeId * 7919u + request.m_uiSegment);

  // Place commits immediately, so only paint stamps feed the stroke preview.
  if (mode == plVegetationBrushMode::Paint)
  {
    request.m_bStrokePreview = true;
    m_bStrokePreviewSent = true;
  }

  ++m_uiPendingSegments;
  SendRequest(request);
}

void plVegetationPaintEditTool::UpdatePlacePreview(const plVec3& vCenter, const plVec3& vNormal)
{
  m_vPreviewCenter = vCenter;
  m_vPreviewNormal = vNormal;
  m_bPreviewShown = true;

  plVegetationStrokeRequest request;
  FillRequest(request, plVegetationBrushMode::Place, vCenter, vNormal);
  request.m_uiSeed = m_uiPlaceSeed;
  request.m_bPreview = true;

  SendRequest(request);
}

void plVegetationPaintEditTool::HidePlacePreview()
{
  if (!m_bPreviewShown)
    return;

  m_bPreviewShown = false;

  plVegetationStrokeRequest request;
  request.m_uiMode = plVegetationBrushMode::Place;
  request.m_bPreview = true; // no types: hides the ghost

  SendRequest(request);
}

void plVegetationPaintEditTool::PlaceSingle(const plVec3& vCenter, const plVec3& vNormal)
{
  BeginStroke(plVegetationBrushMode::Place);
  if (!m_bStrokeActive)
    return;

  if (plVegetationBrush::s_Settings.GetSelectedPlantCount() == 0)
  {
    plLog::Warning("Vegetation: select a plant type in the Vegetation panel to place. Grass Types are painted as coverage.");
    m_bStrokeActive = false;
    return;
  }

  SendSegment(plVegetationBrushMode::Place, vCenter, vNormal);
  EndStroke();

  // the next ghost gets a fresh yaw and scale
  RollPlaceSeed();
  if (m_bPreviewShown)
    UpdatePlacePreview(vCenter, vNormal);
}

void plVegetationPaintEditTool::EndStroke()
{
  if (!m_bStrokeActive)
    return;

  m_bStrokeActive = false;

  if (m_uiPendingSegments > 0)
  {
    // results still in flight: the last one to arrive commits
    m_bCommitPending = true;
    return;
  }

  CommitStroke();
}

void plVegetationPaintEditTool::ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg)
{
  const auto* pConfig = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg);
  if (pConfig == nullptr || pConfig->m_sWhatToDo != plVegetationStrokeResult::s_szWhatToDo)
    return;

  if (!pConfig->m_PayloadValue.IsA<plDataBuffer>())
    return;

  plVegetationStrokeResult result;
  if (result.FromBuffer(pConfig->m_PayloadValue.Get<plDataBuffer>()).Failed())
    return;

  if (result.m_uiStrokeId != m_uiStrokeId)
    return; // a stale segment from a stroke that was cancelled

  if (m_uiPendingSegments > 0)
    --m_uiPendingSegments;

  // world space -> layer space
  // Instance scales are uniform; the layer component multiplies them by the owner's average scale, so divide by it.
  const plTransform inv = m_LayerTransform.GetInverse();
  const float fLayerScale = m_LayerTransform.m_vScale.GetLength() / plMath::Sqrt(3.0f);
  const float fInvScale = fLayerScale > 0.0001f ? 1.0f / fLayerScale : 1.0f;

  for (const auto& entry : result.m_Added.m_Entries)
  {
    if (entry.m_Instances.IsEmpty())
      continue;

    auto& target = m_StrokeData.GetOrAddEntry(entry.m_sType);
    for (const auto& inst : entry.m_Instances)
    {
      plVegetationLayerInstance local = inst;
      local.m_vPosition = inv.TransformPosition(inst.m_vPosition);
      local.m_qRotation = inv.m_qRotation * inst.m_qRotation;
      local.m_fScale = inst.m_fScale * fInvScale;
      target.m_Instances.PushBack(local);
    }

    m_bStrokeDirty = true;
  }

  // the coverage delta is already in layer space, on the grid the request asked for
  if (!result.m_GrassDelta.IsEmpty())
  {
    if (m_StrokeGrass.MergeDelta(result.m_GrassDelta).Succeeded())
    {
      m_bStrokeGrassDirty = true;
    }
  }

  if (m_bCommitPending && m_uiPendingSegments == 0)
  {
    m_bCommitPending = false;
    CommitStroke();
  }
}

void plVegetationPaintEditTool::DropStrokePreview()
{
  if (!m_bStrokePreviewSent)
    return;

  m_bStrokePreviewSent = false;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = plVegetationDropPreviewMsg::s_szWhatToDo;
  GetDocument()->SendMessageToEngine(&msg);
}

void plVegetationPaintEditTool::CommitStroke()
{
  // Before the transaction: the engine drops the preview and takes the committed data over in the same batch of
  // messages, so the stroke never disappears for a frame.
  DropStrokePreview();

  if (!m_bStrokeDirty && !m_bStrokeGrassDirty)
    return;

  const bool bPlants = m_bStrokeDirty;
  const bool bGrass = m_bStrokeGrassDirty;
  m_bStrokeDirty = false;
  m_bStrokeGrassDirty = false;

  // The layer the stroke started on, even when the panel selected another layer meanwhile.
  const plDocumentObject* pComponent = GetLayerComponentObject(m_StrokeLayer);
  if (pComponent == nullptr)
    return;

  plCommandHistory* pHistory = GetDocument()->GetCommandHistory();
  pHistory->StartTransaction("Vegetation Stroke");

  if (bPlants)
  {
    m_StrokeData.Compact();

    plDataBuffer buffer;
    m_StrokeData.ToBuffer(buffer);

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pComponent->GetGuid();
    cmd.m_sProperty = "Data";
    cmd.m_NewValue = buffer;

    if (pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
      return;
    }
  }

  if (bGrass)
  {
    m_StrokeGrass.Compact();

    plDataBuffer buffer;
    m_StrokeGrass.ToBuffer(buffer);

    plSetObjectPropertyCommand cmd;
    cmd.m_Object = pComponent->GetGuid();
    cmd.m_sProperty = "GrassCoverage";
    cmd.m_NewValue = buffer;

    if (pHistory->AddCommand(cmd).Failed())
    {
      pHistory->CancelTransaction();
      return;
    }
  }

  pHistory->FinishTransaction();

  plVegetationBrush::NotifyStrokeApplied();
}
