#pragma once

#include <Foundation/Math/Angle.h>
#include <Foundation/Math/Math.h>
#include <Foundation/Types/Uuid.h>
#include <Foundation/Math/Transform.h>
#include <VegetationPlugin/Components/VegetationLayerData.h>
#include <VegetationPlugin/Grass/GrassCoverageData.h>

/// \brief The editor's paint tool and the engine process exchange strokes with these messages
/// (plSimpleDocumentConfigMsgToEngine "Vegetation.Stroke" and plSimpleDocumentConfigMsgToEditor
/// "Vegetation.StrokeResult", payload value = data buffer). The engine evaluates placement and grass coverage against
/// scene collision; the editor commits the result to the layer's Data and GrassCoverage properties as one undo step.
///
/// "Vegetation.Resnap" / "Vegetation.ResnapResult" re-raycast a layer's painted coverage. "Vegetation.Overlay" (payload
/// value = plUuid of the target layer component, invalid to hide) switches the engine's coverage overlay.
///
/// While a stroke is running the engine mirrors every stamp onto a stroke preview of the target layer, so the plants
/// and grass of the stroke render before it is committed. "Vegetation.DropPreview" drops that preview again.
struct plVegetationBrushMode
{
  using StorageType = plUInt8;

  enum Enum : plUInt8
  {
    Paint,
    Erase,
    Thin,
    Place,
    Fill,     ///< scatters the checked plants over a rectangle (m_vFillMin, m_vFillMax) at m_fTreesPerHectare
    Clearing, ///< paints clearings into the layer (removing the plants inside); with m_bRemoveClearings it erases them

    Default = Paint
  };
};

struct PL_VEGETATIONPLUGIN_DLL plVegetationStrokeRequest
{
  static constexpr const char* s_szWhatToDo = "Vegetation.Stroke";

  plUInt8 m_uiMode = plVegetationBrushMode::Paint;
  plUuid m_LayerGuid;
  plUInt32 m_uiStrokeId = 0;
  plUInt32 m_uiSegment = 0;

  plVec3 m_vCenter = plVec3::MakeZero(); ///< world space pick position
  plVec3 m_vNormal = plVec3(0, 0, 1);    ///< world space pick normal
  float m_fRadius = 4.0f;
  float m_fDensity = 0.6f;
  float m_fFalloff = 0.5f;
  float m_fStrength = 0.5f;
  float m_fAlignToNormal = 0.6f;
  plAngle m_MaxSlope = plAngle::MakeFromDegree(45.0f);
  float m_fMinScale = 0.8f;
  float m_fMaxScale = 1.25f;
  float m_fSink = 0.05f;
  float m_fMinSpacing = 0.4f;
  plUInt32 m_uiSeed = 0;

  /// Place mode only: evaluate the placement and show it as a ghost in the scene instead of answering. An empty
  /// type list hides the ghost.
  bool m_bPreview = false;

  /// Mirror this stamp onto the engine's stroke preview of the target layer, so it renders right away.
  bool m_bStrokePreview = false;

  /// Erase and Thin: the editor evaluated the stamp itself, the engine only mirrors it and sends no result.
  bool m_bMirrorOnly = false;

  plHybridArray<plString, 8> m_Types; ///< plant (Vegetation Type) guids
  plHybridArray<float, 8> m_Weights;

  plHybridArray<plString, 8> m_GrassTypes; ///< Grass Type guids, painted as coverage
  float m_fLength = 1.0f;                  ///< grass length channel, 0..1
  float m_fTexelSize = plGrassCoverageData::DefaultTexelSize; ///< the target layer's Grass Texel Size
  plTransform m_LayerTransform = plTransform::MakeIdentity(); ///< global transform of the layer object; coverage is in its local space

  // version 5

  /// Paint, Place and Fill take slope, scale, spacing, sink and alignment from each Vegetation Type; false uses the
  /// brush values above for every type. Requests of version 4 read as false, which is how they placed.
  bool m_bUseTypeRules = true;

