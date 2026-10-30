#include <VegetationPlugin/VegetationPluginPCH.h>

#include <RendererCore/Pipeline/RenderDataBatch.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/DynamicBuffer.h>
#include <VegetationPlugin/Grass/GrassCullPass.h>
#include <VegetationPlugin/Grass/GrassRenderer.h>

#include <Shaders/Vegetation/Grass/GrassGpuData.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassRenderData, 1, plRTTIDefaultAllocator<plGrassRenderData>)
PL_END_DYNAMIC_REFLECTED_TYPE;

PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassRenderer, 1, plRTTIDefaultAllocator<plGrassRenderer>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassRenderer::plGrassRenderer()
{
  // Renderer instances are initialized before parallel recording. Never mutate this shared buffer in RenderBatch.
  plDynamicArray<plUInt16> indices;
  for (plUInt32 segments = 1; segments <= GRASS_MAX_SEGMENTS; ++segments)
  {
    for (plUInt32 card = 0; card < GRASS_MAX_BILLBOARDS; ++card)
    {
      for (plUInt32 segment = 0; segment < segments; ++segment)
      {
        const plUInt16 base = static_cast<plUInt16>(card * (segments + 1) * 2 + segment * 2);
        indices.PushBack(base);
        indices.PushBack(base + 1);
        indices.PushBack(base + 2);
        indices.PushBack(base + 2);
        indices.PushBack(base + 1);
        indices.PushBack(base + 3);
      }
    }
  }
  plGALBufferCreationDescription desc;
  desc.m_uiTotalSize = indices.GetCount() * sizeof(plUInt16);
  desc.m_uiStructSize = sizeof(plUInt16);
  desc.m_BufferFlags = plGALBufferUsageFlags::IndexBuffer;
  m_hIndices = plGALDevice::GetDefaultDevice()->CreateBuffer(desc, indices.GetByteArrayPtr());
}
plGrassRenderer::~plGrassRenderer()
{
  if (!m_hIndices.IsInvalidated())
    plGALDevice::GetDefaultDevice()->DestroyBuffer(m_hIndices);
}

void plGrassRenderer::GetSupportedRenderDataTypes(plHybridArray<const plRTTI*, 8>& ref_types) const
{
  ref_types.PushBack(plGetStaticRTTI<plGrassRenderData>());
}

void plGrassRenderer::GetSupportedRenderDataCategories(plHybridArray<plRenderData::Category, 8>& ref_categories) const
{
  ref_categories.PushBack(plDefaultRenderDataCategories::LitOpaque);
  ref_categories.PushBack(plDefaultRenderDataCategories::LitForwardOpaque);
  ref_categories.PushBack(plDefaultRenderDataCategories::LitMasked);
  ref_categories.PushBack(plDefaultRenderDataCategories::LitTransparent);
  ref_categories.PushBack(plDefaultRenderDataCategories::Selection);
}

