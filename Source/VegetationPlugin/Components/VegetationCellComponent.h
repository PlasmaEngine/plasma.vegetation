#pragma once

#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/SharedPtr.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <VegetationPlugin/VegetationDeclarations.h>

struct plVegetationCell;
struct plVegetationCellGroup;
struct plSceneRegistryRTChunk;
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

  /// \brief Appends the cell's ray tracing snapshots for a scene gathered around vCenter: one per type that is part
  /// of the ray tracing scene and within its range (capped by fRangeCap) of that point.
  ///
  /// A snapshot holds every instance of its type in the cell at the type's ray tracing mesh. It is built when first
  /// asked for and again only after the cell or the type changed. Called while views extract.
  void GetRTChunks(const plRenderDataManager* pRenderDataManager, const plVec3& vCenter, float fRangeCap, plDynamicArray<plSharedPtr<plRefCounted>>& out_chunks) const;

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
    float m_fFillTanHalfFov = 0.0f;
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

  /// The ray tracing snapshot of one group (type) of the cell and what it was built from.
  struct RTGroup
  {
    plSharedPtr<plSceneRegistryRTChunk> m_pChunk; ///< null: the type is not in the ray tracing scene or has no instances
    plInstanceDataOffset m_Offset;                ///< the snapshot's instances in the static instance buffer
    plGALDynamicBufferHandle m_hBuffer;
    plBoundingBox m_Bounds = plBoundingBox::MakeInvalid();
    float m_fRange = 0.0f;
    plUInt32 m_uiTypeChangeCounter = 0;
    plInt32 m_iLodOverride = -1; ///< Vegetation.RayTracing.LodOverride the snapshot was built with
    bool m_bBuilt = false;       ///< false: a resource it needs is still loading, built again on the next request
  };

  /// Instance data of a replaced snapshot. A frame still in flight may read it, so it is freed a few frames later.
  struct RetiredRTData
  {
    plInstanceDataOffset m_Offset;
    plUInt64 m_uiFrame = 0;
  };

  void BuildRTGroup(const plVegetationCellGroup& group, RTGroup& ref_state, const plRenderDataManager* pRenderDataManager) const;
  void RetireRTGroup(RTGroup& ref_state) const;
  void ReleaseRTGroups(const plRenderDataManager* pRenderDataManager, bool bAll) const;

  mutable plMutex m_RTMutex;
  mutable plUInt32 m_uiRTSyncedVersion = 0xFFFFFFFF;
  mutable plHybridArray<RTGroup, 4> m_RTGroups;
  mutable plHybridArray<RetiredRTData, 4> m_RetiredRTData;
};
