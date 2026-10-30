#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Memory/FrameAllocator.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <RendererCore/Lights/ClusteredDataExtractor.h>
#include <RendererCore/Lights/DirectionalLightComponent.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Pipeline/Extractor.h>
#include <RendererCore/Pipeline/Implementation/RenderPipelineResourceLoader.h>
#include <RendererCore/Pipeline/Passes/DepthOnlyPass.h>
#include <RendererCore/Pipeline/Passes/AOPass.h>
#include <RendererCore/Pipeline/Passes/OpaqueForwardRenderPass.h>
#include <RendererCore/Pipeline/Passes/SourcePass.h>
#include <RendererCore/Pipeline/Passes/TargetPass.h>
#include <RendererCore/Pipeline/RenderPipelineResource.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>
#include <Shaders/Vegetation/Grass/GrassGpuData.h>
#include <TestFramework/Framework/TestFramework.h>
#include <Texture/Image/Image.h>
#include <VegetationPlugin/Components/VegetationLayerComponent.h>
#include <VegetationPlugin/Grass/GrassCullPass.h>
#include <VegetationPlugin/Grass/GrassContactAOPass.h>
#include <VegetationPlugin/Grass/GrassSurfaceComponent.h>

void TestGrassStreaming(bool adaptive)
{
  auto* budget = static_cast<plCVarInt*>(plCVar::FindCVarByName("Vegetation.Grass.MaxStrands"));
  const plInt32 oldBudget = *budget;
  *budget = 4096;
  {
    plWorldDesc desc("Grass streaming pressure test");
    plWorld world(desc);
    plCamera camera;
    camera.LookAt(plVec3(4, 4, 2), plVec3(4, 5, 2), plVec3(0, 0, 1));
    plView* view = nullptr;
    const auto viewHandle = plRenderWorld::CreateView("Grass streaming camera", view);
    view->SetWorld(&world);
    view->SetCamera(&camera);
    view->SetCameraUsageHint(plCameraUsageHint::MainView);

    plGrassTypeResourceDescriptor type;
    type.m_Settings.m_fStrandsPerSquareMeter = 64.0f; // Exactly one 4096-strand page per 8 m tile.
    type.m_Settings.m_fSimulationDistance = 0;
    type.m_Settings.m_bCastShadows = false;
    type.m_Settings.m_bProceduralBlades = adaptive;
    type.m_Settings.m_bAdaptiveDensity = adaptive;
    type.m_Settings.m_sMaterial = "GrassUpgradeMaterial";
    const auto typeHandle = plResourceManager::CreateResource<plGrassTypeResource>(adaptive ? "GrassStreamingAdaptiveType" : "GrassStreamingType", std::move(type));
    plGrassCoverageData coverage;
    auto& entry = coverage.GetOrAddEntry(typeHandle.GetResourceID());
    for (plInt16 x : {plInt16(0), plInt16(12)})
    {
      plGrassCoverageTile tile;
      tile.m_iX = x;
      for (auto& texel : tile.m_Texels)
        texel.m_uiDensity = 255;
      entry.m_Tiles.Insert(plGrassCoverageTile::MakeKey(x, 0), tile);
    }
    plDataBuffer blob;
    coverage.ToBuffer(blob);
    {
      PL_LOCK(world.GetWriteMarker());
      plGameObject* owner = nullptr;
      world.CreateObject(plGameObjectDesc(), owner);
      plVegetationLayerComponent* layer = nullptr;
      plVegetationLayerComponent::CreateComponent(owner, layer);
      layer->SetGrassCoverageBlob(blob);
    }
    for (plUInt32 step = 0; step < 3; ++step)
    {
      const float x = step == 1 ? 100.0f : 4.0f;
      camera.LookAt(plVec3(x, 4, 2), plVec3(x, 5, 2), plVec3(0, 0, 1));
      for (plUInt32 frame = 0; frame < 3; ++frame)
      {
        {
          PL_LOCK(world.GetWriteMarker());
          world.Update();
        }
        plRenderWorld::ExtractMainViews();
        plRenderWorld::BeginFrame();
        plRenderWorld::Render(plRenderContext::GetDefaultInstance());
        plRenderWorld::EndFrame();
        plFrameAllocator::Swap();
      }
      plGrassRenderSnapshot snapshot;
      PL_TEST_BOOL(plGrassGpuManager::GetRenderSnapshot(world.GetIndex(), snapshot));
      if (PL_TEST_INT(snapshot.m_ResidentPages.GetCount(), 1))
        PL_TEST_FLOAT(snapshot.m_ResidentPages[0].m_WorldBounds.GetCenter().x, adaptive ? 52.0f : x, 0.01f);
      if (adaptive)
      {
        auto* device = plGALDevice::GetDefaultDevice();
        plGALReadbackBufferHelper readback;
        auto* encoder = device->BeginCommands("Verify grass coverage at both distances");
        readback.ReadbackBuffer(*encoder, snapshot.m_hStatic);
        encoder->Flush();
        device->EndCommands(encoder);
        PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(10)) == plGALAsyncResult::Ready);
        plArrayPtr<const plUInt8> bytes;
        auto lock = readback.LockBuffer(bytes);
        const auto* roots = reinterpret_cast<const plGrassStrandStatic*>(bytes.GetPtr());
        plUInt32 left = 0, right = 0;
        for (plUInt32 i = 0; i < bytes.GetCount() / sizeof(plGrassStrandStatic); ++i)
        {
          if ((roots[i].AtlasFlags & (GRASS_STRAND_FLAG_ALIVE << 8)) == 0)
            continue;
          left += roots[i].Root.x < 8;
          right += roots[i].Root.x >= 96;
        }
        PL_TEST_BOOL(left > 0 && right > 0);
        PL_TEST_BOOL(step == 1 ? right > left : left > right);
      }
    }
    plRenderWorld::DeleteView(viewHandle);
  }
  *budget = oldBudget;
}

