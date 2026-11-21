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

struct plAssetCuratorEvent;
struct plDocumentObjectPropertyEvent;

class plVegetationTypeAssetDocument : public plSimpleAssetDocument<plVegetationTypeAssetProperties>
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationTypeAssetDocument, plSimpleAssetDocument<plVegetationTypeAssetProperties>);

public:
  plVegetationTypeAssetDocument(plStringView sDocumentPath);
  ~plVegetationTypeAssetDocument();

  /// \brief Writes the runtime descriptor; used by the transform and by the window's live preview.
  plStatus WriteVegetationType(plStreamWriter& inout_stream) const;

  plVegetationPreviewWind::Enum GetPreviewWind() const { return m_PreviewWind; }
  void SetPreviewWind(plVegetationPreviewWind::Enum wind);

  /// \brief Pushes the preview plant once, as a character would.
  void TriggerTouch();

  plEvent<const plVegetationTypeAssetEvent&> m_Events;

protected:
  virtual void InitializeAfterLoading(bool bFirstTimeCreation) override;
  virtual plTransformStatus InternalTransformAsset(plStreamWriter& stream, plStringView sOutputTag, const plPlatformProfile* pAssetProfile, const plAssetFileHeader& AssetHeader, plBitflags<plTransformFlags> transformFlags) override;
  virtual plTransformStatus InternalCreateThumbnail(const ThumbnailInfo& ThumbnailInfo) override;

private:
  /// The Composite summary follows LOD 0: set while it is a foliage composite, empty otherwise. It is a temporary
  /// property, so setting it neither saves nor undoes.
  void UpdateCompositeInfo();
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void AssetCuratorEventHandler(const plAssetCuratorEvent& e);

  plVegetationPreviewWind::Enum m_PreviewWind = plVegetationPreviewWind::Light;
  plString m_sCompositeMesh; ///< LOD 0 mesh the summary was read for
};
