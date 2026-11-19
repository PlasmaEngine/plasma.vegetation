#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <EditorFramework/Document/GameObjectDocument.h>
#include <EditorPluginVegetation/Paint/VegetationBrushSettings.h>
#include <EditorPluginVegetation/Paint/VegetationLayerFill.h>
#include <Foundation/Types/UniquePtr.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <GuiFoundation/PropertyGrid/PropertyMetaState.h>
#include <ToolsFoundation/Command/TreeCommands.h>
#include <ToolsFoundation/Document/DocumentManager.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationLayerFillAction, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  /// Fill requests carry stroke ids with this bit, so the paint tool, which counts its own ids up from 1, never takes
  /// their answers for its strokes.
  constexpr plUInt32 s_uiFillStrokeIdBit = 0x80000000u;

  constexpr plStringView s_sLayerType = "plVegetationLayerComponent"_plsv;

  struct FillPart
  {
    plUuid m_Layer;
    plUInt32 m_uiStrokeId = 0;
    plVegetationStrokeRequest m_Request;
    plVegetationFillSettings m_Settings;
    bool m_bWriteSettings = false;
    bool m_bReceived = false;
    plVegetationStrokeResult m_Result;
  };

  /// The layers of one Fill; they commit together once every engine answer arrived.
  struct FillJob
  {
    plGameObjectDocument* m_pDocument = nullptr;
    plDynamicArray<FillPart> m_Parts;
  };

  plDynamicArray<plUniquePtr<FillJob>> s_Jobs;
  plHybridArray<plDocument*, 4> s_HookedDocuments;
  plUInt32 s_uiNextStrokeId = 1;

  enum class SpeciesKind
  {
    Unknown,
    Plant,
    Grass,
  };

  SpeciesKind GetSpeciesKind(plStringView sType)
  {
    plUuid guid;
    if (!plConversionUtils::TryConvertStringToUuid(sType, guid))
      return SpeciesKind::Unknown;

    auto pSubAsset = plAssetCurator::GetSingleton()->GetSubAsset(guid);
    if (pSubAsset == nullptr)
      return SpeciesKind::Unknown;

    if (pSubAsset->m_Data.m_sSubAssetsDocumentTypeName == "Grass Type")
      return SpeciesKind::Grass;
    if (pSubAsset->m_Data.m_sSubAssetsDocumentTypeName == "Vegetation Type")
      return SpeciesKind::Plant;
    return SpeciesKind::Unknown;
  }

  /// The layer's committed Data and GrassCoverage, the coverage on the layer's Grass Texel Size.
  void LoadLayer(plGameObjectDocument* pDocument, const plDocumentObject* pComponent, plVegetationLayerData& out_data, plGrassCoverageData& out_grass, plTransform& out_transform)
  {
    const plVariant value = pComponent->GetTypeAccessor().GetValue("Data");
    if (!value.IsA<plDataBuffer>() || out_data.FromBuffer(value.Get<plDataBuffer>()).Failed())
      out_data = plVegetationLayerData();

    const plVariant grass = pComponent->GetTypeAccessor().GetValue("GrassCoverage");
    if (!grass.IsA<plDataBuffer>() || out_grass.FromBuffer(grass.Get<plDataBuffer>()).Failed())
      out_grass.Clear();

    const plVariant texelSize = pComponent->GetTypeAccessor().GetValue("GrassTexelSize");
    const float fTexelSize = plGrassCoverageData::ClampTexelSize(texelSize.CanConvertTo<float>() ? texelSize.ConvertTo<float>() : plGrassCoverageData::DefaultTexelSize);
    if (out_grass.IsEmpty())
      out_grass.m_fTexelSize = fTexelSize;
    else
      out_grass.Resample(fTexelSize);

    out_transform = pDocument->GetGlobalTransform(pComponent->GetParent());
  }

  /// Layer-space bounds of the plants, coverage and clearings; invalid for an empty layer. Without any height (only
  /// clearings) the box sits at z = 0.
  plBoundingBox GetLayerBounds(const plVegetationLayerData& data, const plGrassCoverageData& grass)
  {
    plBoundingBox bounds = plBoundingBox::MakeInvalid();
    bool bHeight = false;

    for (const auto& entry : data.m_Entries)
    {
      for (const auto& inst : entry.m_Instances)
      {
        bounds.ExpandToInclude(inst.m_vPosition);
        bHeight = true;
      }
    }

    const float fTile = grass.m_fTexelSize * plGrassCoverageTile::TexelsPerSide;
    for (const auto& entry : grass.m_Entries)
    {
      for (auto it = entry.m_Tiles.GetIterator(); it.IsValid(); ++it)
      {
        const plGrassCoverageTile& tile = it.Value();
        if (tile.IsEmpty())
          continue;

        bounds.ExpandToInclude(plVec3(tile.m_iX * fTile, tile.m_iY * fTile, tile.m_fBaseHeight));
        bounds.ExpandToInclude(plVec3((tile.m_iX + 1) * fTile, (tile.m_iY + 1) * fTile, tile.m_fBaseHeight));
        bHeight = true;
      }
    }

    for (const auto& clearing : data.m_Clearings)
    {
      const float z = bHeight ? bounds.m_vMin.z : 0.0f;
      bounds.ExpandToInclude(plVec3(clearing.m_vCenter.x - clearing.m_fRadius, clearing.m_vCenter.y - clearing.m_fRadius, z));
      bounds.ExpandToInclude(plVec3(clearing.m_vCenter.x + clearing.m_fRadius, clearing.m_vCenter.y + clearing.m_fRadius, z));
    }

    return bounds;
  }

  void OnProcessMessage(const plEditorEngineDocumentMsg* pMsg);

  void HookDocument(plDocument* pDocument)
  {
    if (s_HookedDocuments.Contains(pDocument))
      return;

    if (auto* pAssetDocument = plDynamicCast<plAssetDocument*>(pDocument))
    {
      pAssetDocument->m_ProcessMessageEvent.AddEventHandler(&OnProcessMessage);
      s_HookedDocuments.PushBack(pDocument);
    }
  }

  void UnhookDocument(plDocument* pDocument)
  {
    if (!s_HookedDocuments.RemoveAndSwap(pDocument))
      return;

    if (auto* pAssetDocument = plDynamicCast<plAssetDocument*>(pDocument))
      pAssetDocument->m_ProcessMessageEvent.RemoveEventHandler(&OnProcessMessage);
  }

  plResult AddSet(plCommandHistory* pHistory, const plUuid& object, plStringView sProperty, const plVariant& value)
  {
    plSetObjectPropertyCommand cmd;
    cmd.m_Object = object;
    cmd.m_sProperty = sProperty;
    cmd.m_NewValue = value;
    return pHistory->AddCommand(cmd).Succeeded() ? PL_SUCCESS : PL_FAILURE;
  }

  plResult WriteSettings(plCommandHistory* pHistory, const plDocumentObject* pComponent, const plVegetationFillSettings& settings)
  {
    const plUuid guid = pComponent->GetGuid();

    // the species list is replaced as a whole
    plHybridArray<plUuid, 8> oldSpecies;
    for (const plDocumentObject* pChild : pComponent->GetChildren())
    {
      if (pChild->GetParentProperty() == plStringView("FillSpecies"))
        oldSpecies.PushBack(pChild->GetGuid());
    }

    for (const plUuid& child : oldSpecies)
    {
      plRemoveObjectCommand cmd;
      cmd.m_Object = child;
      if (pHistory->AddCommand(cmd).Failed())
        return PL_FAILURE;
    }

    for (const auto& species : settings.m_Species)
    {
      plAddObjectCommand cmd;
      cmd.m_pType = plGetStaticRTTI<plVegetationFillSpecies>();
      cmd.m_Parent = guid;
      cmd.m_sParentProperty = "FillSpecies";
      cmd.m_Index = -1;
      cmd.m_NewObjectGuid = plUuid::MakeUuid();
      if (pHistory->AddCommand(cmd).Failed())
        return PL_FAILURE;

      PL_SUCCEED_OR_RETURN(AddSet(pHistory, cmd.m_NewObjectGuid, "Type", species.m_sType));
      PL_SUCCEED_OR_RETURN(AddSet(pHistory, cmd.m_NewObjectGuid, "Weight", species.m_fWeight));
    }

    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillTreesPerHectare", settings.m_fTreesPerHectare));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillGrassDensity", settings.m_fGrassDensity));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillGrassLength", settings.m_fGrassLength));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillGrassMaxSlope", settings.m_GrassMaxSlope));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillCenter", settings.m_vCenter));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillExtent", settings.m_vExtent));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillSeed", settings.m_uiSeed));
    PL_SUCCEED_OR_RETURN(AddSet(pHistory, guid, "FillReplace", settings.m_bReplace));
    return PL_SUCCESS;
  }

  void Commit(FillJob& job)
  {
    plGameObjectDocument* pDocument = job.m_pDocument;
    plCommandHistory* pHistory = pDocument->GetCommandHistory();
    pHistory->StartTransaction("Vegetation Fill");

    bool bChanged = false;
    plUInt32 uiPlants = 0;

    for (FillPart& part : job.m_Parts)
    {
      const plDocumentObject* pComponent = pDocument->GetObjectManager()->GetObject(part.m_Layer);
      if (pComponent == nullptr)
        continue;

      // the layer as it is now: edits made while the engine worked are kept
      plVegetationLayerData data;
      plGrassCoverageData grass;
      plTransform layerTransform;
      LoadLayer(pDocument, pComponent, data, grass, layerTransform);

      const bool bReplaced = part.m_Request.ApplyFillReplace(data, &grass);
      bool bPlants = bReplaced && !part.m_Request.m_Types.IsEmpty();
      bool bGrass = bReplaced && !part.m_Request.m_GrassTypes.IsEmpty();

      // world space -> layer space, as the paint tool merges its strokes
      const plTransform inv = layerTransform.GetInverse();
      const float fLayerScale = layerTransform.m_vScale.GetLength() / plMath::Sqrt(3.0f);
      const float fInvScale = fLayerScale > 0.0001f ? 1.0f / fLayerScale : 1.0f;

      for (const auto& entry : part.m_Result.m_Added.m_Entries)
      {
        if (entry.m_Instances.IsEmpty())
          continue;

        auto& target = data.GetOrAddEntry(entry.m_sType);
        for (const auto& inst : entry.m_Instances)
        {
          plVegetationLayerInstance local = inst;
          local.m_vPosition = inv.TransformPosition(inst.m_vPosition);
          local.m_qRotation = inv.m_qRotation * inst.m_qRotation;
          local.m_fScale = inst.m_fScale * fInvScale;
          target.m_Instances.PushBack(local);
        }

        uiPlants += entry.m_Instances.GetCount();
        bPlants = true;
      }

      if (!part.m_Result.m_GrassDelta.IsEmpty())
      {
        if (grass.MergeDelta(part.m_Result.m_GrassDelta).Succeeded())
          bGrass = true;
        else
          plLog::Warning("Vegetation Fill: the layer's Grass Texel Size changed while filling; its grass was not filled.");
      }

      if (bPlants)
      {
        data.Compact();
        plDataBuffer buffer;
        data.ToBuffer(buffer);
        if (AddSet(pHistory, part.m_Layer, "Data", buffer).Failed())
        {
          pHistory->CancelTransaction();
          return;
        }
      }

      if (bGrass)
      {
        grass.Compact();
        plDataBuffer buffer;
        grass.ToBuffer(buffer);
        if (AddSet(pHistory, part.m_Layer, "GrassCoverage", buffer).Failed())
        {
          pHistory->CancelTransaction();
          return;
        }
      }

      if (part.m_bWriteSettings)
      {
        if (WriteSettings(pHistory, pComponent, part.m_Settings).Failed())
        {
          pHistory->CancelTransaction();
          return;
        }
      }

      bChanged |= bPlants || bGrass || part.m_bWriteSettings;
    }

    if (!bChanged)
    {
      pHistory->CancelTransaction();
      plLog::Info("Vegetation Fill: nothing to add.");
      return;
    }

    pHistory->FinishTransaction();
    plLog::Info("Vegetation Fill: {} layer(s), {} plants added.", job.m_Parts.GetCount(), uiPlants);

    plVegetationBrush::NotifyStrokeApplied();
  }

  void OnProcessMessage(const plEditorEngineDocumentMsg* pMsg)
  {
    const auto* pConfig = plDynamicCast<const plSimpleDocumentConfigMsgToEditor*>(pMsg);
    if (pConfig == nullptr || pConfig->m_sWhatToDo != plVegetationStrokeResult::s_szWhatToDo || !pConfig->m_PayloadValue.IsA<plDataBuffer>())
      return;

    plVegetationStrokeResult result;
    if (result.FromBuffer(pConfig->m_PayloadValue.Get<plDataBuffer>()).Failed() || (result.m_uiStrokeId & s_uiFillStrokeIdBit) == 0)
      return;

    for (plUInt32 j = 0; j < s_Jobs.GetCount(); ++j)
    {
      FillJob& job = *s_Jobs[j];

      bool bAll = true;
      bool bMine = false;
      for (FillPart& part : job.m_Parts)
      {
        if (part.m_uiStrokeId == result.m_uiStrokeId && !part.m_bReceived)
        {
          part.m_Result = std::move(result);
          part.m_bReceived = true;
          bMine = true;
        }

        bAll &= part.m_bReceived;
      }

      if (!bMine)
        continue;

      if (bAll)
      {
        plUniquePtr<FillJob> pJob = std::move(s_Jobs[j]);
        s_Jobs.RemoveAtAndCopy(j);
        Commit(*pJob);
      }
      return;
    }
  }

  void DocumentManagerEventHandler(const plDocumentManager::Event& e)
  {
    if (e.m_Type != plDocumentManager::Event::Type::DocumentClosing)
      return;

    for (plUInt32 j = s_Jobs.GetCount(); j > 0; --j)
    {
      if (s_Jobs[j - 1]->m_pDocument == e.m_pDocument)
        s_Jobs.RemoveAtAndCopy(j - 1);
    }

    UnhookDocument(e.m_pDocument);
  }

  /// Builds the Fill request of one layer; fails with a reason when there is nothing to fill.
  plStatus BuildRequest(plGameObjectDocument* pDocument, const plDocumentObject* pComponent, const plVegetationFillSettings& settings, const plVegetationStrokeRequest* pBrush, plVegetationStrokeRequest& out_request)
  {
    out_request = pBrush != nullptr ? *pBrush : plVegetationStrokeRequest();
    if (pBrush == nullptr)
      out_request.m_bUseTypeRules = true;

    out_request.m_uiMode = plVegetationBrushMode::Fill;
    out_request.m_LayerGuid = pComponent->GetGuid();
    out_request.m_uiSegment = 0;
    out_request.m_bPreview = false;
    out_request.m_bStrokePreview = false;
    out_request.m_bMirrorOnly = false;
    out_request.m_Types.Clear();
    out_request.m_Weights.Clear();
    out_request.m_GrassTypes.Clear();

    for (const auto& species : settings.m_Species)
    {
      switch (GetSpeciesKind(species.m_sType))
      {
        case SpeciesKind::Plant:
          out_request.m_Types.PushBack(species.m_sType);
          out_request.m_Weights.PushBack(plMath::Max(species.m_fWeight, 0.0f));
          break;
        case SpeciesKind::Grass:
          if (species.m_fWeight > 0.0f)
            out_request.m_GrassTypes.PushBack(species.m_sType);
          break;
        default:
          plLog::Warning("Vegetation Fill: '{}' is neither a Vegetation Type nor a Grass Type; skipped.", species.m_sType);
          break;
      }
    }

    if (out_request.m_Types.IsEmpty() && out_request.m_GrassTypes.IsEmpty())
      return plStatus("The layer's Fill Species holds no Vegetation Type or Grass Type.");

    plVegetationLayerData data;
    plGrassCoverageData grass;
    plTransform layerTransform;
    LoadLayer(pDocument, pComponent, data, grass, layerTransform);

    plBoundingBox region;
    if (settings.m_vExtent.x > 0.0f && settings.m_vExtent.y > 0.0f)
    {
      // the box; the ground is searched around the layer's content height, or the layer origin
      const plBoundingBox bounds = GetLayerBounds(data, grass);
      const float zMin = bounds.IsValid() ? bounds.m_vMin.z : 0.0f;
      const float zMax = bounds.IsValid() ? bounds.m_vMax.z : 0.0f;
      const plVec2 vHalf = settings.m_vExtent * 0.5f;
      region = plBoundingBox::MakeFromMinMax(plVec3(settings.m_vCenter.x - vHalf.x, settings.m_vCenter.y - vHalf.y, zMin), plVec3(settings.m_vCenter.x + vHalf.x, settings.m_vCenter.y + vHalf.y, zMax));
    }
    else
    {
      region = GetLayerBounds(data, grass);
      if (!region.IsValid() || region.m_vMax.x - region.m_vMin.x < 0.5f || region.m_vMax.y - region.m_vMin.y < 0.5f)
        return plStatus("The layer is empty, so it has no area to fill: set Fill Extent (and Fill Center) to fill a box.");
    }

    out_request.m_vFillMin = region.m_vMin;
    out_request.m_vFillMax = region.m_vMax;
    out_request.m_fTreesPerHectare = settings.m_fTreesPerHectare;
    out_request.m_fDensity = settings.m_fGrassDensity;
    out_request.m_fLength = settings.m_fGrassLength;
    out_request.m_MaxSlope = settings.m_GrassMaxSlope;
    out_request.m_bFillReplace = settings.m_bReplace;
    out_request.m_fTexelSize = grass.m_fTexelSize;
    out_request.m_LayerTransform = layerTransform;

    // the same settings fill each layer differently
    const plUuid guid = pComponent->GetGuid();
    out_request.m_uiSeed = plHashingUtils::xxHash32(&guid, sizeof(plUuid), settings.m_uiSeed);
    return plStatus(PL_SUCCESS);
  }

  plStatus StartJob(plGameObjectDocument* pDocument, plUniquePtr<FillJob>&& pJob)
  {
    if (pJob->m_Parts.IsEmpty())
      return plStatus("Nothing to fill.");

    HookDocument(pDocument);

    for (const FillPart& part : pJob->m_Parts)
    {
      plSimpleDocumentConfigMsgToEngine msg;
      msg.m_sWhatToDo = plVegetationStrokeRequest::s_szWhatToDo;
      plDataBuffer buffer;
      part.m_Request.ToBuffer(buffer);
      msg.m_PayloadValue = buffer;
      pDocument->SendMessageToEngine(&msg);
    }

    s_Jobs.PushBack(std::move(pJob));
    return plStatus(PL_SUCCESS);
  }

  plStatus AddPart(FillJob& job, plGameObjectDocument* pDocument, const plDocumentObject* pComponent, const plVegetationFillSettings& settings, bool bWriteSettings, const plVegetationStrokeRequest* pBrush)
  {
    FillPart part;
    PL_SUCCEED_OR_RETURN(BuildRequest(pDocument, pComponent, settings, pBrush, part.m_Request));

    part.m_Layer = pComponent->GetGuid();
    part.m_uiStrokeId = s_uiFillStrokeIdBit | (s_uiNextStrokeId++ & ~s_uiFillStrokeIdBit);
    part.m_Request.m_uiStrokeId = part.m_uiStrokeId;
    part.m_Settings = settings;
    part.m_bWriteSettings = bWriteSettings;
    job.m_Parts.PushBack(std::move(part));
    return plStatus(PL_SUCCESS);
  }
} // namespace

