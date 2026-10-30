#pragma once

#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Containers/HybridArray.h>
#include <Foundation/Strings/String.h>
#include <Foundation/Types/Uuid.h>

class plDocument;
class plDocumentObject;
class plEditorUiDocumentFrame;

struct plGameObjectEvent;
struct plVegetationBrushEvent;
struct plDocumentObjectStructureEvent;
struct plDocumentObjectPropertyEvent;
struct plCommandHistoryEvent;
class plEditorEngineDocumentMsg;
class plVegetationPanelView;

/// \brief What the Vegetation panel of a scene shows and does: target layer, brush mode and values, the type palette
/// (Plants and Grass groups), coverage tools and statistics. Writes plVegetationBrush::s_Settings; the paint tool reads
/// them. Brings its panel forward when the tool activates.
///
/// Its content is engine-drawn (plVegetationPanelView), which owns it.
///
/// It also keeps every layer's GrassCoverage on the layer's GrassTexelSize: a texel size change resamples the coverage
/// inside the same transaction, so both undo as one step.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationPanel
{
  PL_DISALLOW_COPY_AND_ASSIGN(plVegetationPanel);

public:
  explicit plVegetationPanel(plDocument* pDocument);
  ~plVegetationPanel();

  /// The content that shows the panel; it refreshes when what it shows changes.
  void SetView(plVegetationPanelView* pView);

  /// \name What the engine-drawn content shows and does
  ///@{
  bool IsToolActive() const;
  void ToggleTool();
  plArrayPtr<const plString> GetLayerNames() const { return m_LayerNames; }
  plInt32 GetLayerIndex() const { return m_iLayerIndex; }
  void SetLayerIndex(plInt32 iIndex) { OnLayerChanged(iIndex); }
  void AddLayer() { OnNewLayerClicked(); }
  void RefreshTypes() { OnRefreshTypesClicked(); }
  void ResnapCoverage() { OnResnapClicked(); }
  void ClearCoverage() { OnClearCoverageClicked(); }
  bool CanResnap() const;
  bool CanClearCoverage() const;
  plStringView GetStatistics() const { return m_sStatistics; }

  /// \brief Plant instances of a type in the target layer.
  plUInt32 GetInstanceCount(plStringView sTypeGuid) const;

  /// \brief Called after the content changed the checked types.
  void OnTypesChanged();
  ///@}

private:
  void OnLayerChanged(int iIndex);
  void OnNewLayerClicked();
  void OnRefreshTypesClicked();
  void OnResnapClicked();
  void OnClearCoverageClicked();

  void GameObjectEventHandler(const plGameObjectEvent& e);
  void BrushEventHandler(const plVegetationBrushEvent& e);
  void StructureEventHandler(const plDocumentObjectStructureEvent& e);
  void PropertyEventHandler(const plDocumentObjectPropertyEvent& e);
  void CommandHistoryEventHandler(const plCommandHistoryEvent& e);
  void ProcessMessageEventHandler(const plEditorEngineDocumentMsg* pMsg);

  void RebuildLayerList();
  void RebuildTypeList();
  void UpdateStatistics();
  void UpdateTypeDependentUi();
  void CollectLayerComponents(const plDocumentObject* pObject, plDynamicArray<const plDocumentObject*>& out_components) const;
  const plDocumentObject* GetTargetLayer() const;
  void ResampleChangedLayers();
  float GetStrandsPerSquareMeter(plStringView sGrassType) const;

  void RefreshView();

  plDocument* m_pDocument = nullptr;

  plVegetationPanelView* m_pView = nullptr;   ///< the engine-drawn content, which owns the panel
  plHybridArray<plString, 8> m_LayerNames;    ///< "(no layer)", then every layer
  plHybridArray<plUuid, 8> m_LayerGuids;      ///< the layer components; invalid for "(no layer)"
  plInt32 m_iLayerIndex = 0;
  plString m_sStatistics;

  plHybridArray<plUuid, 4> m_TexelSizeChanged; ///< layers whose GrassTexelSize changed in the open transaction
  plUInt32 m_uiResnapRequest = 0;             ///< id of the Resnap request in flight, 0 when none
};

/// \brief Shows a Vegetation panel in the frame of every scene in the engine shell: a tab beside Properties.
class PL_EDITORPLUGINVEGETATION_DLL plVegetationPanelManager
{
public:
  static void Startup();
  static void Shutdown();

private:
  static void AddShellPanel(plEditorUiDocumentFrame& ref_frame);
};