void plGrassRenderer::RenderBatch(const plRenderViewContext& renderViewContext, const plRenderPipelinePass* pPass, const plRenderDataBatch& batch) const
{
  if (pPass == nullptr || pPass->GetPipeline() == nullptr || m_hIndices.IsInvalidated())
    return;

  // The view's cull pass owns the indirect arguments and visible lists this draw consumes.
  const plGrassCullPass* pCullPass = nullptr;
  {
    plHybridArray<const plRenderPipelinePass*, 32> passes;
    pPass->GetPipeline()->GetPasses(passes);
    for (const plRenderPipelinePass* pCandidate : passes)
    {
      if (const plGrassCullPass* pCull = plDynamicCast<const plGrassCullPass*>(pCandidate))
      {
        pCullPass = pCull;
        break;
      }
    }
  }

  if (pCullPass == nullptr)
  {
    static bool s_bWarned = false;
    if (!s_bWarned)
    {
      s_bWarned = true;
      plLog::Warning("Render pipeline '{}' has no Grass Cull pass; GPU grass is not drawn in it. Pipelines built in code or without an opaque or depth-only pass do not receive one from plGrassPipelineExtension.", pPass->GetPipeline()->GetViewName());
    }
    return;
  }

  const plGrassCullPass::DrawInputs& inputs = pCullPass->GetDrawInputs();
  if (!inputs.m_bValid)
    return;

  plRenderContext* pContext = renderViewContext.m_pRenderContext;
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  const plGrassRenderSnapshot& snapshot = inputs.m_Snapshot;

  for (auto it = batch.GetIterator<plGrassRenderData>(0, batch.GetDataCount()); it.IsValid(); ++it)
  {
    const plGrassRenderData* pRenderData = it;
    if (pRenderData->m_uiWorldIndex != inputs.m_uiWorldIndex || pRenderData->m_uiSourceSlot >= snapshot.m_uiSourceCount ||
        (pRenderData->m_uiSourceSlot + 1) * GRASS_LOD_COUNT > inputs.m_uiArgsRecords || !pRenderData->m_hMaterial.IsValid())
      continue;

    const plGALDynamicBuffer* pInstanceBuffer = pDevice->GetDynamicBuffer(pRenderData->m_hInstanceDataBuffer);
    if (pInstanceBuffer == nullptr)
      continue;

    pContext->BindMaterial(pRenderData->m_hMaterial);

    plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    bindGroup.BindBuffer("perInstanceData", pInstanceBuffer->GetBufferForRendering());
    bindGroup.BindBuffer("GrassStatic", snapshot.m_hStatic);
    bindGroup.BindBuffer("GrassState", snapshot.m_hState);
    bindGroup.BindBuffer("GrassPages", snapshot.m_hPages);
    bindGroup.BindBuffer("GrassSources", snapshot.m_hSources);
    bindGroup.BindBuffer("GrassAtlas", snapshot.m_hAtlas);
    bindGroup.BindBuffer("GrassVisible", inputs.m_hVisible);

    plGrassDrawConstants constants;
    plMemoryUtils::ZeroFill(&constants, 1);
    constants.DrawDisableProceduralAO = plGrassGpuManager::GetAOMode() == plGrassAOMode::None ? 1u : 0u;
    constants.DrawInstanceDataOffset = pRenderData->m_uiInstanceDataOffset;
    constants.DrawMaterialDataOffset = pRenderData->m_uiMaterialDataOffset;
    constants.DrawSourceIndex = pRenderData->m_uiSourceSlot;
    constants.DrawSourceCount = snapshot.m_uiSourceCount;
    constants.DrawPageCount = snapshot.m_uiPageCount;
    constants.DrawVisibleEntries = inputs.m_uiVisibleEntries;
    constants.DrawStateSegments = snapshot.m_uiStateSegments;
    constants.DrawAtlasCount = snapshot.m_uiAtlasCount;
    constants.DrawPoolStrands = snapshot.m_uiPoolStrands;

    pContext->BindMeshBuffer({}, m_hIndices, {}, plGALPrimitiveTopology::Triangles, 0);

    for (plUInt32 uiLod = 0; uiLod < GRASS_LOD_COUNT; ++uiLod)
    {
      // Shadow regions, reflections and the main view can all draw the same source. Include the view and pass
      // so a device-loss breadcrumb identifies which draw actually stopped, rather than only its grass LOD.
      plStringBuilder marker;
      marker.SetFormat("Grass source {} LOD {} view '{}' pass '{}'", pRenderData->m_uiSourceSlot, uiLod,
        pPass->GetPipeline()->GetViewName(), pPass->GetName());
      pContext->GetCommandEncoder()->PushMarker(marker);
      constants.DrawLod = uiLod;
      pContext->SetPushConstants("plGrassDrawConstants", constants);
      pContext->DrawMeshBufferIndirect(inputs.m_hArgs, (pRenderData->m_uiSourceSlot * GRASS_LOD_COUNT + uiLod) * GRASS_ARGS_STRIDE).IgnoreResult();
      pContext->GetCommandEncoder()->PopMarker();
    }
  }
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassRenderer);