void plVegetationFillSettings::ReadFrom(const plDocumentObject* pComponent)
{
  *this = plVegetationFillSettings();

  const plIReflectedTypeAccessor& accessor = pComponent->GetTypeAccessor();

  auto GetFloat = [&](plStringView sName, float fDefault)
  {
    const plVariant v = accessor.GetValue(sName);
    return v.CanConvertTo<float>() ? v.ConvertTo<float>() : fDefault;
  };

  m_fTreesPerHectare = GetFloat("FillTreesPerHectare", m_fTreesPerHectare);
  m_fGrassDensity = GetFloat("FillGrassDensity", m_fGrassDensity);
  m_fGrassLength = GetFloat("FillGrassLength", m_fGrassLength);

  if (const plVariant v = accessor.GetValue("FillGrassMaxSlope"); v.IsA<plAngle>())
    m_GrassMaxSlope = v.Get<plAngle>();
  if (const plVariant v = accessor.GetValue("FillCenter"); v.IsA<plVec2>())
    m_vCenter = v.Get<plVec2>();
  if (const plVariant v = accessor.GetValue("FillExtent"); v.IsA<plVec2>())
    m_vExtent = v.Get<plVec2>();
  if (const plVariant v = accessor.GetValue("FillSeed"); v.CanConvertTo<plUInt32>())
    m_uiSeed = v.ConvertTo<plUInt32>();
  if (const plVariant v = accessor.GetValue("FillReplace"); v.IsA<bool>())
    m_bReplace = v.Get<bool>();

  for (const plDocumentObject* pChild : pComponent->GetChildren())
  {
    if (pChild->GetParentProperty() != plStringView("FillSpecies"))
      continue;

    Species& species = m_Species.ExpandAndGetRef();
    const plVariant type = pChild->GetTypeAccessor().GetValue("Type");
    species.m_sType = type.IsA<plString>() ? type.Get<plString>() : plString();
    const plVariant weight = pChild->GetTypeAccessor().GetValue("Weight");
    species.m_fWeight = weight.CanConvertTo<float>() ? weight.ConvertTo<float>() : 1.0f;
  }
}

