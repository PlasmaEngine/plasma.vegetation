#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorEngineProcessFramework/EngineProcess/EngineProcessMessages.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAsset.h>
#include <Foundation/Utilities/Progress.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeAssetDocument, 1, plRTTINoAllocator)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassTypeAssetDocument::plGrassTypeAssetDocument(plStringView sDocumentPath)
  : plSimpleAssetDocument<plGrassTypeAssetProperties>(sDocumentPath, plAssetDocEngineConnection::Simple, true)
{
}

plStatus plGrassTypeAssetDocument::WriteGrassType(plStreamWriter& inout_stream) const
{
  const plGrassTypeAssetProperties* pProp = GetProperties();
  if (pProp == nullptr)
    return plStatus("Grass type document has no properties.");

  plGrassTypeResourceDescriptor desc;
  pProp->FillDescriptor(desc);
  desc.Save(inout_stream);

  return plStatus(PL_SUCCESS);
}

void plGrassTypeAssetDocument::SetPreviewWind(plVegetationPreviewWind::Enum wind)
{
  if (m_PreviewWind == wind)
    return;

  m_PreviewWind = wind;

  plSimpleDocumentConfigMsgToEngine msg;
  msg.m_sWhatToDo = "SetWindStrength";
  msg.m_sPayload = "";
  msg.m_PayloadValue = (plInt32)wind;
  SendMessageToEngine(&msg);

  plGrassTypeAssetEvent e;
  e.m_Type = plGrassTypeAssetEvent::Type::PreviewWindChanged;
  m_Events.Broadcast(e);
}

plTransformStatus plGrassTypeAssetDocument::InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags)
{
  PL_IGNORE_UNUSED(sOutputTag);
  PL_IGNORE_UNUSED(pAssetProfile);
  PL_IGNORE_UNUSED(AssetHeader);
  PL_IGNORE_UNUSED(transformFlags);

  plProgressRange range("Transforming Asset", 1, false);

  const plStatus status = WriteGrassType(stream);
  if (status.Failed())
    return status;

  return plStatus(PL_SUCCESS);
}

plTransformStatus plGrassTypeAssetDocument::InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo)
{
  return plAssetDocument::RemoteCreateThumbnail(ThumbnailInfo);
}