void TestGrassRendering(plGALDevice* device)
{
  const bool stress = plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassStress", false);
  const bool contactAO = plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassContactAO", false);
  const bool automaticContactAO = contactAO && plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassContactAuto", false);
  auto* sceneAOIntensity = static_cast<plCVarFloat*>(plCVar::FindCVarByName("Rendering.AO.Intensity"));
  const float oldSceneAOIntensity = *sceneAOIntensity;
  if (automaticContactAO)
    *sceneAOIntensity = 0.0f;
  // Isolate receiver AO from the mesh culler's existing Batch Table barrier mismatch. Grass GPU culling stays on.
  auto* meshGpuCulling = static_cast<plCVarBool*>(plCVar::FindCVarByName("Rendering.Culling.GPU"));
  const bool oldMeshGpuCulling = *meshGpuCulling;
  if (contactAO)
    *meshGpuCulling = false;
  auto* aoMode = static_cast<plCVarInt*>(plCVar::FindCVarByName("Vegetation.Grass.AO.Mode"));
  const plInt32 oldAOMode = *aoMode;
  *aoMode = 2;
  auto* virtualShadows = static_cast<plCVarBool*>(plCVar::FindCVarByName("Rendering.Shadows.UseVirtualShadowMaps"));
  const bool wasVirtualShadows = *virtualShadows;
  *virtualShadows = false;
  auto* multithreading = static_cast<plCVarBool*>(plCVar::FindCVarByName("Rendering.Multithreading"));
  const bool wasMultithreaded = *multithreading;
  *multithreading = true;
  plWorldDesc worldDesc("Procedural grass rendering test");
  plWorld world(worldDesc);
  world.GetClock().SetFixedTimeStep(plTime::MakeFromSeconds(1.0 / 60.0));

  plDynamicArray<plUniquePtr<plRenderPipelinePass>> passes;
  plUniquePtr<plSourcePass> color = PL_DEFAULT_NEW(plSourcePass, "Grass Color");
  plUniquePtr<plSourcePass> depth = PL_DEFAULT_NEW(plSourcePass, "Grass Depth");
  auto Property = [](plReflectedClass* object, const char* name, const plVariant& value)
  {
    plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(object->GetDynamicRTTI()->FindPropertyByName(name)), object, value);
  };
  Property(color.Borrow(), "Clear", true);
  if (contactAO)
  {
    Property(color.Borrow(), "ClearColor", plColor(0.8f, 0.8f, 0.8f, 1.0f));
    Property(color.Borrow(), "Format", static_cast<plInt64>(plSourceFormat::Color4Channel8BitNormalized));
  }
  Property(depth.Borrow(), "Clear", true);
  Property(depth.Borrow(), "Format", static_cast<plInt64>(plSourceFormat::Depth32BitFloat));
  passes.PushBack(std::move(color));
  passes.PushBack(std::move(depth));
  passes.PushBack(PL_DEFAULT_NEW(plOpaqueForwardRenderPass));
  passes.PushBack(PL_DEFAULT_NEW(plTargetPass));
  passes.PushBack(PL_DEFAULT_NEW(plGrassCullPass));
  if (contactAO)
  {
    passes[2] = PL_DEFAULT_NEW(plDepthOnlyPass);
    passes.PushBack(PL_DEFAULT_NEW(plGrassContactAOPass));
    if (automaticContactAO)
    {
      passes[5] = PL_DEFAULT_NEW(plAOPass);
    }
  }
  plDynamicArray<const plRenderPipelinePass*> passPointers;
  for (auto& pass : passes)
    passPointers.PushBack(pass.Borrow());
  plUniquePtr<plVisibleObjectsExtractor> extractor = PL_DEFAULT_NEW(plVisibleObjectsExtractor);
  plUniquePtr<plClusteredDataExtractor> lights = PL_DEFAULT_NEW(plClusteredDataExtractor);
  const plExtractor* extractorPointers[] = {extractor.Borrow(), lights.Borrow()};
  plDynamicArray<plRenderPipelineResourceLoaderConnection> connections;
  connections.PushBack({0, 2, "Output", "Color"});
  connections.PushBack({1, 2, "Output", "DepthStencil"});
  connections.PushBack({2, 3, "Color", "Color0"});
  connections.PushBack({2, 3, "DepthStencil", "DepthStencil"});
  if (contactAO)
  {
    connections.Clear();
    connections.PushBack({1, 2, "Output", "DepthStencil"});
    connections.PushBack({2, 5, "DepthStencil", "Depth"});
    connections.PushBack({0, 5, "Output", "AO"});
    connections.PushBack({5, 3, "AO", "Color0"});
    if (automaticContactAO)
    {
      connections.Clear();
      connections.PushBack({1, 2, "Output", "DepthStencil"});
      connections.PushBack({2, 5, "DepthStencil", "DepthInput"});
      connections.PushBack({5, 3, "Output", "Color0"});
    }
  }
  plRenderPipelineResourceDescriptor pipelineDesc;
  plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> storage(&pipelineDesc.m_SerializedPipeline);
  plMemoryStreamWriter writer(&storage);
  PL_TEST_BOOL(plRenderPipelineResourceLoader::ExportPipeline(passPointers, plMakeArrayPtr(extractorPointers, stress ? 2u : 1u), connections, writer).Succeeded());
  const auto pipeline = plResourceManager::CreateResource<plRenderPipelineResource>("GrassUpgradePipeline", std::move(pipelineDesc));
  plRenderPipelineResourceHandle shadowPipeline;
  if (stress)
  {
    passes[2] = PL_DEFAULT_NEW(plDepthOnlyPass);
    passPointers.Clear();
    for (auto& pass : passes)
      passPointers.PushBack(pass.Borrow());
    connections.RemoveAtAndCopy(2);
    connections.RemoveAtAndCopy(0);
    plRenderPipelineResourceDescriptor shadowDesc;
    plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> shadowStorage(&shadowDesc.m_SerializedPipeline);
    plMemoryStreamWriter shadowWriter(&shadowStorage);
    PL_TEST_BOOL(plRenderPipelineResourceLoader::ExportPipeline(passPointers, plMakeArrayPtr(extractorPointers, 1), connections, shadowWriter).Succeeded());
    shadowPipeline = plResourceManager::CreateResource<plRenderPipelineResource>("GrassStressShadowPipeline", std::move(shadowDesc));
  }

  plGALTextureCreationDescription targetDesc;
  targetDesc.SetAsRenderTarget(512, 512, plGALResourceFormat::RGBAUByteNormalizedsRGB);
  if (contactAO)
    targetDesc.m_Format = plGALResourceFormat::RGBAUByteNormalized;
  auto target = device->CreateTexture(targetDesc);
  plGALRenderTargets targets;
  targets.m_hRTs[0] = target;
  plCamera camera;
  camera.SetCameraMode(plCameraMode::OrthoFixedHeight, 6.0f, 0.1f, 150.0f);
  plView* view = nullptr;
  const auto viewHandle = plRenderWorld::CreateView("Grass upgrade test", view);
  view->SetWorld(&world);
  view->SetCamera(&camera);
  view->SetCameraUsageHint(plCameraUsageHint::MainView);
  view->SetViewport(plRectFloat(0, 0, 512, 512));
  view->SetRenderTargets(targets);
  view->SetRenderPipelineResource(pipeline);
  plRenderWorld::AddMainView(viewHandle);

  plViewHandle secondViewHandle;
  plGALTextureHandle secondTarget;
  if (stress)
  {
    camera.SetCameraMode(plCameraMode::OrthoFixedHeight, 100.0f, 0.1f, 300.0f);
    secondTarget = device->CreateTexture(targetDesc);
    targets.m_hRTs[0] = secondTarget;
    plView* secondView = nullptr;
    secondViewHandle = plRenderWorld::CreateView("Grass stress second view", secondView);
    secondView->SetWorld(&world);
    secondView->SetCamera(&camera);
    secondView->SetCameraUsageHint(plCameraUsageHint::Shadow);
    secondView->SetViewport(plRectFloat(0, 0, 512, 512));
    secondView->SetRenderTargets(targets);
    secondView->SetRenderPipelineResource(shadowPipeline);
    plRenderWorld::AddMainView(secondViewHandle);
  }

  plMaterialResourceDescriptor material;
  material.m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/Grass.plShader");
  material.m_RenderDataCategory = plDefaultRenderDataCategories::LitMasked;
  auto Permutation = [&](const char* name, const char* value)
  {
    auto& var = material.m_PermutationVars.ExpandAndGetRef();
    var.m_sName.Assign(name);
    var.m_sValue.Assign(value);
  };
  Permutation("BLEND_MODE", "BLEND_MODE_MASKED");
  Permutation("SHADING_MODE", stress ? "SHADING_MODE_LIT" : "SHADING_MODE_FULLBRIGHT");
  Permutation("TWO_SIDED", "TRUE");
  material.m_Parameters.PushBack({plMakeHashedString("base_color"), plColor(0.15f, 0.5f, 0.05f)});
  material.m_Parameters.PushBack({plMakeHashedString("base_weight"), 1.0f});
  material.m_Parameters.PushBack({plMakeHashedString("geometry_opacity"), 1.0f});
  material.m_Parameters.PushBack({plMakeHashedString("specular_roughness"), 0.5f});
  material.m_Parameters.PushBack({plMakeHashedString("specular_ior"), 1.5f});
  material.m_Parameters.PushBack({plMakeHashedString("foliage_root_occlusion"), 0.75f});
  material.m_Parameters.PushBack({plMakeHashedString("foliage_root_occlusion_height"), 0.65f});
  material.m_Parameters.PushBack({plMakeHashedString("foliage_root_occlusion_falloff"), 1.5f});
  material.m_Parameters.PushBack({plMakeHashedString("foliage_ground_cover_occlusion"), 0.2f});
  const auto materialHandle = plResourceManager::CreateResource<plMaterialResource>("GrassUpgradeMaterial", std::move(material));
  plMaterialResourceDescriptor receiverMaterial;
  receiverMaterial.m_hShader = plResourceManager::LoadResource<plShaderResource>("Shaders/Materials/DefaultMaterial.plShader");
  receiverMaterial.m_RenderDataCategory = plDefaultRenderDataCategories::LitOpaque;
  receiverMaterial.m_PermutationVars.PushBack({plMakeHashedString("BLEND_MODE"), plMakeHashedString("BLEND_MODE_OPAQUE")});
  receiverMaterial.m_PermutationVars.PushBack({plMakeHashedString("TWO_SIDED"), plMakeHashedString("TRUE")});
  const auto receiverMaterialHandle = plResourceManager::CreateResource<plMaterialResource>("GrassAOReceiver", std::move(receiverMaterial));

  plMeshResourceDescriptor mesh;
  auto& buffer = mesh.MeshBufferDesc();
  buffer.AddCommonStreams();
  buffer.AllocateStreams(4, plGALPrimitiveTopology::Triangles, 2);
  for (plUInt32 i = 0; i < 4; ++i)
  {
    const float size = stress ? 64.0f : 4.0f;
    buffer.SetPosition(i, plVec3(float(i & 1) * size - size * 0.5f, float(i >> 1) * size - size * 0.5f, 0));
    buffer.SetNormal(i, plVec3(0, 0, 1));
    buffer.SetTangent(i, plVec4(1, 0, 0, 1));
    buffer.SetTexCoord0(i, plVec2(float(i & 1), float(i >> 1)));
  }
  buffer.SetTriangleIndices(0, 0, 1, 2);
  buffer.SetTriangleIndices(1, 2, 1, 3);
  mesh.AddSubMesh(2, 0, 0);
  mesh.ComputeBounds();
  auto cpuMesh = mesh;
  const auto cpuHandle = plResourceManager::CreateResource<plCpuMeshResource>("GrassUpgradeSurface", std::move(cpuMesh));
  const auto meshHandle = plResourceManager::CreateResource<plMeshResource>("GrassUpgradeSurface", std::move(mesh));
  plGrassTypeResourceDescriptor type;
  type.m_Settings.m_bProceduralBlades = true;
  type.m_Settings.m_uiSegmentCount = 8;
  type.m_Settings.m_fLength = 0.6f;
  type.m_Settings.m_fBladeWidth = 0.1f;
  type.m_Settings.m_fLodStartDistance = 8.0f;
  type.m_Settings.m_fViewDistance = 100.0f;
  type.m_Settings.m_fSimulationDistance = 15.0f;
  type.m_Settings.m_fGroundCoverStartDistance = 24.0f;
  type.m_Settings.m_sMaterial = materialHandle.GetResourceID();
  type.m_Settings.m_bCastShadows = false;
  if (stress)
  {
    type.m_Settings.m_fStrandsPerSquareMeter = 128.0f;
    type.m_Settings.m_fLodStartDistance = 150.0f;
    type.m_Settings.m_fViewDistance = 250.0f;
    type.m_Settings.m_fShadowDistance = 250.0f;
    type.m_Settings.m_bCastShadows = true;
    type.m_Settings.m_bAdaptiveDensity = false;
    type.m_Settings.m_bFarGroundCover = false;
  }
  const auto typeHandle = plResourceManager::CreateResource<plGrassTypeResource>("GrassUpgradeType", std::move(type));
  plGameObject* object = nullptr;
  plGameObject* receiver = nullptr;
  {
    PL_LOCK(world.GetWriteMarker());
    plGameObjectDesc objectDesc;
    objectDesc.m_bDynamic = true;
    world.CreateObject(objectDesc, object);
    if (contactAO)
    {
      world.CreateObject(objectDesc, receiver);
      receiver->SetLocalScaling(plVec3(2.0f));
      plMeshComponent* meshComponent = nullptr;
      plMeshComponent::CreateComponent(receiver, meshComponent);
      meshComponent->SetMesh(meshHandle);
      meshComponent->SetMaterial(0, receiverMaterialHandle);
    }
    if (!stress && !automaticContactAO)
    {
      plGrassSurfaceComponent* grass = nullptr;
      plGrassSurfaceComponent::CreateComponent(object, grass);
      grass->SetMesh(meshHandle);
      grass->SetGrassType(typeHandle);
    }
    else
    {
      plGrassCoverageData coverage;
      auto& entry = coverage.GetOrAddEntry(typeHandle.GetResourceID());
      for (plInt16 y = stress ? -4 : -1; y < (stress ? 4 : 1); ++y)
        for (plInt16 x = stress ? -4 : -1; x < (stress ? 4 : 1); ++x)
        {
          plGrassCoverageTile tile;
          tile.m_iX = x;
          tile.m_iY = y;
          for (plUInt32 i = 0; i < plGrassCoverageTile::TexelCount; ++i)
          {
            const float rootX = x * 8.0f + (i % 16 + 0.5f) * 0.5f;
            const float rootY = y * 8.0f + (i / 16 + 0.5f) * 0.5f;
            if (stress || (plMath::Abs(rootX) < 2.0f && plMath::Abs(rootY) < 2.0f))
              tile.m_Texels[i].m_uiDensity = 255;
          }
          entry.m_Tiles.Insert(plGrassCoverageTile::MakeKey(x, y), tile);
        }
      plDataBuffer blob;
      coverage.ToBuffer(blob);
      plVegetationLayerComponent* layer = nullptr;
      plVegetationLayerComponent::CreateComponent(object, layer);
      layer->SetGrassCoverageBlob(blob);
    }
    if (stress)
    {
      plGameObject* lightObject = nullptr;
      world.CreateObject(objectDesc, lightObject);
      plDirectionalLightComponent* light = nullptr;
      plDirectionalLightComponent::CreateComponent(lightObject, light);
      light->SetIntensity(10.0f);
      light->SetCastShadows(false);
      lightObject->SetLocalRotation(plQuat::MakeFromAxisAndAngle(plVec3(0, 1, 0), plAngle::MakeFromDegree(45)));
    }
  }

  for (plUInt32 shot = 0; shot < (stress ? 1u : (contactAO ? 7u : 10u)); ++shot)
  {
    const float scale = shot == 0 ? 1.0f : (shot == 1 ? 3.0f : 8.0f);
    camera.LookAt(plVec3(0, -5, 3) * scale, plVec3(0, 0, 0.3f), plVec3(0, 0, 1));
    if (stress)
      camera.LookAt(plVec3(0, -55, 55), plVec3::MakeZero(), plVec3(0, 0, 1));
    if (shot == 3 && !contactAO)
    {
      PL_LOCK(world.GetWriteMarker());
      object->SetLocalRotation(plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree(90)));
      camera.LookAt(plVec3(0, -45, 0), plVec3::MakeZero(), plVec3(0, 0, 1));
    }
    if (shot >= 4 && !contactAO)
    {
      if (shot == 4)
        *aoMode = 1;
      if (shot == 5)
        *aoMode = 2;
      view->SetViewRenderMode(plViewRenderMode::Occlusion);
      PL_LOCK(world.GetWriteMarker());
      object->SetLocalRotation(shot == 6 ? plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree(90)) : plQuat::MakeIdentity());
      camera.LookAt(shot == 5 || shot == 9 ? plVec3(0, -45, 3) : (shot == 6 ? plVec3(0, -45, 0) : plVec3(0, -5, 0.8f)),
        plVec3(0, 0, 0.3f), plVec3(0, 0, 1));
      if (shot == 7)
      {
        plResourceLock<plMaterialResource> aoMaterial(materialHandle, plResourceAcquireMode::BlockTillLoaded);
        aoMaterial->SetParameter("foliage_root_occlusion", 0.0f);
        aoMaterial->SetParameter("foliage_ground_cover_occlusion", 0.0f);
      }
      if (shot == 8)
      {
        plResourceLock<plMaterialResource> aoMaterial(materialHandle, plResourceAcquireMode::BlockTillLoaded);
        aoMaterial->SetParameter("foliage_root_occlusion", 0.75f);
        aoMaterial->SetParameter("foliage_ground_cover_occlusion", 0.2f);
        *aoMode = 0;
      }
    }
    if (contactAO)
    {
      *aoMode = shot == 1 ? 1 : (shot == 2 ? 0 : 2);
      PL_LOCK(world.GetWriteMarker());
      const auto rotation = shot == 3 ? plQuat::MakeFromAxisAndAngle(plVec3(1, 0, 0), plAngle::MakeFromDegree(90)) : plQuat::MakeIdentity();
      object->SetLocalRotation(rotation);
      receiver->SetLocalRotation(rotation);
      receiver->SetLocalPosition(shot == 4 ? plVec3(0, 0, 0.2f) : plVec3::MakeZero());
      camera.LookAt(shot == 3 ? plVec3(0, -6, 0) : plVec3(0, -4, 5), plVec3::MakeZero(), plVec3(0, 0, 1));
      if (shot == 5)
        camera.SetCameraMode(plCameraMode::PerspectiveFixedFovY, 60.0f, 0.1f, 150.0f);
      if (shot == 6)
        view->m_ExcludeTags.Set(plTagRegistry::GetGlobalRegistry().RegisterTag("skipGI"));
      view->ForceUpdate();
    }
    for (plUInt32 frame = 0; frame < (stress ? 30u : 5u); ++frame)
    {
      // A rebuild sorts the already-sorted pipeline again. Culling must still precede every grass draw.
      if (stress && frame >= 10 && frame % 5 == 0)
      {
        view->SetViewport(plRectFloat(0, 0, frame % 10 == 0 ? 480.0f : 512.0f, 512));
        view->ForceUpdate();
      }
      {
        PL_LOCK(world.GetWriteMarker());
        world.Update();
      }
      plRenderWorld::ExtractMainViews();
      plRenderWorld::BeginFrame();
      plRenderWorld::Render(plRenderContext::GetDefaultInstance());
      plRenderWorld::EndFrame();
      plFrameAllocator::Swap();
    }
    plGrassRenderSnapshot snapshot;
    {
      PL_LOCK(world.GetReadMarker());
      PL_TEST_BOOL(!object->HasTag(plTempHashedString("skipGI")));
      bool foundGrassChild = false;
      for (auto child = object->GetChildren(); child.IsValid(); ++child)
      {
        if (child->GetName() == "Grass Render")
        {
          foundGrassChild = true;
          PL_TEST_BOOL(child->HasTag(plTempHashedString("skipGI")));
        }
      }
      if (!stress && !automaticContactAO)
        PL_TEST_BOOL(foundGrassChild);
    }
    PL_TEST_BOOL(plGrassGpuManager::GetRenderSnapshot(world.GetIndex(), snapshot));
    if (stress)
      PL_TEST_BOOL(snapshot.m_ResidentPages.GetCount() >= 100);
    if (shot == 0 && !stress)
      PL_TEST_BOOL(snapshot.m_uiStateSegments > 0);
    if (shot == 2 && !contactAO)
      PL_TEST_INT(snapshot.m_uiStateSegments, 0);

    plGALReadbackTextureHelper readback;
    auto* encoder = device->BeginCommands("Grass screenshot");
    readback.ReadbackTexture(*encoder, target);
    encoder->Flush();
    device->EndCommands(encoder);
    PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(10)) == plGALAsyncResult::Ready);
    plGALTextureSubresource subresource;
    plDynamicArray<plGALSystemMemoryDescription> result;
    auto lock = readback.LockTexture(plMakeArrayPtr(&subresource, 1), result);
    if (!PL_TEST_BOOL(!result.IsEmpty()))
      continue;
    plImageHeader header;
    header.SetWidth(512);
    header.SetHeight(512);
    header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM_SRGB);
    plImage image;
    image.ResetAndAlloc(header);
    plUInt32 colored = 0;
    plUInt32 darkRoots = 0;
    plUInt32 unoccluded = 0;
    plUInt32 coverAO = 0;
    plUInt32 groundDarkened = 0;
    plUInt32 auxiliaryPreserved = 0;
    for (plUInt32 y = 0; y < 512; ++y)
    {
      auto* out = image.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
      const auto* in = result[0].m_pData.GetPtr() + y * result[0].m_uiRowPitch;
      plMemoryUtils::Copy(out, in, 512 * 4);
      for (plUInt32 x = 0; x < 512; ++x)
      {
        colored += in[x * 4 + 1] > 30;
        darkRoots += in[x * 4 + 1] > 30 && in[x * 4 + 1] < 140;
        unoccluded += in[x * 4 + 1] >= 253;
        coverAO += in[x * 4 + 1] >= 201 && in[x * 4 + 1] <= 207;
        groundDarkened += in[x * 4] < (automaticContactAO ? 240 : 190);
        auxiliaryPreserved += in[x * 4 + 1] >= 203 && in[x * 4 + 1] <= 205;
      }
    }
    PL_TEST_BOOL_MSG(colored > 1000, "Grass shot %u has only %u visible pixels", shot, colored);
    if (shot == 4 && !contactAO)
    {
      PL_TEST_BOOL_MSG(darkRoots > 100, "Root AO must remain visible at grazing angles (%u dark pixels)", darkRoots);
      PL_TEST_BOOL_MSG(unoccluded > 100, "Blade tips must remain unoccluded (%u white pixels)", unoccluded);
    }
    if ((shot == 5 || shot == 6) && !contactAO)
      PL_TEST_BOOL_MSG(coverAO > colored * 0.95f, "Distant AO must be uniform on ground and walls (%u / %u pixels)", coverAO, colored);
    if ((shot == 7 || shot == 8 || shot == 9) && !contactAO)
      PL_TEST_BOOL_MSG(unoccluded > colored * 0.99f, "Disabling procedural AO must restore white (%u / %u pixels)", unoccluded, colored);
    if (contactAO)
    {
      if (!automaticContactAO)
        PL_TEST_INT(auxiliaryPreserved, 512 * 512);
      if (shot == 0 || shot == 3 || shot == 5)
        PL_TEST_BOOL_MSG(groundDarkened > 500, "Ground/wall contact AO missing in shot %u (%u pixels)", shot, groundDarkened);
      else
        PL_TEST_INT(groundDarkened, 0);
    }
    plStringBuilder file;
    file.SetFormat(":shadercache/grass-{}-{}.png", contactAO ? (automaticContactAO ? "contact-auto" : "contact") : "shot", shot);
    PL_TEST_BOOL(image.SaveTo(file).Succeeded());
  }
  plRenderWorld::RemoveMainView(viewHandle);
  plRenderWorld::DeleteView(viewHandle);
  if (stress)
  {
    plRenderWorld::RemoveMainView(secondViewHandle);
    plRenderWorld::DeleteView(secondViewHandle);
    device->DestroyTexture(secondTarget);
  }
  device->DestroyTexture(target);
  *multithreading = wasMultithreaded;
  *virtualShadows = wasVirtualShadows;
  *aoMode = oldAOMode;
  *meshGpuCulling = oldMeshGpuCulling;
  *sceneAOIntensity = oldSceneAOIntensity;
}
