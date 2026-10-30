#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Communication/Event.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Types/Uuid.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>

/// \brief One selectable type in the brush palette.
struct plVegetationBrushType
{
  plString m_sGuid; ///< asset guid string, what the layer stores
  plString m_sName;
  float m_fWeight = 1.0f;
  bool m_bSelected = false;
  bool m_bGrass = false; ///< a Grass Type, painted as coverage; otherwise a Vegetation Type (plant instances)
};

/// \brief Brush state shared by the Vegetation panel (writes) and the paint tool (reads). One per editor; the
/// target layer is per document and re-validated by the tool on every stroke.
struct PL_EDITORPLUGINVEGETATION_DLL plVegetationBrushSettings
{
  plVegetationBrushMode::Enum m_Mode = plVegetationBrushMode::Paint;
  plUuid m_LayerComponent; ///< document object guid of the target plVegetationLayerComponent
  float m_fRadius = 4.0f;
  float m_fDensity = 0.6f;
  float m_fFalloff = 0.5f;
  float m_fStrength = 0.5f;
  bool m_bAlignToNormal = true;
  float m_fAlignToNormal = 0.6f;
  plAngle m_MaxSlope = plAngle::MakeFromDegree(40.0f);
  float m_fMinScale = 0.8f;
  float m_fMaxScale = 1.25f;
  float m_fSink = 0.05f;
  float m_fMinSpacing = 0.4f;
  float m_fLength = 1.0f; ///< grass length channel written by Paint
  float m_fLastTexelSize = 0.5f; ///< Grass Texel Size given to layers created with New
  plHybridArray<plVegetationBrushType, 16> m_Types;

  plUInt32 GetSelectedTypeCount() const;
  plUInt32 GetSelectedPlantCount() const;
  plUInt32 GetSelectedGrassCount() const;

  /// \brief Place stamps plants; it is unavailable when only Grass Types are checked.
  bool IsPlaceAvailable() const { return GetSelectedGrassCount() == 0 || GetSelectedPlantCount() > 0; }

  /// \brief Checked plant types and their weights.
  void FillRequestTypes(plHybridArray<plString, 8>& out_types, plHybridArray<float, 8>& out_weights) const;

  /// \brief Checked Grass Types.
  void FillRequestGrassTypes(plHybridArray<plString, 8>& out_types) const;
};

struct plVegetationBrushEvent
{
  enum class Type
  {
    SettingsChanged, ///< the panel changed a value
    StrokeApplied,   ///< the tool committed a stroke; panels refresh their statistics
  };

  Type m_Type;
};

/// \brief Global brush settings and the event both sides listen to.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationBrush
{
public:
  static plVegetationBrushSettings s_Settings;
  static plEvent<const plVegetationBrushEvent&> s_Events;

  static void NotifySettingsChanged();
  static void NotifyStrokeApplied();
};
