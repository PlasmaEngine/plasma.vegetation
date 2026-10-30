#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Foundation/Configuration/CVar.h>
#include <Foundation/Math/Frustum.h>
#include <RendererCore/Pipeline/ExtractedRenderData.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/Culling/GpuCullingContext.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderGraph/RenderGraph.h>
#include <RendererCore/RenderGraph/RenderGraphContext.h>
#include <RendererCore/RenderGraph/RenderGraphPassBuilder.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Profiling/Profiling.h>
#include <RendererFoundation/Resources/Buffer.h>
#include <VegetationPlugin/Grass/GrassCullPass.h>

#include <Shaders/Vegetation/Grass/GrassGpuData.h>

plCVarBool cvar_VegetationGrassHiZ("Vegetation.Grass.HiZ", true, plCVarFlags::Default, "Tests strands of main and editor views against the previous frame's Hi-Z pyramid of the mesh GPU culling. Off: frustum and distance culling only; the pyramid stays bound");

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassCullPass, 1, plRTTIDefaultAllocator<plGrassCullPass>)
{
  PL_BEGIN_ATTRIBUTES
  {
    new plCategoryAttribute("Vegetation"),
  }
  PL_END_ATTRIBUTES;
}
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

namespace
{
  constexpr plUInt32 s_uiGrassArgsGranularity = 64 * GRASS_LOD_COUNT;
  constexpr plUInt32 s_uiGrassVisibleGranularity = 64 * 1024;
  constexpr plUInt32 s_uiGrassViewPageGranularity = 256;

  plGALBufferHandle GrassCreateViewBuffer(plUInt32 uiTotalSize, plUInt32 uiStructSize, plBitflags<plGALBufferUsageFlags> flags, plArrayPtr<const plUInt8> initialData, const char* szName)
  {
    plGALBufferCreationDescription desc;
    desc.m_uiStructSize = uiStructSize;
    desc.m_uiTotalSize = uiTotalSize;
    desc.m_BufferFlags = flags;
    desc.m_ResourceAccess.m_bImmutable = !initialData.IsEmpty();

    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    plGALBufferHandle hBuffer = pDevice->CreateBuffer(desc, initialData);
    if (const plGALBuffer* pBuffer = pDevice->GetBuffer(hBuffer))
      pBuffer->SetDebugName(szName);
    return hBuffer;
  }
} // namespace

bool plGrassCullPass::Declaration::operator==(const Declaration& other) const
{
  return m_hStatic == other.m_hStatic && m_hState == other.m_hState && m_hPages == other.m_hPages && m_hSources == other.m_hSources &&
         m_hAtlas == other.m_hAtlas && m_hViewPages == other.m_hViewPages && m_hArgs == other.m_hArgs && m_hArgsTemplate == other.m_hArgsTemplate &&
         m_hVisible == other.m_hVisible && m_hHiZTexture == other.m_hHiZTexture && m_hHiZValidity == other.m_hHiZValidity &&
         m_bPasses == other.m_bPasses && m_bHiZ == other.m_bHiZ;
}

plGrassCullPass::plGrassCullPass()
  : plRenderPipelinePass("GrassCullPass", true)
{
  m_hCullShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/GrassCull.plShader");
  m_hConstants = plRenderContext::CreateConstantBufferStorage<plGrassCullConstants>();
  m_hFallbackTexture = plResourceManager::LoadResource<plTexture2DResource>("White.color");

  const plUInt32 uiZero = 0;
  m_hZeroValidity = GrassCreateViewBuffer(sizeof(plUInt32), sizeof(plUInt32), plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource,
    plMakeArrayPtr(&uiZero, 1).ToByteArray(), "Grass Cull - Zero Validity");
}

plGrassCullPass::~plGrassCullPass()
{
  DestroyViewBuffers();

  if (!m_hZeroValidity.IsInvalidated())
    plGALDevice::GetDefaultDevice()->DestroyBuffer(m_hZeroValidity);

  plRenderContext::DeleteConstantBufferStorage(m_hConstants);
}

