#pragma once

#include <RendererCore/Pipeline/RenderPipelinePass.h>
#include <RendererCore/Shader/ConstantBufferStorage.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>

/// \brief Per-view GPU grass culling. On the CPU it first keeps the resident pages whose box passes the view's frustum
/// and distances (shadow views: the shadow distance from the main camera). Then it declares, ahead of the draw passes,
/// a transfer pass that resets this view's indirect arguments and uploads that page list, a compute pass ("Grass
/// Cull") that culls every strand of the listed pages into three LOD buckets per source and writes the indirect vertex
/// counts, and a pass that moves the results into the states the grass draws read them in (arguments as
/// IndirectArgument, lists and pool as vertex shader resources).
///
/// Must be declared before the passes that draw grass. The plugin registers it as the pipeline injection
/// "plasma.vegetation/GrassCull" (VegetationPluginPCH.cpp), which the engine places in every pipeline that draws
/// opaque or depth-only geometry; plGrassRenderer draws nothing for views whose pipeline has no active cull pass.
class PL_VEGETATIONPLUGIN_DLL plGrassCullPass : public plRenderPipelinePass
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassCullPass, plRenderPipelinePass);

public:
  plGrassCullPass();
  ~plGrassCullPass();

  virtual plStatus AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;
  virtual plStatus AddRenderPassesInactive(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs) override;
  virtual plResult OnRenderGraphCacheHit(plRenderGraph& ref_graph) override;

  /// \brief What this view's grass draws bind, captured when the graph was declared or re-validated this frame.
  struct DrawInputs
  {
    plGrassRenderSnapshot m_Snapshot;
    plGALBufferHandle m_hArgs;
    plGALBufferHandle m_hVisible;
    plUInt32 m_uiArgsRecords = 0;
    plUInt32 m_uiVisibleEntries = 0;
    plUInt32 m_uiWorldIndex = 0;
    bool m_bValid = false;
  };

  const DrawInputs& GetDrawInputs() const { return m_DrawInputs; }

private:
  /// What a graph build imports; a cache hit with a different result forces a rebuild.
  struct Declaration
  {
    plGALBufferHandle m_hStatic;
    plGALBufferHandle m_hState;
    plGALBufferHandle m_hPages;
    plGALBufferHandle m_hSources;
    plGALBufferHandle m_hAtlas;
    plGALBufferHandle m_hViewPages;
    plGALBufferHandle m_hArgs;
    plGALBufferHandle m_hArgsTemplate;
    plGALBufferHandle m_hVisible;
    plGALTextureHandle m_hHiZTexture;
    plGALBufferHandle m_hHiZValidity;
    bool m_bPasses = false;
    bool m_bHiZ = false;

    bool operator==(const Declaration& other) const;
  };

  /// Per-frame cull inputs, captured on the render thread before recording.
  struct FrameState
  {
    plVec4 m_Planes[6];
    plUInt32 m_uiPlaneCount = 0;
    plVec3 m_vCameraPosition = plVec3::MakeZero();
    bool m_bShadow = false;
    bool m_bOcclusion = false;
    plMat4 m_HiZViewProjection = plMat4::MakeIdentity();
    plUInt32 m_uiHiZWidth = 0;
    plUInt32 m_uiHiZHeight = 0;
    plUInt32 m_uiHiZMipCount = 0;
    plUInt32 m_uiInstanceCount = 1;
    plDynamicArray<plUInt32> m_Pages; ///< pool pages the view culls this frame
  };

  /// Computes this frame's declaration and cull inputs. With bAllowResize the per-view buffers grow to fit; otherwise
  /// a needed resize reports a mismatch through the declaration.
  Declaration Prepare(bool bAllowResize);
  void DestroyViewBuffers();
  void ExecuteCull(const plRenderGraphContext& ctx);

  Declaration m_Declared;
  DrawInputs m_DrawInputs;
  FrameState m_Frame;

  plGALBufferHandle m_hArgs;
  plGALBufferHandle m_hArgsTemplate;
  plGALBufferHandle m_hVisible;
  plGALBufferHandle m_hViewPages;
  plUInt32 m_uiArgsRecords = 0;
  plUInt32 m_uiTemplateInstanceCount = 0;
  plUInt32 m_uiVisibleEntries = 0;
  plUInt32 m_uiViewPageCapacity = 0;
  plGALBufferHandle m_hZeroValidity;

  plShaderResourceHandle m_hCullShader;
  plConstantBufferStorageHandle m_hConstants;
  plTexture2DResourceHandle m_hFallbackTexture;
};