namespace
{
  /// The layer component's Fill properties that do nothing for its species and region are hidden.
  void PropertyMetaStateEventHandler(plPropertyMetaStateEvent& e)
  {
    if (e.m_pObject->GetType()->GetTypeName() != s_sLayerType)
      return;

    plVegetationFillSettings settings;
    settings.ReadFrom(e.m_pObject);

    bool bPlants = false;
    bool bGrass = false;
    for (const auto& species : settings.m_Species)
    {
      const SpeciesKind kind = GetSpeciesKind(species.m_sType);
      bPlants |= kind == SpeciesKind::Plant;
      bGrass |= kind == SpeciesKind::Grass;
    }

    auto& props = *e.m_pPropertyStates;
    if (!bPlants)
      props["FillTreesPerHectare"].m_Visibility = plPropertyUiState::Invisible;

    if (!bGrass)
    {
      props["FillGrassDensity"].m_Visibility = plPropertyUiState::Invisible;
      props["FillGrassLength"].m_Visibility = plPropertyUiState::Invisible;
      props["FillGrassMaxSlope"].m_Visibility = plPropertyUiState::Invisible;
    }

    // a zero extent fills the whole layer, which has no centre
    if (settings.m_vExtent.x <= 0.0f || settings.m_vExtent.y <= 0.0f)
      props["FillCenter"].m_Visibility = plPropertyUiState::Invisible;
  }
} // namespace

