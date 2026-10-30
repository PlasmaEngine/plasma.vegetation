#pragma once

#include <EditorFramework/Assets/SimpleAssetDocument.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAssetObjects.h>

/// Preview wind strength selectable from the asset window toolbar.
struct plVegetationPreviewWind
{
  enum Enum
  {
    Off,
    Light,
    Moderate,
    Strong,

    Default = Light
  };
};

struct plVegetationTypeAssetEvent
{
  enum class Type
  {
    PreviewWindChanged,
  };

  Type m_Type;
};

class plVegetationTypeAssetDocument : public plSimpleAssetDocument<plVegetationTypeAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeAssetDocument, plSimpleAssetDocument<plVegetationTypeAssetProperties>);

public:
  plVegetationTypeAssetDocument(plStringView sDocumentPath);

  /// \brief Writes the runtime descriptor; used by the transform and by the window's live preview.
  plStatus WriteVegetationType(plStreamWriter& inout_stream) const;

  plVegetationPreviewWind::Enum GetPreviewWind() const { return m_PreviewWind; }
  void SetPreviewWind(plVegetationPreviewWind::Enum wind);

  /// \brief Pushes the preview plant once, as a character would.
  void TriggerTouch();

  plEvent<const plVegetationTypeAssetEvent&> m_Events;

protected:
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;

private:
  plVegetationPreviewWind::Enum m_PreviewWind = plVegetationPreviewWind::Light;
};
