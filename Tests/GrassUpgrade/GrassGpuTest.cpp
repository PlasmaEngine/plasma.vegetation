#include <Foundation/Basics.h>
#include <Foundation/Configuration/Plugin.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/Memory/FrameAllocator.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/ShaderCompiler/ShaderManager.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Device/DeviceFactory.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>
#include <TestFramework/Framework/TestFramework.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>

#include <Shaders/Vegetation/Grass/GrassGpuData.h>

void TestGrassRendering(plGALDevice* device);
void TestGrassStreaming(bool adaptive);
void TestGrassAOReplay(plGALDevice* device, const char* folder);

PL_CREATE_SIMPLE_TEST(GrassUpgrade, GpuIndexedArguments)
{
  plStartup::StartupCoreSystems();
  plFileSystem::SetSdkRootDirectory(PL_STRINGIZE(GRASS_TEST_SDK));
  plPlugin::AddPluginSearchRoot(PL_STRINGIZE(GRASS_TEST_BIN));
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(">sdk/Data/Base", "GrassGpuTest", "base").Succeeded());
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(PL_STRINGIZE(GRASS_TEST_PACKAGE) "/Assets", "GrassGpuTest", "grass").Succeeded());
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(">sdk/tmp/grass-shadercheck", "GrassGpuTest", "shadercache", plDataDirUsage::AllowWrites).Succeeded());
  plShaderManager::Configure("DX12", true);
  PL_TEST_BOOL(plPlugin::LoadPlugin("plShaderCompilerDX12").Succeeded());

  plGALDeviceCreationDescription deviceDesc;
  deviceDesc.m_bDebugDevice = plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassGpuValidation", false);
  deviceDesc.m_bAllowAsyncCompute = plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassStress", false);
  plGALDevice* device = plGALDeviceFactory::CreateDevice("DX12", plFoundation::GetDefaultAllocator(), deviceDesc);
  if (!PL_TEST_BOOL(device != nullptr && device->Init().Succeeded()))
    return;
  plGALDevice::SetDefaultDevice(device);
  plStartup::StartupHighLevelSystems();
  {
    auto* context = plRenderContext::GetDefaultInstance();
    context->SetAllowAsyncShaderLoading(false);
    const auto shader = plResourceManager::LoadResource<plShaderResource>("Shaders/Vegetation/Grass/GrassCull.plShader");
    auto constantsHandle = plRenderContext::CreateConstantBufferStorage<plGrassCullConstants>();
    auto* constants = plRenderContext::GetConstantBufferData<plGrassCullConstants>(constantsHandle);
    plMemoryUtils::ZeroFill(constants, 1);
    constants->CullResidentPageCount = 1;
    constants->CullPageCount = 1;
    constants->CullSourceCount = 1;
    constants->CullVisibleEntries = 2048;
    constants->CullArgsRecords = 3;
    constants->CullPoolStrands = 1024;

    plDynamicArray<plGALBufferHandle> buffers;
    auto Buffer = [&](plArrayPtr<const plUInt8> data, plUInt32 stride, bool raw, bool writable)
    {
      plGALBufferCreationDescription desc;
      desc.m_uiTotalSize = data.GetCount();
      desc.m_uiStructSize = raw ? 0 : stride;
      desc.m_BufferFlags = plGALBufferUsageFlags::ShaderResource;
      desc.m_BufferFlags |= raw ? plGALBufferUsageFlags::ByteAddressBuffer : plGALBufferUsageFlags::StructuredBuffer;
      if (writable)
        desc.m_BufferFlags |= plGALBufferUsageFlags::UnorderedAccess;
      auto handle = device->CreateBuffer(desc, data);
      buffers.PushBack(handle);
      return handle;
    };

    plDynamicArray<plGrassStrandStatic> strands;
    strands.SetCount(1024);
    for (plUInt32 i = 0; i < strands.GetCount(); ++i)
      plGrassGpuManager::MakeStrandStatic(plVec3::MakeZero(), plVec3(0, 0, 1), 1.0f, 0.0f, 0, i, strands[i]);
    auto statics = Buffer(strands.GetByteArrayPtr(), sizeof(plGrassStrandStatic), false, false);
    plGrassSourceGpu source;
    plMemoryUtils::ZeroFill(&source, 1);
    source.ObjectToWorld = plTransform::MakeIdentity();
    source.Counts = plVec4U32(4, 1, 0, 0);
    source.Layout = plVec4U32(0, 1024, GRASS_SOURCE_FLAG_BLADES, 1);
    source.Distances = plVec4(200, 40, 15, 25);
    source.Shape = plVec4(1, 1, 1, 0);
    source.Blade = plVec4(0.06f, 0, 0, 0);
    auto sources = Buffer(plMakeArrayPtr(&source, 1).ToByteArray(), sizeof(source), false, false);
    plGrassPageGpu page;
    plMemoryUtils::ZeroFill(&page, 1);
    page.StrandCount = 1024;
    page.Flags = 255u << GRASS_PAGE_DENSITY_SHIFT;
    auto pages = Buffer(plMakeArrayPtr(&page, 1).ToByteArray(), sizeof(page), false, false);
    const plUInt32 zero = 0;
    auto viewPages = Buffer(plMakeArrayPtr(&zero, 1).ToByteArray(), sizeof(zero), false, false);
    plDynamicArray<plUInt32> empty;
    empty.SetCount(2048);
    plMemoryUtils::ZeroFill(empty.GetData(), empty.GetCount());
    auto visible = Buffer(empty.GetByteArrayPtr(), 4, false, true);
    plGALTextureCreationDescription textureDesc;
    textureDesc.m_uiWidth = textureDesc.m_uiHeight = 1;
    textureDesc.m_Format = plGALResourceFormat::RFloat;
    textureDesc.m_ResourceAccess.m_bImmutable = false;
    auto texture = device->CreateTexture(textureDesc);

    for (plUInt32 stereo : {1u, 2u})
    {
      for (plUInt32 lod = 0; lod < 3; ++lod)
      {
        auto args = Buffer(empty.GetArrayPtr().GetSubArray(0, 15).ToByteArray(), 0, true, true);
        constants = plRenderContext::GetConstantBufferData<plGrassCullConstants>(constantsHandle);
        constants->CullInstanceCount = stereo;
        constants->CullCameraPosition = plVec3(lod == 0 ? 0.0f : (lod == 1 ? 20.0f : 50.0f), 0, 0);
        device->BeginFrame();
        auto* encoder = device->BeginCommands("Grass indirect argument test");
        context->BeginCompute();
        context->GetBindGroup().BindBuffer("plGrassCullConstants", constantsHandle);
        auto& group = context->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
        group.BindBuffer("GrassStatic", statics);
        group.BindBuffer("GrassSources", sources);
        group.BindBuffer("GrassPages", pages);
        group.BindBuffer("GrassViewPages", viewPages);
        group.BindBuffer("GrassArgs", args);
        group.BindBuffer("GrassVisible", visible);
        group.BindBuffer("HiZValidity", viewPages);
        group.BindTexture("HiZTexture", texture);
        context->BindShader(shader);
        PL_TEST_BOOL(context->Dispatch(GRASS_CULL_GROUPS_PER_PAGE, 1, 1).Succeeded());
        context->EndCompute();
        plGALReadbackBufferHelper readback;
        readback.ReadbackBuffer(*encoder, args);
        encoder->Flush();
        device->EndCommands(encoder);
        device->EndFrame();
        PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(10)) == plGALAsyncResult::Ready);
        plArrayPtr<const plUInt8> data;
        auto lock = readback.LockBuffer(data);
        if (PL_TEST_BOOL(data.GetCount() == GRASS_ARGS_STRIDE * 3))
        {
          const auto* records = reinterpret_cast<const plUInt32*>(data.GetPtr());
          const plUInt32 segments = 4u >> lod;
          PL_TEST_INT(records[lod * 5], segments * 6 - 3);
          PL_TEST_INT(records[lod * 5 + 1], 1024 * stereo);
          PL_TEST_INT(records[lod * 5 + 2], segments * (segments - 1) / 2 * 6 * GRASS_MAX_BILLBOARDS);
          PL_TEST_INT(records[lod * 5 + 3], 0);
          PL_TEST_INT(records[lod * 5 + 4], 0);
          for (plUInt32 other = 0; other < 3; ++other)
            if (other != lod)
              PL_TEST_INT(records[other * 5 + 1], 0);
        }
      }
    }
    for (auto handle : buffers)
      device->DestroyBuffer(handle);
    device->DestroyTexture(texture);
    plRenderContext::DeleteConstantBufferStorage(constantsHandle);
  }
  const plString replay = plCommandLineUtils::GetGlobalInstance()->GetStringOption("-grassAOReplay", 0, "");
  if (!replay.IsEmpty())
    TestGrassAOReplay(device, replay.GetData());
  else
    TestGrassRendering(device);
  if (replay.IsEmpty() && !plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassStress", false))
  {
    TestGrassStreaming(false);
    TestGrassStreaming(true);
  }
  plRenderWorld::DeleteAllCachedRenderData();
  for (plUInt32 frame = 0; frame < 3; ++frame)
  {
    plRenderWorld::ExtractMainViews();
    plRenderWorld::BeginFrame();
    plRenderWorld::Render(plRenderContext::GetDefaultInstance());
    plRenderWorld::EndFrame();
    plFrameAllocator::Swap();
  }
  plRenderContext::GetDefaultInstance()->ResetContextState();
  plStartup::ShutdownHighLevelSystems();
  plResourceManager::FreeAllUnusedResources();
  device->Shutdown().IgnoreResult();
  PL_DEFAULT_DELETE(device);
  plFileSystem::RemoveDataDirectoryGroup("GrassGpuTest");
  plPlugin::ClearPluginSearchRoots();
  plStartup::ShutdownCoreSystems();
}