void plGrassCullPass::DestroyViewBuffers()
{
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  for (plGALBufferHandle* pHandle : {&m_hArgs, &m_hArgsTemplate, &m_hVisible, &m_hViewPages})
  {
    if (!pHandle->IsInvalidated())
      pDevice->DestroyBuffer(*pHandle);
  }
  m_uiArgsRecords = 0;
  m_uiTemplateInstanceCount = 0;
  m_uiVisibleEntries = 0;
  m_uiViewPageCapacity = 0;
}

plGrassCullPass::Declaration plGrassCullPass::Prepare(bool bAllowResize)
{
  Declaration decl;
  m_DrawInputs = DrawInputs();

  const plExtractedRenderData& data = GetPipeline()->GetRenderData();
  const plViewData& viewData = data.GetViewData();
  const plCamera& camera = data.GetCamera();

  plGrassRenderSnapshot snapshot;
  if (!plGrassGpuManager::GetRenderSnapshot(data.GetWorldIndex(), snapshot) || !snapshot.IsUsable())
    return decl;

  const plUInt32 uiInstanceCount = camera.IsStereoscopic() ? 2 : 1;
  const plUInt32 uiNeededRecords = snapshot.m_uiSourceCount * GRASS_LOD_COUNT;
  const plUInt32 uiNeededVisible = plMath::Max(snapshot.m_uiVisibleEntries, 1u);
  const plUInt32 uiNeededPages = plMath::Max(snapshot.m_uiPageCount, 1u);
  const bool bResize = m_hArgs.IsInvalidated() || m_hVisible.IsInvalidated() || m_hArgsTemplate.IsInvalidated() || m_hViewPages.IsInvalidated() ||
                       uiNeededRecords > m_uiArgsRecords || uiNeededVisible > m_uiVisibleEntries || uiNeededPages > m_uiViewPageCapacity ||
                       uiInstanceCount != m_uiTemplateInstanceCount;

  if (bResize)
  {
    if (!bAllowResize)
    {
      // Report a declaration no cached graph can match.
      decl.m_bPasses = true;
      return decl;
    }

    DestroyViewBuffers();

    m_uiArgsRecords = ((uiNeededRecords + s_uiGrassArgsGranularity - 1) / s_uiGrassArgsGranularity) * s_uiGrassArgsGranularity;
    m_uiVisibleEntries = ((uiNeededVisible + s_uiGrassVisibleGranularity - 1) / s_uiGrassVisibleGranularity) * s_uiGrassVisibleGranularity;
    m_uiTemplateInstanceCount = uiInstanceCount;

    m_hArgs = GrassCreateViewBuffer(m_uiArgsRecords * GRASS_ARGS_STRIDE, 0,
      plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess | plGALBufferUsageFlags::DrawIndirect, {}, "Grass Cull - Args");

    plDynamicArray<plUInt32> templateArgs;
    templateArgs.SetCount(m_uiArgsRecords * 5);
    plMemoryUtils::ZeroFill(templateArgs.GetData(), templateArgs.GetCount());
    m_hArgsTemplate = GrassCreateViewBuffer(m_uiArgsRecords * GRASS_ARGS_STRIDE, 0, plGALBufferUsageFlags::ByteAddressBuffer | plGALBufferUsageFlags::ShaderResource,
      templateArgs.GetByteArrayPtr(), "Grass Cull - Args Template");

    m_hVisible = GrassCreateViewBuffer(m_uiVisibleEntries * sizeof(plUInt32), sizeof(plUInt32),
      plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::UnorderedAccess, {}, "Grass Cull - Visible");

    m_uiViewPageCapacity = ((uiNeededPages + s_uiGrassViewPageGranularity - 1) / s_uiGrassViewPageGranularity) * s_uiGrassViewPageGranularity;
    m_hViewPages = GrassCreateViewBuffer(m_uiViewPageCapacity * sizeof(plUInt32), sizeof(plUInt32),
      plGALBufferUsageFlags::StructuredBuffer | plGALBufferUsageFlags::ShaderResource, {}, "Grass Cull - View Pages");
  }

  decl.m_bPasses = true;
  decl.m_hStatic = snapshot.m_hStatic;
  decl.m_hState = snapshot.m_hState;
  decl.m_hPages = snapshot.m_hPages;
  decl.m_hSources = snapshot.m_hSources;
  decl.m_hAtlas = snapshot.m_hAtlas;
  decl.m_hViewPages = m_hViewPages;
  decl.m_hArgs = m_hArgs;
  decl.m_hArgsTemplate = m_hArgsTemplate;
  decl.m_hVisible = m_hVisible;

  // Frustum; shadow views skip the near and far planes so casters outside the depth range still land in the map.
  FrameState& frame = m_Frame;
  frame = FrameState();
  frame.m_bShadow = viewData.m_CameraUsageHint == plCameraUsageHint::Shadow;
  frame.m_vCameraPosition = viewData.m_InverseViewMatrix[0].GetTranslationVector();
  frame.m_uiInstanceCount = uiInstanceCount;

  plFrustum frustum;
  const bool bHasFrustum = uiInstanceCount == 1 && plFrustum::TryMakeFromMVP(frustum, viewData.m_ViewProjectionMatrix[0]).Succeeded();
  if (bHasFrustum)
  {
    for (plUInt32 i = 0; i < plFrustum::PLANE_COUNT; ++i)
    {
      if (frame.m_bShadow && (i == plFrustum::NearPlane || i == plFrustum::FarPlane))
        continue;

      // plFrustum planes face outward; the shader tests inside-positive planes.
      const plPlane& plane = frustum.GetPlane(static_cast<plUInt8>(i));
      frame.m_Planes[frame.m_uiPlaneCount++] = plVec4(-plane.m_vNormal.x, -plane.m_vNormal.y, -plane.m_vNormal.z, -plane.m_fNegDistance);
    }
  }

  // Page prefilter: the same planes and distances the strand test applies, on each page's conservative box. A page
  // without a box (no bounds known yet) is always culled on the GPU. Shadow views measure from the main camera and
  // stop at the shadow distance, so a shadow view far from the grass dispatches nothing. Pages of a source whose type
  // casts no shadows never enter a shadow view.
  frame.m_Pages.Reserve(snapshot.m_ResidentPages.GetCount());
  for (const plGrassPageCullInfo& info : snapshot.m_ResidentPages)
  {
    if (frame.m_bShadow && !info.m_bCastShadows)
      continue;

    if (info.m_WorldBounds.IsValid())
    {
      const plVec3 vReference = frame.m_bShadow ? snapshot.m_vMainCamera : frame.m_vCameraPosition;
      const float fDistance = info.m_WorldBounds.GetDistanceTo(vReference);
      if (fDistance > info.m_fViewDistance || (frame.m_bShadow && fDistance > info.m_fShadowDistance))
        continue;

      bool bOutside = false;
      if (bHasFrustum)
      {
        for (plUInt32 i = 0; i < plFrustum::PLANE_COUNT && !bOutside; ++i)
        {
          if (frame.m_bShadow && (i == plFrustum::NearPlane || i == plFrustum::FarPlane))
            continue;

          // Outward-facing planes: a box entirely in front of one lies outside the frustum.
          bOutside = frustum.GetPlane(static_cast<plUInt8>(i)).GetObjectPosition(info.m_WorldBounds) == plPositionOnPlane::Front;
        }
      }
      if (bOutside)
        continue;
    }

    frame.m_Pages.PushBack(info.m_uiPage);
  }

  // Hi-Z for main views, only when the mesh culling of this graph build declared its own read of the pyramid (a resize
  // then waits for the next build) and the pyramid is exactly one frame old.
  const bool bMainView = viewData.m_CameraUsageHint == plCameraUsageHint::MainView || viewData.m_CameraUsageHint == plCameraUsageHint::EditorView;
  if (const plGpuCullingContext* pCulling = GetPipeline()->GetGpuCullingContext(); pCulling != nullptr && bMainView && uiInstanceCount == 1)
  {
    if (pCulling->IsHiZReadDeclared() && !pCulling->GetHiZTexture().IsInvalidated() && !pCulling->GetHiZValidityBuffer().IsInvalidated())
    {
      decl.m_bHiZ = true;
      decl.m_hHiZTexture = pCulling->GetHiZTexture();
      decl.m_hHiZValidity = pCulling->GetHiZValidityBuffer();

      // The declaration (and so the cached graph) is unchanged by the cvar; only the occlusion flag of the dispatch follows it.
      frame.m_bOcclusion = cvar_VegetationGrassHiZ && pCulling->GetHiZBuiltFrame() != 0 && pCulling->GetHiZBuiltFrame() + 1 == plRenderWorld::GetFrameCounter();
      frame.m_HiZViewProjection = pCulling->GetHiZViewProjection();
      frame.m_uiHiZWidth = pCulling->GetHiZWidth();
      frame.m_uiHiZHeight = pCulling->GetHiZHeight();
      frame.m_uiHiZMipCount = pCulling->GetHiZMipCount();
    }
  }

  m_DrawInputs.m_Snapshot = snapshot;
  m_DrawInputs.m_hArgs = m_hArgs;
  m_DrawInputs.m_hVisible = m_hVisible;
  m_DrawInputs.m_uiArgsRecords = m_uiArgsRecords;
  m_DrawInputs.m_uiVisibleEntries = m_uiVisibleEntries;
  m_DrawInputs.m_uiWorldIndex = data.GetWorldIndex();
  m_DrawInputs.m_bValid = true;
  return decl;
}