void plVegetationLayerFill::Startup()
{
  plDocumentManager::s_Events.AddEventHandler(&DocumentManagerEventHandler);
  plPropertyMetaState::GetSingleton()->m_Events.AddEventHandler(&PropertyMetaStateEventHandler);
}

void plVegetationLayerFill::Shutdown()
{
  plPropertyMetaState::GetSingleton()->m_Events.RemoveEventHandler(&PropertyMetaStateEventHandler);
  plDocumentManager::s_Events.RemoveEventHandler(&DocumentManagerEventHandler);

  while (!s_HookedDocuments.IsEmpty())
    UnhookDocument(s_HookedDocuments.PeekBack());

  s_Jobs.Clear();
}

plStatus plVegetationLayerFill::FillLayers(plGameObjectDocument* pDocument, plArrayPtr<const plDocumentObject* const> layerComponents)
{
  plUniquePtr<FillJob> pJob = PL_DEFAULT_NEW(FillJob);
  pJob->m_pDocument = pDocument;

  for (const plDocumentObject* pComponent : layerComponents)
  {
    plVegetationFillSettings settings;
    settings.ReadFrom(pComponent);

    const plStatus status = AddPart(*pJob, pDocument, pComponent, settings, false, nullptr);
    if (status.Failed())
      plLog::Warning("Vegetation Fill skips a layer: {}", status.m_sMessage);
  }

  return StartJob(pDocument, std::move(pJob));
}

