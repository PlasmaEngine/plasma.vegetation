#pragma once

#include <RendererCore/Pipeline/RenderPipelineExtension.h>
#include <VegetationPlugin/VegetationPluginDLL.h>

/// \brief Adds a plGrassCullPass to every render pipeline created from an asset while the vegetation plugin is loaded.
///
/// The pass goes into pipelines that draw opaque geometry (a plOpaqueForwardRenderPass, including the deferred
/// forward pass, or a plGBufferPass) and into depth-only pipelines (a plDepthOnlyPass without an opaque pass, which
/// is how the shadow pipelines are built), so grass casts shadows. Pipelines that already hold a plGrassCullPass are
/// left alone. The pass has no pins; plRenderPipeline::SortPasses orders a pin-less pass added last ahead of every
/// connected pass, so its cull dispatch is declared before the opaque and depth passes that draw grass.
///
/// Pipeline assets never contain the pass, so they still load when the plugin is missing.
class PL_VEGETATIONPLUGIN_DLL plGrassPipelineExtension : public plRenderPipelineExtension
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassPipelineExtension, plRenderPipelineExtension);

public:
  virtual plResult Apply(plRenderPipeline& ref_pipeline) const override;
};
