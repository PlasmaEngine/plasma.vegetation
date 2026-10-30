#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <EditorUi/Content.h>
#include <Foundation/Types/UniquePtr.h>
#include <GameUiPlugin/Elements/UiDrawList.h>

class plDocument;
class plVegetationPanel;
struct plEditorUiPointerEvent;
struct plMsgUiEvent;

/// \brief The content of the Vegetation panel, drawn by the engine: the tool button, the target layer, the brush
/// mode with its key caps, the brush values, the type check lists, the coverage tools and the statistics. It
/// scrolls when the panel is shorter than its content.
///
/// The brush values live in plVegetationBrush::s_Settings; layers, types, statistics and the tool state come from
/// the panel's logic (plVegetationPanel), which the view owns.
///
/// The engine shell shows it as the Vegetation panel of a scene's frame (plVegetationPanelManager).
class plVegetationPanelView : public plEditorUiContent
{
public:
  explicit plVegetationPanelView(plDocument* pDocument);
  ~plVegetationPanelView();

  /// \brief Re-reads the panel and the brush settings. Values update in place; the document is rebuilt only when
  /// what it shows changes (types, layers, mode, enabled states).
  void Refresh();

protected:
  void OnUiUpdate() override;
  void OnUiEvent(const plMsgUiEvent& event) override;

private:
  enum class Field : plUInt8
  {
    MaxSlope,
    MinScale,
    MaxScale,
    Count
  };

  void BuildDocument();
  plString GetStructureKey() const;
  void UpdateBindings();
  void PollSliders();
  void PollFields();
  void CommitField(Field field);
  void OpenLayerMenu();
  void DrawHint();
  void ClampScroll();
  void OnPointer(const plEditorUiPointerEvent& e);

  plUniquePtr<plVegetationPanel> m_pPanel;

  bool m_bRebuild = true;
  plVec2 m_vBuiltSize = plVec2::MakeZero();
  plString m_sBuiltKey;
  float m_fContentHeight = 0.0f;
  float m_fScroll = 0.0f;
  float m_fHintTop = 0.0f;
  float m_fHintHeight = 0.0f;
  plRectFloat m_LayerRect = plRectFloat(0, 0, 0, 0);
  plInt32 m_iFocusedField = -1; ///< the number field being edited; it commits when focus leaves

  plUiDrawListWriter m_HintWriter;
  plEventSubscriptionID m_PointerSubscription = 0;
};
