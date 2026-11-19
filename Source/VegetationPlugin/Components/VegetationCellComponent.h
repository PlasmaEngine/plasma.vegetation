#pragma once

#include <Foundation/Threading/Mutex.h>
#include <Foundation/Types/SharedPtr.h>
#include <RendererCore/Components/RenderComponent.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <VegetationPlugin/VegetationDeclarations.h>
#include <VegetationPlugin/World/VegetationCompositeSplit.h>

struct plVegetationCell;
struct plVegetationCompositeInfo;
class plVegetationTypeResource;
struct plVegetationCellGroup;
struct plSceneRegistryRTChunk;
class plVegetationWorldModule;
class plRenderDataManager;

using plVegetationCellComponentManager = plComponentManager<class plVegetationCellComponent, plBlockStorageType::FreeList>;

/// \brief Internal render component on each vegetation cell object. Created by plVegetationWorldModule, never placed
/// by hand. Culls its cell's instances against the view, picks LODs and emits one instanced mesh batch per
/// (type, LOD, sub mesh).
///
/// A type whose LOD 0 is a foliage composite is drawn natively instead: per sub mesh one batch of every part mesh for the
/// trees within their switch distance of the main camera, and one of the merged mesh for the others, each instance with
/// its tree's bone palette. The split is kept until a tree crosses a threshold, so camera motion alone rebuilds nothing.
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

  /// One instanced draw of a foliage composite type: a part mesh for every tree showing its parts, or the merged mesh.
  struct CompositeBatch
  {
    plInstanceDataOffset m_Offset;
    plGALDynamicBufferHandle m_hBuffer;
    plUInt32 m_uiCapacity = 0;
    plCustomInstanceDataOffset m_BonesOffset; ///< plFoliageInstanceBones per drawn instance
    plGALDynamicBufferHandle m_hBonesBuffer;
    plUInt32 m_uiBonesCapacity = 0;
    plUInt32 m_uiCount = 0;
    plDynamicArray<plUInt32> m_Trees; ///< per drawn instance, the instance index of its tree in the group
  };

  struct CompositeSet
  {
    plDynamicArray<CompositeBatch> m_Parts; ///< by composite part index
    CompositeBatch m_Merged;
    bool m_bDirty = true;         ///< the split changed (or a part mesh was loading): instances are written again
    bool m_bBonesPending = false; ///< a registered tree had no pose yet: its bone data is read again next frame
    plUInt32 m_uiBonesGeneration = 0xFFFFFFFF;
  };

  /// A foliage composite type's per-tree split and its batches, rebuilt only when a tree crosses a threshold.
  struct CompositeState
  {
    plVegetationCompositeSplit m_Split;
    plUInt64 m_uiFrame = 0; ///< frame the split was last updated in; every view of a frame uses that split
    plUInt32 m_uiTypeChangeCounter = 0xFFFFFFFF;
    CompositeSet m_Main;
    CompositeSet m_Shadow; ///< the main camera's split, without the trees that cast no shadow
  };

  struct GroupState
  {
    SetState m_Main;
    SetState m_Shadow;
    CompositeState m_Composite;
  };

  void ReleaseBatches(const plRenderDataManager* pRenderDataManager) const;
  void ReleaseCompositeSet(const plRenderDataManager* pRenderDataManager, CompositeSet& ref_set) const;
  void ReleaseCompositeBatch(const plRenderDataManager* pRenderDataManager, CompositeBatch& ref_batch) const;
  void SyncGroupStates(const plRenderDataManager* pRenderDataManager) const;
  void BuildBatches(const plVegetationCellGroup& group, GroupState& state, bool bShadow, const plVec3& vCameraPosition, float fTanHalfFov, plMsgExtractRenderData& msg) const;

  /// Draws a type whose LOD 0 is a foliage composite (see plVegetationCompositeInfo).
  void BuildCompositeBatches(const plVegetationCellGroup& group, CompositeState& ref_state, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bShadow, const plVegetationWorldModule& module, plMsgExtractRenderData& msg) const;
  void FillCompositeSet(const plVegetationCellGroup& group, CompositeSet& ref_set, plArrayPtr<const plUInt8> states, const plVegetationTypeResource& type, const plVegetationCompositeInfo& info, bool bShadow, plMsgExtractRenderData& msg) const;
  void WriteCompositeBones(const plVegetationCellGroup& group, CompositeSet& ref_set, plMsgExtractRenderData& msg) const;
  void EmitCompositeBatch(const CompositeBatch& batch, const plMeshResourceHandle& hMesh, const plVegetationTypeResource& type, const plMeshResourceHandle& hMergedMesh, const plBoundingBoxSphere& cellBounds, plMsgExtractRenderData& msg) const;

  /// \brief World-space box around every instance of the cell at its bounding radius; invalid for an empty cell.
  plBoundingBox ComputeWorldBox() const;

  /// \brief Tells the cached shadow systems that this cell's casters changed, covering the previous footprint too.
  void InvalidateCachedShadows();

  plVegetationCell* m_pCell = nullptr;
  mutable plUInt32 m_uiSyncedVersion = 0xFFFFFFFF;
  mutable plMutex m_BatchMutex;
  mutable plHybridArray<GroupState, 4> m_GroupStates;
  mutable plUInt32 m_uiFoliageBonesDataIndex = 0xFFFFFFFF; ///< custom instance data index the bone batches allocate from

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
