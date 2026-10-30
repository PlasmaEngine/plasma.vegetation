#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAssetObjects.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>

struct plGrassTypeAssetEvent
{
  enum class Type
  {
    PreviewWindChanged,
  };

  Type m_Type;
};

class plGrassTypeAssetDocument : public plSimpleAssetDocument<plGrassTypeAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeAssetDocument, plSimpleAssetDocument<plGrassTypeAssetProperties>);

public:
  plGrassTypeAssetDocument(plStringView sDocumentPath);

  /// \brief Writes the runtime descriptor; used by the transform and by the window's live preview.
  plStatus WriteGrassType(plStreamWriter& inout_stream) const;

  /// Preview wind strength, shared with the Vegetation Type window's choices.
  plVegetationPreviewWind::Enum GetPreviewWind() const { return m_PreviewWind; }
  void SetPreviewWind(plVegetationPreviewWind::Enum wind);

  plEvent<const plGrassTypeAssetEvent&> m_Events;

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;

private:
  plVegetationPreviewWind::Enum m_PreviewWind = plVegetationPreviewWind::Light;
};
