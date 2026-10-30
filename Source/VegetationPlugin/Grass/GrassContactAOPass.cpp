#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Containers/HashSet.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/Pipeline/ViewData.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererFoundation/Device/Device.h>
#include <Shaders/Vegetation/Grass/GrassContactAOConstants.h>
#include <VegetationPlugin/Grass/GrassContactAOPass.h>
#include <VegetationPlugin/Grass/GrassRenderer.h>

plCVarFloat cvar_GrassContactAOStrength("Vegetation.Grass.ContactAO.Strength", 0.35f, plCVarFlags::Save, "Maximum ground contact occlusion under grass (0 disables). Overlaps do not accumulate.");
plCVarFloat cvar_GrassContactAORadius("Vegetation.Grass.ContactAO.Radius", 0.25f, plCVarFlags::Save, "Ground footprint radius around each grass root, in world metres.");
plCVarFloat cvar_GrassContactAOHeight("Vegetation.Grass.ContactAO.Height", 0.06f, plCVarFlags::Save, "Maximum separation from the grass root plane receiving contact AO, in metres.");
plCVarFloat cvar_GrassContactAODistance("Vegetation.Grass.ContactAO.Distance", 40.0f, plCVarFlags::Save, "Grass ground contact AO distance, fading out over the final 20 percent.");

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassContactAOPass, 1, plRTTIDefaultAllocator<plGrassContactAOPass>)
{
  PL_BEGIN_PROPERTIES
  {
    PL_MEMBER_PROPERTY("Depth", m_Depth),
    PL_MEMBER_PROPERTY("AO", m_AO),
  }
  PL_END_PROPERTIES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassContactAOPass::plGrassContactAOPass()
  : plRenderPipelinePass("Grass Contact AO", true)
{
  m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/GrassContactAO.plShader");
  // A closed root-aligned box clips correctly even when the camera intersects it. Indexed instancing transforms
  // only eight vertices per root and shades its exit faces once, rather than a conservative screen-facing quad.
  const plUInt16 indices[] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
    2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
  plGALBufferCreationDescription desc;
  desc.m_uiTotalSize = sizeof(indices);
  desc.m_uiStructSize = sizeof(plUInt16);
  desc.m_BufferFlags = plGALBufferUsageFlags::IndexBuffer;
  m_hVolumeIndices = plGALDevice::GetDefaultDevice()->CreateBuffer(desc, plMakeArrayPtr(indices).ToByteArray());
}

plGrassContactAOPass::~plGrassContactAOPass()
{
  plGALDevice::GetDefaultDevice()->DestroyBuffer(m_hVolumeIndices);
}

void plGrassContactAOPass::Prepare()
{
  m_Snapshot = {};
  const auto& data = GetPipeline()->GetRenderData();
  if (plGrassGpuManager::GetAOMode() != plGrassAOMode::All || cvar_GrassContactAOStrength <= 0.0f || cvar_GrassContactAODistance <= 0.0f ||
      data.GetViewData().m_CameraUsageHint == plCameraUsageHint::Shadow)
    return;
  plGrassGpuManager::GetRenderSnapshot(data.GetWorldIndex(), m_Snapshot);
  // Respect view exclusions and hidden grass layers, just as the blade renderer does.
  plHashSet<plUInt32> visibleSources;
  const plRenderData::Category categories[] = {plDefaultRenderDataCategories::LitOpaqueStatic, plDefaultRenderDataCategories::LitOpaqueDynamic,
    plDefaultRenderDataCategories::LitMaskedStatic, plDefaultRenderDataCategories::LitMaskedDynamic,
    plDefaultRenderDataCategories::LitForwardOpaqueStatic, plDefaultRenderDataCategories::LitForwardOpaqueDynamic,
    plDefaultRenderDataCategories::LitTransparent};
  for (auto category : categories)
  {
    for (const auto& entry : data.GetRawRenderDataWithCategory(category))
      if (const auto* grass = plDynamicCast<const plGrassRenderData*>(entry.m_pRenderData))
        visibleSources.Insert(grass->m_uiSourceSlot);
  }
  for (plUInt32 i = m_Snapshot.m_ResidentPages.GetCount(); i-- > 0;)
  {
    if (!visibleSources.Contains(m_Snapshot.m_ResidentPages[i].m_uiSource))
      m_Snapshot.m_ResidentPages.RemoveAtAndSwap(i);
  }
}

plResult plGrassContactAOPass::OnRenderGraphCacheHit(plRenderGraph& graph)
{
  PL_IGNORE_UNUSED(graph);
  const auto hStatic = m_Snapshot.m_hStatic;
  const auto hPages = m_Snapshot.m_hPages;
  const auto hSources = m_Snapshot.m_hSources;
  Prepare();
  // Page counts and transforms change freely; imported buffer replacement requires a graph rebuild.
  return m_bDeclared == m_Snapshot.IsUsable() && (!m_bDeclared ||
    (hStatic == m_Snapshot.m_hStatic && hPages == m_Snapshot.m_hPages && hSources == m_Snapshot.m_hSources)) ? PL_SUCCESS : PL_FAILURE;
}

plStatus plGrassContactAOPass::AddRenderPassesInactive(const plViewData&, const plCamera&, plRenderGraph&,
  plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  outputs[m_AO.m_uiOutputIndex] = inputs[m_AO.m_uiInputIndex];
  m_bDeclared = false;
  return plStatus(PL_SUCCESS);
}

plStatus plGrassContactAOPass::AddRenderPasses(const plViewData&, const plCamera& camera, plRenderGraph& graph,
  plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  const auto hDepth = inputs[m_Depth.m_uiInputIndex].m_TextureHandle;
  const auto hAO = inputs[m_AO.m_uiInputIndex].m_TextureHandle;
  outputs[m_AO.m_uiOutputIndex] = inputs[m_AO.m_uiInputIndex];
  m_bDeclared = false;
  if (hDepth.IsInvalidated() || hAO.IsInvalidated())
    return plStatus("Grass Contact AO requires Depth and AO inputs");
  const auto& depthDesc = graph.GetTextureDesc(hDepth);
  const auto& aoDesc = graph.GetTextureDesc(hAO);
  if (depthDesc.m_SampleCount != plGALMSAASampleCount::None || depthDesc.m_uiWidth != aoDesc.m_uiWidth || depthDesc.m_uiHeight != aoDesc.m_uiHeight)
    return plStatus("Grass Contact AO requires resolved depth and matching AO dimensions");
  Prepare();
  m_bDeclared = m_Snapshot.IsUsable();
  if (!m_bDeclared)
    return plStatus(PL_SUCCESS);

  auto pass = graph.AddGraphicsPass("Grass Contact AO");
  pass.SetStereoscopic(camera.IsStereoscopic());
  pass.ReadTexture(hDepth);
  pass.ReadBuffer(graph.ImportBuffer(m_Snapshot.m_hStatic), plGALResourceState::ShaderRead);
  pass.ReadBuffer(graph.ImportBuffer(m_Snapshot.m_hPages), plGALResourceState::ShaderRead);
  pass.ReadBuffer(graph.ImportBuffer(m_Snapshot.m_hSources), plGALResourceState::ShaderRead);
  // Preserve scene AO and its auxiliary channels. MIN blending only writes the red AO channel.
  pass.AddColorTarget(hAO);
  pass.SetExecuteCallback([this, hDepth](const plRenderGraphContext& ctx)
    {
      auto* context = ctx.GetRenderContext();
      auto& group = context->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
      group.BindTexture("ContactDepth", ctx.ResolveTexture(hDepth));
      group.BindBuffer("GrassStatic", m_Snapshot.m_hStatic);
      group.BindBuffer("GrassPages", m_Snapshot.m_hPages);
      group.BindBuffer("GrassSources", m_Snapshot.m_hSources);
      context->BindShader(m_hShader);
      context->BindMeshBuffer({}, m_hVolumeIndices, {}, plGALPrimitiveTopology::Triangles, 12);
      plGrassContactAOConstants constants = {};
      constants.ContactRadius = plMath::Clamp(float(cvar_GrassContactAORadius), 0.01f, 2.0f);
      constants.ContactHeight = plMath::Clamp(float(cvar_GrassContactAOHeight), 0.001f, 0.5f);
      constants.ContactStrength = plMath::Clamp(float(cvar_GrassContactAOStrength), 0.0f, 1.0f);
      constants.ContactDistance = plMath::Max(float(cvar_GrassContactAODistance), 0.01f);
      const plVec3 cameraPosition = GetPipeline()->GetRenderData().GetCamera().GetCenterPosition();
      for (const auto& page : m_Snapshot.m_ResidentPages)
      {
        if (page.m_WorldBounds.IsValid() && page.m_WorldBounds.GetDistanceTo(cameraPosition) > constants.ContactDistance + constants.ContactRadius)
          continue;
        constants.ContactPage = page.m_uiPage;
        context->SetPushConstants("plGrassContactAOConstants", constants);
        context->DrawMeshBuffer(12, 0, 4096).IgnoreResult();
      }
    });
  return plStatus(PL_SUCCESS);
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassContactAOPass);