  /// Fill: the region in the layer's local space; x and y span the rectangle, z the heights around which the ground is
  /// searched. Plants of m_Types go in at m_fTreesPerHectare (existing ones count), grass coverage of m_GrassTypes at
  /// m_fDensity and m_fLength on ground no steeper than m_MaxSlope.
  plVec3 m_vFillMin = plVec3::MakeZero();
  plVec3 m_vFillMax = plVec3::MakeZero();
  float m_fTreesPerHectare = 600.0f; ///< Fill: wanted plants of the checked types per 10000 m^2, existing ones count
  bool m_bRemoveClearings = false;   ///< Clearing: the stamp erases clearings instead of painting one
  bool m_bFillReplace = false;       ///< Fill: the region's plants and coverage of the fill types go first (ApplyFillReplace)

  /// \brief Removal probability for plants in Erase and Thin. The editor's evaluation and the engine's stroke preview
  /// share it so both remove the same instances.
  float GetPlantRemovalStrength() const
  {
    return m_uiMode == plVegetationBrushMode::Erase ? m_fStrength : plMath::Clamp(1.0f - m_fDensity, 0.0f, 1.0f) * m_fStrength;
  }

  /// \brief Applies a Clearing stamp to layer data (layer space): adds a clearing of the brush radius and removes every
  /// plant inside it, or with m_bRemoveClearings erases the clearings whose centre is under the brush. The editor's
  /// evaluation and the engine's stroke preview share it. Returns whether the data changed.
  bool ApplyClearingStamp(plVegetationLayerData& inout_data) const;

  /// \brief Fill with m_bFillReplace: removes the plants of m_Types and the coverage of m_GrassTypes inside the fill
  /// region (layer space). The editor applies it before merging the result, the engine evaluates against data it was
  /// applied to. Returns whether anything changed; does nothing without m_bFillReplace.
  bool ApplyFillReplace(plVegetationLayerData& inout_data, plGrassCoverageData* pGrass) const;

  void Write(plStreamWriter& inout_stream) const;
  plResult Read(plStreamReader& inout_stream);
  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);
};

struct PL_VEGETATIONPLUGIN_DLL plVegetationStrokeResult
{
  static constexpr const char* s_szWhatToDo = "Vegetation.StrokeResult";

  plUuid m_LayerGuid;
  plUInt32 m_uiStrokeId = 0;
  plUInt32 m_uiSegment = 0;
  plVegetationLayerData m_Added;      ///< world space
  plGrassCoverageData m_GrassDelta;   ///< layer space; merge with plGrassCoverageData::MergeDelta

  void Write(plStreamWriter& inout_stream) const;
  plResult Read(plStreamReader& inout_stream);
  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);
};

/// \brief Re-raycasts every painted texel of a layer's coverage, keeping density and length.
struct PL_VEGETATIONPLUGIN_DLL plVegetationResnapRequest
{
  static constexpr const char* s_szWhatToDo = "Vegetation.Resnap";

  plUuid m_LayerGuid;
  plUInt32 m_uiRequestId = 0;
  plTransform m_LayerTransform = plTransform::MakeIdentity();
  plDataBuffer m_Coverage; ///< the layer's GrassCoverage blob

  void Write(plStreamWriter& inout_stream) const;
  plResult Read(plStreamReader& inout_stream);
  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);
};

struct PL_VEGETATIONPLUGIN_DLL plVegetationResnapResult
{
  static constexpr const char* s_szWhatToDo = "Vegetation.ResnapResult";

  plUuid m_LayerGuid;
  plUInt32 m_uiRequestId = 0;
  plUInt32 m_uiResnappedTexels = 0;
  plDataBuffer m_Coverage; ///< the new GrassCoverage blob

  void Write(plStreamWriter& inout_stream) const;
  plResult Read(plStreamReader& inout_stream);
  void ToBuffer(plDataBuffer& out_buffer) const;
  plResult FromBuffer(const plDataBuffer& buffer);
};

struct plVegetationOverlayMsg
{
  static constexpr const char* s_szWhatToDo = "Vegetation.Overlay";
};

/// \brief Drops the document's stroke preview: the layer goes back to the instances and coverage of its committed
/// blobs. Sent right before the stroke's transaction, so the committed data takes over without a gap.
struct plVegetationDropPreviewMsg
{
  static constexpr const char* s_szWhatToDo = "Vegetation.DropPreview";
};
