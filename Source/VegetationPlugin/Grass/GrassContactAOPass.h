#pragma once

#include <RendererCore/Pipeline/RenderPipelinePass.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>

/// Adds depth-tested root footprints to scene AO, independent of the receiving mesh's material.
class PL_VEGETATIONPLUGIN_DLL plGrassContactAOPass : public plRenderPipelinePass
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassContactAOPass, plRenderPipelinePass);

public:
  plGrassContactAOPass();
  ~plGrassContactAOPass();
  virtual plStatus AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& graph,
    plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;
  virtual plStatus AddRenderPassesInactive(const plViewData& viewData, const plCamera& camera, plRenderGraph& graph,
    plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;
  virtual plResult OnRenderGraphCacheHit(plRenderGraph& graph) override;

private:
  void Prepare();
  plRenderPipelineNodeInputPin m_Depth;
  plRenderPipelineNodePassThroughPin m_AO;
  plShaderResourceHandle m_hShader;
  plGALBufferHandle m_hVolumeIndices;
  plGrassRenderSnapshot m_Snapshot;
  bool m_bDeclared = false;
};
