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

  /// \brief Removal probability for plants in Erase and Thin. The editor's evaluation and the engine's stroke preview
  /// share it so both remove the same instances.
  float GetPlantRemovalStrength() const
  {
    return m_uiMode == plVegetationBrushMode::Erase ? m_fStrength : plMath::Clamp(1.0f - m_fDensity, 0.0f, 1.0f) * m_fStrength;
  }

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
