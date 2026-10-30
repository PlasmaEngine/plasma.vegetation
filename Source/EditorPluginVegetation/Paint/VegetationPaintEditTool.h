#pragma once

#include <EditorFramework/EditTools/EditTool.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorPluginVegetation/Paint/VegetationBrushContext.h>
#include <VegetationPlugin/Components/VegetationBrushProtocol.h>

class plEditorEngineDocumentMsg;
struct plGameObjectEvent;
struct plManipulatorManagerEvent;
struct plVegetationBrushEvent;

/// \brief The Vegetation paint tool of the scene editor. Paint and place strokes are evaluated in the engine process
/// against scene collision; erase and thin run on the editor side. Every stroke ends as one undo step that sets the
/// target layer's Data blob, its GrassCoverage blob, or both. While the stroke is running the engine mirrors it on a
/// preview of the layer, so it renders with the mouse still down. While active, the engine shows the target layer's
/// coverage overlay.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationPaintEditTool : public plGameObjectEditTool
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationPaintEditTool, plGameObjectEditTool);

public:
  plVegetationPaintEditTool();
  ~plVegetationPaintEditTool();

  virtual plEditorInputContext* GetEditorInputContextOverride() override;
  virtual plEditToolSupportedSpaces GetSupportedSpaces() const override;
  virtual bool GetSupportsMoveParentOnly() const override;

  // called by the brush context
  void BeginStroke(plVegetationBrushMode::Enum mode);
  void StrokeSegment(const plVec3& vCenter, const plVec3& vNormal);
  void EndStroke();
  void PlaceSingle(const plVec3& vCenter, const plVec3& vNormal);

  /// \brief Place mode: asks the engine to show a ghost of the plant where a click would put it. The ghost uses the
  /// same seed as the click, so what is shown is what gets placed.
  void UpdatePlacePreview(const plVec3& vCenter, const plVec3& vNormal);
  void HidePlacePreview();

  void Deactivate();

  /// \brief Finds the layer component object the settings point at, or nullptr when it is gone.
  const plDocumentObject* GetLayerComponentObject() const;

protected:
  virtual void OnConfigured() override;
  virtual void OnActiveChanged(bool bIsActive) override;

private:
  void GameObjectEventHandler(const plGameObjectEvent& e);
  void ManipulatorManagerEventHandler(const plManipulatorManagerEvent& e);
  void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg);
  void BrushEventHandler(const plVegetationBrushEvent& e);
  void UpdateGizmoState();
  void SetPickTransparent(bool bEnable);
  void SendOverlay();

  /// \brief The layer component object with this guid, or nullptr when it is gone or not a layer.
  const plDocumentObject* GetLayerComponentObject(const plUuid& layer) const;

  bool LoadLayerData(plVegetationLayerData& out_data, plGrassCoverageData& out_grass, plTransform& out_layerTransform, plUuid& out_layer) const;
  void FillRequest(plVegetationStrokeRequest& out_request, plVegetationBrushMode::Enum mode, const plVec3& vCenter, const plVec3& vNormal) const;
  void SendSegment(plVegetationBrushMode::Enum mode, const plVec3& vCenter, const plVec3& vNormal);
  void SendRequest(const plVegetationStrokeRequest& request);
  void RollPlaceSeed();
  void CommitStroke();

  /// \brief Tells the engine to drop its stroke preview. Sent before the stroke's transaction, so the committed
  /// layer data takes over without a gap.
  void DropStrokePreview();

  plVegetationBrushContext m_BrushContext;

  plUInt32 m_uiPlaceSeed = 1;            ///< shared by the ghost preview and the click that follows it
  bool m_bPreviewShown = false;          ///< a ghost is (or may be) visible in the engine
  plVec3 m_vPreviewCenter = plVec3::MakeZero();
  plVec3 m_vPreviewNormal = plVec3(0, 0, 1);

  bool m_bStrokeActive = false;
  bool m_bCommitPending = false;
  plVegetationBrushMode::Enum m_StrokeMode = plVegetationBrushMode::Paint;
  plUInt32 m_uiStrokeId = 0;
  plUInt32 m_uiNextSegment = 0;
  plUInt32 m_uiPendingSegments = 0;

  plVegetationLayerData m_StrokeData; ///< the layer's data as it will be after this stroke (layer local space)
  plGrassCoverageData m_StrokeGrass;  ///< the layer's grass coverage as it will be after this stroke (layer local space)
  plTransform m_LayerTransform = plTransform::MakeIdentity();
  plUuid m_StrokeLayer; ///< layer component the stroke was loaded from and commits to
  bool m_bStrokeDirty = false;
  bool m_bStrokeGrassDirty = false;
  bool m_bStrokePreviewSent = false; ///< the engine holds a preview of the running stroke

  plUuid m_OverlayLayer; ///< layer whose coverage overlay the engine shows
};
