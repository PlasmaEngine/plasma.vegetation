#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Math/Angle.h>
#include <Foundation/Strings/String.h>
#include <GuiFoundation/Action/BaseActions.h>

class plDocument;
class plDocumentObject;
class plGameObjectDocument;
struct plVegetationStrokeRequest;

/// \brief The Fill settings of a Vegetation Layer: the plVegetationLayerComponent "Fill*" properties.
struct PL_EDITORPLUGINVEGETATION_DLL plVegetationFillSettings
{
  struct Species
  {
    plString m_sType; ///< Vegetation Type or Grass Type asset guid
    float m_fWeight = 1.0f;
  };

  plHybridArray<Species, 8> m_Species;
  float m_fTreesPerHectare = 600.0f;
  float m_fGrassDensity = 1.0f;
  float m_fGrassLength = 1.0f;
  plAngle m_GrassMaxSlope = plAngle::MakeFromDegree(40.0f);
  plVec2 m_vCenter = plVec2::MakeZero(); ///< layer space
  plVec2 m_vExtent = plVec2::MakeZero(); ///< layer space; zero = the whole layer
  plUInt32 m_uiSeed = 1;
  bool m_bReplace = false;

  /// \brief Reads the Fill properties of a plVegetationLayerComponent document object.
  void ReadFrom(const plDocumentObject* pLayerComponent);
};

/// \brief Runs Fill on layers of a scene, without any UI: sends one "Vegetation.Stroke" Fill request per layer to the
/// engine process and, once every answer is in, commits all layers as one undo step ("Vegetation Fill"). The panel's
/// Fill and the "Vegetation.Layer.Fill" action both run through here.
///
/// The region is the settings' box (layer space) or, with a zero extent, the area the layer's plants, grass coverage
/// and clearings cover. Plants of the Vegetation Types go in at Trees / ha with each type's placement rules, coverage of
/// the Grass Types at Grass Density on ground no steeper than Grass Max Slope; both keep out of the layer's clearings.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationLayerFill
{
public:
  static void Startup();
  static void Shutdown();

  /// \brief Fills each layer component with its own Fill properties.
  static plStatus FillLayers(plGameObjectDocument* pDocument, plArrayPtr<const plDocumentObject* const> layerComponents);

  /// \brief Fills one layer with the given settings and, with bWriteSettings, stores them as the layer's Fill properties
  /// in the same undo step. pBrush optionally carries the brush's placement overrides (m_bUseTypeRules and the values).
  static plStatus FillLayer(plGameObjectDocument* pDocument, const plDocumentObject* pLayerComponent, const plVegetationFillSettings& settings, bool bWriteSettings, const plVegetationStrokeRequest* pBrush = nullptr);

  /// \brief Whether a fill of this document still waits for the engine.
  static bool IsFillPending(const plDocument* pDocument);
};

/// \brief "Vegetation.Layer.Fill": fills the selected Vegetation Layers (the objects or their layer components) with
/// their Fill properties. Opens no dialog; problems go to the log.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationLayerFillAction : public plButtonAction
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationLayerFillAction, plButtonAction);

public:
  plVegetationLayerFillAction(const plActionContext& context, const char* szName);

  virtual void Execute(const plVariant& value) override;
};