plStatus plGrassCullPass::AddRenderPasses(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  PL_IGNORE_UNUSED(viewData);
  PL_IGNORE_UNUSED(camera);
  PL_IGNORE_UNUSED(inputs);
  PL_IGNORE_UNUSED(outputs);

  m_Declared = Prepare(true);
  if (!m_Declared.m_bPasses || !m_DrawInputs.m_bValid)
    return plStatus(PL_SUCCESS);

  const Declaration& decl = m_Declared;
  const plRenderGraphBufferHandle hArgs = ref_graph.ImportBuffer(decl.m_hArgs);
  const plRenderGraphBufferHandle hArgsTemplate = ref_graph.ImportBuffer(decl.m_hArgsTemplate);
  const plRenderGraphBufferHandle hVisible = ref_graph.ImportBuffer(decl.m_hVisible);
  const plRenderGraphBufferHandle hStatic = ref_graph.ImportBuffer(decl.m_hStatic);
  const plRenderGraphBufferHandle hState = ref_graph.ImportBuffer(decl.m_hState);
  const plRenderGraphBufferHandle hPages = ref_graph.ImportBuffer(decl.m_hPages);
  const plRenderGraphBufferHandle hSources = ref_graph.ImportBuffer(decl.m_hSources);
  const plRenderGraphBufferHandle hAtlas = ref_graph.ImportBuffer(decl.m_hAtlas);
  const plRenderGraphBufferHandle hViewPages = ref_graph.ImportBuffer(decl.m_hViewPages);

  {
    // The page list is declared by no pass of this graph before the cull reads it, so it sits in its resting state,
    // which an AheadOfTime update transitions from and back to (as the grass PreRender graph uploads its tables).
    auto pass = ref_graph.AddTransferPass("Grass Cull Reset");
    pass.HasSideEffects();
    pass.ReadBuffer(hArgsTemplate, plGALResourceState::CopySource);
    pass.WriteBuffer(hArgs, plGALResourceState::CopyDestination);
    pass.SetExecuteCallback([this](const plRenderGraphContext& ctx)
      {
        if (!m_DrawInputs.m_bValid)
          return;

        ctx.GetCommandEncoder()->CopyBufferRegion(m_Declared.m_hArgs, 0, m_Declared.m_hArgsTemplate, 0, m_DrawInputs.m_uiArgsRecords * GRASS_ARGS_STRIDE);
        if (!m_Frame.m_Pages.IsEmpty())
          ctx.GetCommandEncoder()->UpdateBuffer(m_Declared.m_hViewPages, 0, m_Frame.m_Pages.GetByteArrayPtr(), plGALUpdateMode::AheadOfTime);
      });
  }

  {
    auto pass = ref_graph.AddComputePass("Grass Cull");
    pass.HasSideEffects();
    pass.ReadBuffer(hStatic, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(hPages, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(hSources, plGALResourceState::ComputeShaderRead);
    pass.ReadBuffer(hViewPages, plGALResourceState::ComputeShaderRead);
    pass.WriteBuffer(hArgs);
    pass.WriteBuffer(hVisible);
    if (decl.m_bHiZ)
    {
      pass.ReadTexture(ref_graph.ImportTexture(decl.m_hHiZTexture), {}, plGALResourceState::ComputeShaderRead);
      pass.ReadBuffer(ref_graph.ImportBuffer(decl.m_hHiZValidity), plGALResourceState::ComputeShaderRead);
    }
    pass.SetExecuteCallback([this](const plRenderGraphContext& ctx) { ExecuteCull(ctx); });
  }

  {
    // No work: the declared reads leave the arguments, lists and pool in the states every later draw pass needs.
    auto pass = ref_graph.AddGraphicsPass("Grass Draw Inputs");
    pass.HasSideEffects();
    pass.ReadBuffer(hArgs, plGALResourceState::IndirectArgument);
    // DX12 exposes the draw group's descriptor table to all graphics stages when its per-instance data is also
    // used by the pixel shader. Its grass SRVs must consequently include pixel-read state even though only the
    // vertex shader pulls their contents. Vertex-only transitions fail validation on the lit material path.
    pass.ReadBuffer(hVisible, plGALResourceState::ShaderRead);
    pass.ReadBuffer(hStatic, plGALResourceState::ShaderRead);
    pass.ReadBuffer(hState, plGALResourceState::ShaderRead);
    pass.ReadBuffer(hPages, plGALResourceState::ShaderRead);
    pass.ReadBuffer(hSources, plGALResourceState::ShaderRead);
    pass.ReadBuffer(hAtlas, plGALResourceState::ShaderRead);
    pass.SetExecuteCallback([](const plRenderGraphContext&) {});
  }

  return plStatus(PL_SUCCESS);
}

plStatus plGrassCullPass::AddRenderPassesInactive(const plViewData& viewData, const plCamera& camera, plRenderGraph& ref_graph, const plArrayPtr<const plRenderPipelinePinConnection> inputs, plArrayPtr<plRenderPipelinePinConnection> outputs)
{
  PL_IGNORE_UNUSED(viewData);
  PL_IGNORE_UNUSED(camera);
  PL_IGNORE_UNUSED(ref_graph);
  PL_IGNORE_UNUSED(inputs);
  PL_IGNORE_UNUSED(outputs);

  m_Declared = Declaration();
  m_DrawInputs = DrawInputs();
  return plStatus(PL_SUCCESS);
}

plResult plGrassCullPass::OnRenderGraphCacheHit(plRenderGraph& ref_graph)
{
  PL_IGNORE_UNUSED(ref_graph);

  // Any change of the imported handles, of Hi-Z use, or grass appearing in a graph built without it needs a rebuild.
  const Declaration decl = Prepare(false);
  if (!(decl == m_Declared))
  {
    m_DrawInputs = DrawInputs();
    return PL_FAILURE;
  }

  return PL_SUCCESS;
}

void plGrassCullPass::ExecuteCull(const plRenderGraphContext& ctx)
{
  if (!m_DrawInputs.m_bValid)
    return;

  const DrawInputs& inputs = m_DrawInputs;
  const plGrassRenderSnapshot& snapshot = inputs.m_Snapshot;
  const FrameState& frame = m_Frame;

  // No page passed the prefilter: the reset left every bucket at zero vertices.
  if (frame.m_Pages.IsEmpty())
    return;

  plRenderContext* pContext = ctx.GetRenderContext();
  PL_PROFILE_AND_MARKER(ctx.GetCommandEncoder(), "Grass Cull");

  plGrassCullConstants* pConstants = plRenderContext::GetConstantBufferData<plGrassCullConstants>(m_hConstants);
  pConstants->CullFrustumPlane0 = frame.m_Planes[0];
  pConstants->CullFrustumPlane1 = frame.m_Planes[1];
  pConstants->CullFrustumPlane2 = frame.m_Planes[2];
  pConstants->CullFrustumPlane3 = frame.m_Planes[3];
  pConstants->CullFrustumPlane4 = frame.m_Planes[4];
  pConstants->CullFrustumPlane5 = frame.m_Planes[5];
  pConstants->CullHiZViewProjection = frame.m_HiZViewProjection;
  pConstants->CullCameraPosition = frame.m_vCameraPosition;
  pConstants->CullPlaneCount = frame.m_uiPlaneCount;
  pConstants->CullMainCameraPosition = snapshot.m_vMainCamera;
  pConstants->CullFlags = (frame.m_bShadow ? GRASS_CULL_FLAG_SHADOW : 0u) | (m_Declared.m_bHiZ && frame.m_bOcclusion ? GRASS_CULL_FLAG_OCCLUSION : 0u);
  pConstants->CullHiZSize = plVec2U32(frame.m_uiHiZWidth, frame.m_uiHiZHeight);
  pConstants->CullHiZMipCount = plMath::Max(frame.m_uiHiZMipCount, 1u);
  pConstants->CullResidentPageCount = frame.m_Pages.GetCount();
  pConstants->CullPageCount = snapshot.m_uiPageCount;
  pConstants->CullSourceCount = snapshot.m_uiSourceCount;
  pConstants->CullVisibleEntries = inputs.m_uiVisibleEntries;
  pConstants->CullArgsRecords = inputs.m_uiArgsRecords;
  pConstants->CullPoolStrands = snapshot.m_uiPoolStrands;
  pConstants->CullInstanceCount = frame.m_uiInstanceCount;
  pConstants->CullPadding0 = 0;
  pConstants->CullPadding1 = 0;

  // The CONSTANT_BUFFER macro declares the constants in the frame group.
  pContext->GetBindGroup().BindBuffer("plGrassCullConstants", m_hConstants);

  plBindGroupBuilder& bindGroup = pContext->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
  bindGroup.BindBuffer("GrassStatic", m_Declared.m_hStatic);
  bindGroup.BindBuffer("GrassSources", m_Declared.m_hSources);
  bindGroup.BindBuffer("GrassPages", m_Declared.m_hPages);
  bindGroup.BindBuffer("GrassViewPages", m_Declared.m_hViewPages);
  bindGroup.BindBuffer("GrassArgs", m_Declared.m_hArgs);
  bindGroup.BindBuffer("GrassVisible", m_Declared.m_hVisible);

  if (m_Declared.m_bHiZ)
  {
    bindGroup.BindTexture("HiZTexture", m_Declared.m_hHiZTexture);
    bindGroup.BindBuffer("HiZValidity", m_Declared.m_hHiZValidity);
  }
  else
  {
    // The shader always declares the Hi-Z slots; occlusion is off, so any texture satisfies the binding.
    bindGroup.BindTexture("HiZTexture", m_hFallbackTexture, plResourceAcquireMode::BlockTillLoaded);
    bindGroup.BindBuffer("HiZValidity", m_hZeroValidity);
  }

  pContext->BindShader(m_hCullShader);
  pContext->Dispatch(GRASS_CULL_GROUPS_PER_PAGE, frame.m_Pages.GetCount(), 1).IgnoreResult();
}

PL_STATICLINK_FILE(VegetationPlugin, VegetationPlugin_Grass_GrassCullPass);
