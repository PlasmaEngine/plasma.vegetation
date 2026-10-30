#pragma once

#include <EditorFramework/Gizmos/GizmoBase.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>

class plVegetationPaintEditTool;

/// \brief Input context of the Vegetation paint tool: follows the surface under the cursor with a brush ring drawn by
/// the engine, and turns drags into stroke segments on the tool.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationBrushContext : public plGizmo
{
  PL_ADD_DYNAMIC_REFLECTION(plVegetationBrushContext, plGizmo);

public:
  plVegetationBrushContext();
  ~plVegetationBrushContext();

  void SetTool(plVegetationPaintEditTool* pTool) { m_pTool = pTool; }

  virtual void UpdateStatusBarText(plEngineDocumentWindow* pWindow) override;

protected:
  virtual void OnSetOwner(plEngineDocumentWindow* pOwnerWindow, plEngineViewWidget* pOwnerView) override;
  virtual void OnVisibleChanged(bool bVisible) override;
  virtual void OnTransformationChanged(const plTransform& transform) override;
  virtual void DoFocusLost(bool bCancel) override;

  virtual plEditorInput DoKeyPressEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoKeyReleaseEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMousePressEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseReleaseEvent(const plEditorInputEvent& e) override;
  virtual plEditorInput DoMouseMoveEvent(const plEditorInputEvent& e) override;

private:
  bool UpdateCursor(const plVec2I32& vViewPos);
  void UpdateRing();

  /// \brief Ctrl turns any stroke into Erase: the ring takes the danger colour and the status bar says Erase.
  void SetEraseHeld(bool bHeld);

  plVegetationPaintEditTool* m_pTool = nullptr;
  plEngineGizmoHandle m_hRing;
  plEngineGizmoHandle m_hInnerRing;

  bool m_bHasSurface = false;
  plVec3 m_vCursor = plVec3::MakeZero();
  plVec3 m_vCursorNormal = plVec3(0, 0, 1);
  bool m_bDragging = false;
  bool m_bEraseOverride = false;
  bool m_bEraseHeld = false; ///< Ctrl is down
  plVec3 m_vLastStamp = plVec3::MakeZero();
};
