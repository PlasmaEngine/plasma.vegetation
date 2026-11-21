#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorFramework/Assets/AssetCurator.h>
#include <Foundation/Utilities/ConversionUtils.h>
#include <ToolsFoundation/Object/DocumentObjectManager.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationImpostorBake.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>
#include <Foundation/Utilities/Progress.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

// clang-format off
// Version 2: the transform bakes the impostor of foliage composite types.
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeAssetDocument, 2, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTypeAssetDocument::plVegetationTypeAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plVegetationTypeAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple, true)
{
  GetObjectManager()->m_PropertyEvents.AddEventHandler(plMakeDelegate(&plVegetationTypeAssetDocument::PropertyEventHandler, this));
  plAssetCurator::GetSingleton()->m_Events.AddEventHandler(plMakeDelegate(&plVegetationTypeAssetDocument::AssetCuratorEventHandler, this));
}

plVegetationTypeAssetDocument::~plVegetationTypeAssetDocument()
{
  plAssetCurator::GetSingleton()->m_Events.RemoveEventHandler(plMakeDelegate(&plVegetationTypeAssetDocument::AssetCuratorEventHandler, this));
  GetObjectManager()->m_PropertyEvents.RemoveEventHandler(plMakeDelegate(&plVegetationTypeAssetDocument::PropertyEventHandler, this));
}

void plVegetationTypeAssetDocument::InitializeAfterLoading(bool bFirstTimeCreation)
{
  SUPER::InitializeAfterLoading(bFirstTimeCreation);
  UpdateCompositeInfo();
}

void plVegetationTypeAssetDocument::UpdateCompositeInfo()
{
  const plDocumentObject* pRoot = GetObjectManager()->GetRootObject();
  if (pRoot->GetChildren().IsEmpty())
    return;

  plDocumentObject* pProperties = pRoot->GetChildren()[0];

  // LOD 0 straight from the object model: the native mirror may not have seen the change yet.
  plString sMesh;
  if (pProperties->GetTypeAccessor().GetCount("Lods") > 0)
  {
    const plVariant lod = pProperties->GetTypeAccessor().GetValue("Lods", plVariant(0u));
    if (lod.IsA<plUuid>())
    {
      if (const plDocumentObject* pLod = GetObjectManager()->GetObject(lod.Get<plUuid>()))
      {
        const plVariant mesh = pLod->GetTypeAccessor().GetValue("Mesh");
        if (mesh.IsA<plString>())
          sMesh = mesh.Get<plString>();
      }
    }
  }

  const plVariant current = pProperties->GetTypeAccessor().GetValue("Composite");
  if (sMesh == m_sCompositeMesh && current.IsA<plString>())
    return;

  m_sCompositeMesh = sMesh;

  const plString sSummary = plVegetationCompositeSummary::Query(sMesh).GetSummary();
  if (current.IsA<plString>() && current.Get<plString>() == sSummary)
    return;

  // Outside the command history on purpose: the summary is derived from the mesh asset and never saved.
  GetObjectManager()->SetValue(pProperties, "Composite", sSummary).IgnoreResult();
}

void plVegetationTypeAssetDocument::PropertyEventHandler(const plDocumentObjectPropertyEvent& e)
{
  if (e.m_sProperty == "Composite")
    return;

  UpdateCompositeInfo();
}

void plVegetationTypeAssetDocument::AssetCuratorEventHandler(const plAssetCuratorEvent& e)
{
  if (e.m_Type != plAssetCuratorEvent::Type::AssetUpdated || m_sCompositeMesh.IsEmpty())
    return;

  plUuid guid;
  if (!plConversionUtils::TryConvertStringToUuid(m_sCompositeMesh, guid) || guid != e.m_AssetGuid)
    return;

  // the composite was transformed again: its statistics changed
  m_sCompositeMesh.Clear();
  UpdateCompositeInfo();
}

plStatus plVegetationTypeAssetDocument::WriteVegetationType(plStreamWriter& inout_stream) const
{
  const plVegetationTypeAssetProperties* pProp = GetProperties();
  if (pProp == nullptr)
    return plStatus("Vegetation type document has no properties.");

  plVegetationTypeResourceDescriptor desc;
  pProp->FillDescriptor(desc);
  desc.Save(inout_stream);

  return plStatus(PL_SUCCESS);
}

void plVegetationTypeAssetDocument::SetPreviewWind(plVegetationPreviewWind::Enum wind)
{
  if (m_PreviewWind == wind)
    return;

  m_PreviewWind = wind;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "SetWindStrength";
  msg.m_sPayload = "";
  msg.m_PayloadValue = (plInt32)wind;
  SendMessageToEngine(&msg);

  plVegetationTypeAssetEvent e;
  e.m_Type = plVegetationTypeAssetEvent::Type::PreviewWindChanged;
  m_Events.Broadcast(e);
}

void plVegetationTypeAssetDocument::TriggerTouch()
{
  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "Touch";
  msg.m_sPayload = "";
  SendMessageToEngine(&msg);
}

plTransformStatus plVegetationTypeAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  PL_IGNORE_UNUSED(sOutputTag);
  PL_IGNORE_UNUSED(pAssetProfile);
  PL_IGNORE_UNUSED(AssetHeader);
  PL_IGNORE_UNUSED(transformFlags);

  plProgressRange range("Transforming Asset", 2, false);

  const plVegetationTypeAssetProperties* pProp = GetProperties();
  if (pProp == nullptr)
    return plStatus("Vegetation type document has no properties.");

  plVegetationTypeResourceDescriptor desc;
  pProp->FillDescriptor(desc);

  // A foliage composite bakes its impostor from LOD 0's merged mesh (on the CPU). A failed bake leaves the type without
  // one: its trees keep the merged mesh at every distance.
  range.BeginNextStep("Baking Impostor");
  plSharedPtr<plVegetationImpostorData> pImpostor = PL_DEFAULT_NEW(plVegetationImpostorData);
  plSharedPtr<plVegetationVoxelProxy> pProxy = PL_DEFAULT_NEW(plVegetationVoxelProxy);
  const plStatus bakeStatus = plVegetationImpostorEditorBake::Bake(desc, *pImpostor, *pProxy);
  if (bakeStatus.Succeeded())
  {
    desc.m_pImpostorData = pImpostor;
    desc.m_pVoxelProxy = pProxy;
  }
  else if (!bakeStatus.m_sMessage.IsEmpty())
    plLog::Warning("{}", bakeStatus.m_sMessage);

  range.BeginNextStep("Writing");
  desc.Save(stream);
  return plStatus(PL_SUCCESS);
}

plTransformStatus plVegetationTypeAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo)
{
  return plAssetDocument::RemoteCreateThumbnail(ThumbnailInfo);
}
