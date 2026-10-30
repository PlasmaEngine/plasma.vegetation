#pragma once

#include <Foundation/Threading/Mutex.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plVegetationCell;
struct plVegetationCellGroup;
class plVegetationWorldModule;
class plRenderDataManager;

using plVegetationCellComponentManager = plComponentManager<class plVegetationCellComponent, plBlockStorageType::FreeList>;

/// \brief Internal render component on each vegetation cell object. Created by plVegetationWorldModule, never placed
/// by hand. Culls its cell's instances against the view, picks LODs and emits one instanced mesh batch per
/// (type, LOD, sub mesh).
class PL_VEGETATIONPLUGIN_DLL plVegetationCellComponent : public plRenderComponent
{
  PL_DECLARE_COMPONENT_TYPE(plVegetationCellComponent, plRenderComponent, plVegetationCellComponentManager);

  //////////////////////////////////////////////////////////////////////////
  // plComponent

protected:
  virtual void OnDeactivated() override;

  //////////////////////////////////////////////////////////////////////////
  // plRenderComponent

protected:
  virtual plResult GetLocalBounds(plBoundingBoxSphere& ref_bounds, bool& ref_bAlwaysVisible, plMsgUpdateLocalBounds& ref_msg) override;
  void OnMsgExtractRenderData(plMsgExtractRenderData& msg) const;

  //////////////////////////////////////////////////////////////////////////
  // plVegetationCellComponent

public:
  plVegetationCellComponent();
  ~plVegetationCellComponent();

  void SetCell(plVegetationCell* pCell);
  void OnCellChanged();

private:
  struct LodBatch
  {
    plInstanceDataOffset m_Offset;
    plGALDynamicBufferHandle m_hBuffer;
    plUInt32 m_uiCapacity = 0;
    plUInt32 m_uiVisible = 0;
  };

  struct SetState
  {
    plUInt64 m_uiFrame = 0;
    plVec3 m_vFillCamera = plVec3(plMath::MaxValue<float>()); ///< camera position the batches were last built for
    float m_fFillDistanceScale = -1.0f;
    float m_fFillDensity = -1.0f;
    LodBatch m_Lods[plVegetationMaxLods];
  };

  struct GroupState
  {
    SetState m_Main;
    SetState m_Shadow;
  };

  void ReleaseBatches(const plRenderDataManager* pRenderDataManager) const;
  void SyncGroupStates(const plRenderDataManager* pRenderDataManager) const;
  void BuildBatches(const plVegetationCellGroup& group, GroupState& state, bool bShadow, const plVec3& vCameraPosition, float fTanHalfFov, plMsgExtractRenderData& msg) const;

  /// \brief World-space box around every instance of the cell at its bounding radius; invalid for an empty cell.
  plBoundingBox ComputeWorldBox() const;

  /// \brief Tells the cached shadow systems that this cell's casters changed, covering the previous footprint too.
  void InvalidateCachedShadows();

  plVegetationCell* m_pCell = nullptr;
  mutable plUInt32 m_uiSyncedVersion = 0xFFFFFFFF;
  mutable plMutex m_BatchMutex;
  mutable plHybridArray<GroupState, 4> m_GroupStates;

  plBoundingBox m_LastCasterBox = plBoundingBox::MakeInvalid(); ///< footprint handed to the shadow cache last time
};
