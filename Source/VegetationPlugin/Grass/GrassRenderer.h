#pragma once

#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Pipeline/RenderData.h>
#include <RendererCore/Pipeline/Renderer.h>
#include <RendererFoundation/RendererFoundationDLL.h>
#include <VegetationPlugin/VegetationDeclarations.h>

/// \brief One grass source in one view: which source table entry to draw and with which material and instance data.
class PL_VEGETATIONPLUGIN_DLL plGrassRenderData : public plRenderData
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassRenderData, plRenderData);

public:
  plMaterialResourceHandle m_hMaterial;
  plGALDynamicBufferHandle m_hInstanceDataBuffer;
  plUInt32 m_uiInstanceDataOffset = 0;
  plUInt32 m_uiMaterialDataOffset = 0;
  plUInt32 m_uiWorldIndex = 0;
  plUInt32 m_uiSourceSlot = plInvalidIndex;
};

/// \brief Draws grass from the GPU strand pool: binds a shared index buffer, the pool and the view's visible lists, then
/// issues one indirect draw per LOD with the arguments plGrassCullPass wrote for this view. The vertex shader of
/// Grass.plShader pulls vertices by SV_VertexID and visible strands by SV_InstanceID.
class PL_VEGETATIONPLUGIN_DLL plGrassRenderer : public plRenderer
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassRenderer, plRenderer);
  PL_DISALLOW_COPY_AND_ASSIGN(plGrassRenderer);

public:
  plGrassRenderer();
  ~plGrassRenderer();

  virtual void GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const override;
  virtual void GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const override;
  virtual void RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const override;

private:
  plGALBufferHandle m_hIndices;
};