plStatus plVegetationLayerFill::FillLayer(plGameObjectDocument* pDocument, const plDocumentObject* pLayerComponent, const plVegetationFillSettings& settings, bool bWriteSettings, const plVegetationStrokeRequest* pBrush)
{
  plUniquePtr<FillJob> pJob = PL_DEFAULT_NEW(FillJob);
  pJob->m_pDocument = pDocument;

  PL_SUCCEED_OR_RETURN(AddPart(*pJob, pDocument, pLayerComponent, settings, bWriteSettings, pBrush));
  return StartJob(pDocument, std::move(pJob));
}

bool plVegetationLayerFill::IsFillPending(const plDocument* pDocument)
{
  for (const auto& pJob : s_Jobs)
  {
    if (pJob->m_pDocument == pDocument)
      return true;
  }
  return false;
}

//////////////////////////////////////////////////////////////////////////

plVegetationLayerFillAction::plVegetationLayerFillAction(const plActionContext& context, const char* szName)
  : plButtonAction(context, szName, false, ":/EditorPluginVegetation/Paint.svg")
{
}

void plVegetationLayerFillAction::Execute(const plVariant& value)
{
  PL_IGNORE_UNUSED(value);

  auto* pDocument = plDynamicCast<plGameObjectDocument*>(const_cast<plDocument*>(GetContext().m_pDocument));
  if (pDocument == nullptr)
  {
    plLog::Warning("Vegetation.Layer.Fill needs a scene document.");
    return;
  }

  // selected layer components, or the layer components of selected objects
  plHybridArray<const plDocumentObject*, 8> layers;
  for (const plDocumentObject* pObject : pDocument->GetSelectionManager()->GetSelection())
  {
    if (pObject->GetType()->GetTypeName() == s_sLayerType)
    {
      if (!layers.Contains(pObject))
        layers.PushBack(pObject);
      continue;
    }

    for (const plDocumentObject* pChild : pObject->GetChildren())
    {
      if (pChild->GetType()->GetTypeName() == s_sLayerType && !layers.Contains(pChild))
        layers.PushBack(pChild);
    }
  }

  if (layers.IsEmpty())
  {
    plLog::Warning("Vegetation.Layer.Fill: select a Vegetation Layer object (or its layer component) first.");
    return;
  }

  const plStatus status = plVegetationLayerFill::FillLayers(pDocument, layers);
  if (status.Failed())
    plLog::Warning("Vegetation.Layer.Fill: {}", status.m_sMessage);
}
