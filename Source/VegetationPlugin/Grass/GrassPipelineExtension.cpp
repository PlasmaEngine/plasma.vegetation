#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Configuration/CVar.h>

#include <RendererCore/Pipeline/Passes/AOPass.h>
#include <RendererCore/Pipeline/Passes/DepthOnlyPass.h>
#include <RendererCore/Pipeline/Passes/GBufferPass.h>
#include <RendererCore/Pipeline/Passes/OpaqueForwardRenderPass.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <VegetationPlugin/Grass/GrassContactAOPass.h>
#include <VegetationPlugin/Grass/GrassCullPass.h>
#include <VegetationPlugin/Grass/GrassPipelineExtension.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassPipelineExtension, 1, plRTTIDefaultAllocator<plGrassPipelineExtension>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plResult plGrassPipelineExtension::Apply(plRenderPipeline& ref_pipeline) const
{
  plHybridArray<plRenderPipelinePass*, 32> passes;
  ref_pipeline.GetPasses(passes);

  bool bDrawsGeometry = false;
  plGrassCullPass* pCullPass = nullptr;
  for (plRenderPipelinePass* pPass : passes)
  {
    if (auto* pCull = plDynamicCast<plGrassCullPass*>(pPass))
      pCullPass = pCull;

    if (plDynamicCast<plOpaqueForwardRenderPass*>(pPass) != nullptr || plDynamicCast<plGBufferPass*>(pPass) != nullptr ||
        plDynamicCast<plDepthOnlyPass*>(pPass) != nullptr)
    {
      bDrawsGeometry = true;
    }
  }

  if (!bDrawsGeometry && pCullPass == nullptr)
    return PL_SUCCESS;

  if (pCullPass == nullptr)
  {
    plUniquePtr<plGrassCullPass> pNewPass = PL_DEFAULT_NEW(plGrassCullPass);
    pCullPass = pNewPass.Borrow();
    ref_pipeline.AddPass(std::move(pNewPass));
  }

  // Insertion order is not a dependency: SortPasses reorders the list on every pipeline rebuild. Keep culling
  // ahead of the asset's passes explicitly, including when an asset already contains the grass pass.
  for (plRenderPipelinePass* pPass : passes)
  {
    if (pPass != pCullPass)
      PL_SUCCEED_OR_RETURN(ref_pipeline.AddPassDependency(pCullPass, pPass));
  }

  // Insert after the scene AO algorithm, before all of its consumers. Share the same resolved receiver depth.
  // No material or terrain-specific integration is needed, and shadow/picking pipelines have no AO to modify.
  for (plRenderPipelinePass* pPass : passes)
  {
    if (plDynamicCast<plAOPass*>(pPass) == nullptr)
      continue;
    const auto outputName = plMakeHashedString("Output");
    const auto* output = ref_pipeline.GetOutputConnection(pPass, outputName);
    const auto* depth = ref_pipeline.GetInputConnection(pPass, plMakeHashedString("DepthInput"));
    if (output == nullptr || depth == nullptr || output->m_Inputs.IsEmpty())
      continue;
    bool alreadyConnected = false;
    for (const auto* input : output->m_Inputs)
      alreadyConnected |= plDynamicCast<plGrassContactAOPass*>(input->m_pParent) != nullptr;
    if (alreadyConnected)
      continue;
    const auto consumers = output->m_Inputs;
    auto* depthPass = static_cast<plRenderPipelinePass*>(depth->m_pOutput->m_pParent);
    const auto depthName = depthPass->GetPinName(depth->m_pOutput);
    plUniquePtr<plGrassContactAOPass> contact = PL_DEFAULT_NEW(plGrassContactAOPass);
    auto* contactPass = contact.Borrow();
    ref_pipeline.AddPass(std::move(contact));
    PL_SUCCEED_OR_RETURN(ref_pipeline.AddPassDependency(pCullPass, contactPass));
    for (const auto* input : consumers)
    {
      auto* consumer = static_cast<plRenderPipelinePass*>(input->m_pParent);
      const auto name = consumer->GetPinName(input);
      if (!ref_pipeline.Disconnect(pPass, outputName, consumer, name) ||
          !ref_pipeline.Connect(contactPass, plMakeHashedString("AO"), consumer, name))
        return PL_FAILURE;
    }
    if (!ref_pipeline.Connect(pPass, outputName, contactPass, plMakeHashedString("AO")) ||
        !ref_pipeline.Connect(depthPass, depthName, contactPass, plMakeHashedString("Depth")))
      return PL_FAILURE;
  }
  return PL_SUCCESS;
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassPipelineExtension);
