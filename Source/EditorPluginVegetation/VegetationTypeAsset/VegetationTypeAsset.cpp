#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>
#include <Foundation/Utilities/Progress.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTypeAssetDocument::plVegetationTypeAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plVegetationTypeAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple, true)
{
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

  plProgressRange range("Transforming Asset", 1, false);

  const plStatus status = WriteVegetationType(stream);
  if (status.Failed())
    return status;

  return plStatus(PL_SUCCESS);
}

plTransformStatus plVegetationTypeAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo)
{
  return plAssetDocument::RemoteCreateThumbnail(ThumbnailInfo);
}
