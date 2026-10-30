#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <Foundation/Reflection/ReflectionUtils.h>
#include <Foundation/Memory/FrameAllocator.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/MeshComponent.h>
#include <RendererCore/Pipeline/Extractor.h>
#include <RendererCore/Pipeline/Passes/SimpleRenderPass.h>
#include <RendererCore/Pipeline/Passes/SourcePass.h>
#include <RendererCore/Pipeline/Passes/TargetPass.h>
#include <RendererCore/Pipeline/RenderPipeline.h>
#include <RendererCore/Pipeline/RenderPipelineResource.h>
#include <RendererCore/Pipeline/Implementation/RenderPipelineResourceLoader.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/ShaderCompiler/ShaderManager.h>
#include <RendererCore/Textures/Texture2DResource.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Device/DeviceFactory.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>
#include <TestFramework/Framework/TestFramework.h>

void TestGrassMaterial(plGALDevice* pDevice)
{
  plTestFramework::Output(plTestOutput::Message, "Offscreen material textures and alpha masking");
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(">sdk/Workspace", "GrassTests", "shadercache", plDataDirUsage::AllowWrites).Succeeded());
  const char* szShaderModel = nullptr;
  const char* szShaderCompiler = nullptr;
  plGALDeviceFactory::GetShaderModelAndCompiler("DX12", szShaderModel, szShaderCompiler);
  PL_TEST_BOOL(szShaderModel != nullptr && szShaderCompiler != nullptr);
  plShaderManager::Configure(szShaderModel, true);
  PL_TEST_BOOL(plPlugin::LoadPlugin(szShaderCompiler).Succeeded());
  auto* pMultithreading = static_cast<plCVarBool*>(plCVar::FindCVarByName("Rendering.Multithreading"));
  const bool bMultithreading = *pMultithreading;
  *pMultithreading = false;

  plWorldDesc worldDesc("Grass material rendering");
  plWorld world(worldDesc);
  plCamera camera;
  camera.SetCameraMode(plCameraMode::OrthoFixedHeight, 2.5f, 0.1f, 10);
  camera.LookAt(plVec3(0, 0, 3), plVec3::MakeZero(), plVec3(0, 1, 0));

  plDynamicArray<plUniquePtr<plRenderPipelinePass>> passes;
  plUniquePtr<plSourcePass> source = PL_DEFAULT_NEW(plSourcePass);
  auto* pSource = source.Borrow();
  plReflectionUtils::SetMemberPropertyValue(static_cast<const plAbstractMemberProperty*>(pSource->GetDynamicRTTI()->FindPropertyByName("Clear")), pSource, true);
  plUniquePtr<plSimpleRenderPass> simple = PL_DEFAULT_NEW(plSimpleRenderPass);
  plUniquePtr<plTargetPass> target = PL_DEFAULT_NEW(plTargetPass);
  passes.PushBack(std::move(source));
  passes.PushBack(std::move(simple));
  passes.PushBack(std::move(target));
  plDynamicArray<plUniquePtr<plExtractor>> extractors;
  extractors.PushBack(PL_DEFAULT_NEW(plVisibleObjectsExtractor));
  plDynamicArray<const plRenderPipelinePass*> passPointers;
  for (const auto& pPass : passes)
    passPointers.PushBack(pPass.Borrow());
  plDynamicArray<const plExtractor*> extractorPointers;
  extractorPointers.PushBack(extractors[0].Borrow());
  plDynamicArray<plRenderPipelineResourceLoaderConnection> connections;
  connections.PushBack({0, 1, "Output", "Color"});
  connections.PushBack({1, 2, "Color", "Color0"});
  plRenderPipelineResourceDescriptor pipelineDesc;
  plMemoryStreamContainerWrapperStorage<plDynamicArray<plUInt8>> pipelineStorage(&pipelineDesc.m_SerializedPipeline);
  plMemoryStreamWriter pipelineWriter(&pipelineStorage);
  PL_TEST_RESULT(plRenderPipelineResourceLoader::ExportPipeline(passPointers, extractorPointers, connections, pipelineWriter));
  const auto hPipeline = plResourceManager::CreateResource<plRenderPipelineResource>("GrassMaterialPipeline", std::move(pipelineDesc));

  plGALTextureCreationDescription targetDesc;
  targetDesc.m_uiWidth = targetDesc.m_uiHeight = 64;
  targetDesc.m_Format = plGALResourceFormat::RGBAUByteNormalizedsRGB;
  targetDesc.m_TextureFlags = plGALTextureUsageFlags::RenderTarget | plGALTextureUsageFlags::ShaderResource;
  auto hTarget = pDevice->CreateTexture(targetDesc);
  plGALRenderTargets targets;
  targets.m_hRTs[0] = hTarget;
  plView* pView = nullptr;
  const auto hView = plRenderWorld::CreateView("Grass material", pView);
  pView->SetWorld(&world);
  pView->SetCamera(&camera);
  pView->SetViewport(plRectFloat(0, 0, 64, 64));
  pView->SetRenderTargets(targets);
  pView->SetRenderPipelineResource(hPipeline);
  pView->SetShaderPermutationVariable("FORWARD_PASS_WRITE_DEPTH", "TRUE");
  plRenderWorld::AddMainView(hView);

  const plUInt8 pixels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 0, 255, 255, 0, 255};
  plGALSystemMemoryDescription memory;
  memory.m_pData = plConstByteBlobPtr(pixels, sizeof(pixels));
  memory.m_uiRowPitch = 8;
  memory.m_uiSlicePitch = sizeof(pixels);
  plTexture2DResourceDescriptor textureDesc;
  textureDesc.m_DescGAL.m_uiWidth = textureDesc.m_DescGAL.m_uiHeight = 2;
  textureDesc.m_DescGAL.m_Format = plGALResourceFormat::RGBAUByteNormalizedsRGB;
  textureDesc.m_InitialContent = plMakeArrayPtr(&memory, 1);
  textureDesc.m_SamplerDesc.m_MinFilter = plGALTextureFilterMode::Point;
  textureDesc.m_SamplerDesc.m_MagFilter = plGALTextureFilterMode::Point;
  const auto hTexture = plResourceManager::CreateResource<plTexture2DResource>("GrassChecker", std::move(textureDesc));

  plMeshResourceDescriptor meshDesc;
  auto& buffer = meshDesc.MeshBufferDesc();
  buffer.AddCommonStreams();
  buffer.AllocateStreams(4, plGALPrimitiveTopology::Triangles, 2);
  for (plUInt32 i = 0; i < 4; ++i)
  {
    const plVec2 uv(float(i & 1), float(i >> 1));
    buffer.SetPosition(i, plVec3(uv.x * 2 - 1, uv.y * 2 - 1, 0));
    buffer.SetNormal(i, plVec3(0, 0, 1));
    buffer.SetTangent(i, plVec4(1, 0, 0, 1));
    buffer.SetTexCoord0(i, uv);
  }
  buffer.SetTriangleIndices(0, 0, 1, 2);
  buffer.SetTriangleIndices(1, 2, 1, 3);
  meshDesc.AddSubMesh(2, 0, 0);
  meshDesc.ComputeBounds();
  const auto hMesh = plResourceManager::CreateResource<plMeshResource>("GrassMaterialQuad", std::move(meshDesc));
  plMeshComponent* pMesh = nullptr;
  {
    PL_LOCK(world.GetWriteMarker());
    plGameObject* pObject = nullptr;
    world.CreateObject(plGameObjectDesc(), pObject);
    world.GetOrCreateComponentManager<plMeshComponentManager>()->CreateComponent(pObject, pMesh);
    pMesh->SetMesh(hMesh);
  }

  plDynamicArray<plUInt8> reference[2];
  for (plUInt32 shader = 0; shader < 2; ++shader)
  {
    for (plUInt32 masked = 0; masked < 2; ++masked)
    {
      plMaterialResourceDescriptor material;
      material.m_hShader = plResourceManager::LoadResource<plShaderResource>(shader == 0 ? "Shaders/Materials/DefaultMaterial.plShader" : "Shaders/Vegetation/Grass/Grass.plShader");
      material.m_RenderDataCategory = plDefaultRenderDataCategories::SimpleOpaque;
      auto Permutation = [&](const char* szName, const char* szValue)
      {
        auto& value = material.m_PermutationVars.ExpandAndGetRef();
        value.m_sName.Assign(szName);
        value.m_sValue.Assign(szValue);
      };
      Permutation("BLEND_MODE", masked ? "BLEND_MODE_MASKED" : "BLEND_MODE_OPAQUE");
      Permutation("SHADING_MODE", "SHADING_MODE_FULLBRIGHT");
      Permutation("TWO_SIDED", "TRUE");
      material.m_Parameters.PushBack({plMakeHashedString("BaseColor"), plColor::White});
      material.m_Parameters.PushBack({plMakeHashedString("UseBaseTexture"), true});
      material.m_Parameters.PushBack({plMakeHashedString("MaskThreshold"), 0.5f});
      material.m_Texture2DBindings.PushBack({plMakeHashedString("BaseTexture"), hTexture});
      plStringBuilder name;
      name.SetFormat("GrassMaterial_{}_{}", shader, masked);
      const auto hMaterial = plResourceManager::CreateResource<plMaterialResource>(name, std::move(material));
      {
        PL_LOCK(world.GetWriteMarker());
        pMesh->SetMaterial(0, hMaterial);
      }
      for (plUInt32 frame = 0; frame < 6; ++frame)
      {
        pDevice->BeginFrame();
        {
          PL_LOCK(world.GetWriteMarker());
          world.Update();
        }
        plRenderWorld::ExtractMainViews();
        plRenderWorld::BeginFrame();
        plRenderWorld::Render(plRenderContext::GetDefaultInstance());
        plRenderWorld::EndFrame();
        pDevice->EndFrame();
        plFrameAllocator::Swap();
      }

      plGALReadbackTextureHelper readback;
      auto* pEncoder = pDevice->BeginCommands("Grass material readback");
      readback.ReadbackTexture(*pEncoder, hTarget);
      pEncoder->Flush();
      pDevice->EndCommands(pEncoder);
      PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(10)) == plGALAsyncResult::Ready);
      plGALTextureSubresource subresource;
      plDynamicArray<plGALSystemMemoryDescription> result;
      auto lock = readback.LockTexture(plMakeArrayPtr(&subresource, 1), result);
      plDynamicArray<plUInt8> image;
      plUInt32 uiColored = 0;
      for (plUInt32 y = 0; y < 64; ++y)
      {
        const auto* pRow = result[0].m_pData.GetPtr() + y * result[0].m_uiRowPitch;
        for (plUInt32 x = 0; x < 64; ++x)
        {
          uiColored += pRow[x * 4] > 50 || pRow[x * 4 + 1] > 50 || pRow[x * 4 + 2] > 50;
          for (plUInt32 c = 0; c < 3; ++c)
            image.PushBack(pRow[x * 4 + c]);
        }
      }
      plTestFramework::Output(plTestOutput::Message, "%s: %u colored pixels", name.GetData(), uiColored);
      plTestFramework::Output(plTestOutput::Message, "Samples: %u %u %u / %u %u %u", image[(16 * 64 + 16) * 3], image[(16 * 64 + 16) * 3 + 1], image[(16 * 64 + 16) * 3 + 2], image[(48 * 64 + 16) * 3], image[(48 * 64 + 16) * 3 + 1], image[(48 * 64 + 16) * 3 + 2]);
      PL_TEST_BOOL(uiColored > 1000);
      if (shader == 0)
        reference[masked] = image;
      else
        PL_TEST_BOOL(image == reference[masked]);
    }
  }
  PL_TEST_BOOL(reference[0] != reference[1]);
  plRenderWorld::RemoveMainView(hView);
  plRenderWorld::DeleteView(hView);
  for (plUInt32 frame = 0; frame < 2; ++frame)
  {
    pDevice->BeginFrame();
    plRenderWorld::ExtractMainViews();
    plRenderWorld::BeginFrame();
    plRenderWorld::Render(plRenderContext::GetDefaultInstance());
    plRenderWorld::EndFrame();
    pDevice->EndFrame();
    plFrameAllocator::Swap();
  }
  pDevice->DestroyTexture(hTarget);
  *pMultithreading = bMultithreading;
}
