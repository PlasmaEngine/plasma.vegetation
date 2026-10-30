#include <Foundation/IO/OSFile.h>
#include <Foundation/Math/Float16.h>
#include <Foundation/Utilities/CommandLineUtils.h>
#include <RendererCore/RenderContext/BindGroupBuilder.h>
#include <RendererCore/RenderContext/RenderContext.h>
#include <RendererCore/Shader/ShaderResource.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Resources/ReadbackHelper.h>
#include <Shaders/Common/GlobalConstants.h>
#include <Shaders/Vegetation/Grass/GrassContactAOConstants.h>
#include <TestFramework/Framework/TestFramework.h>
#include <Texture/Image/Image.h>

// Optional regression fixture extracted from a GPU dump: exact camera, receiver depth and grass pool/draws.
void TestGrassAOReplay(plGALDevice* device, const char* folder)
{
  auto Read = [&](const char* name)
  {
    plStringBuilder path(folder, "/", name);
    plOSFile file;
    plDynamicArray<plUInt8> bytes;
    if (PL_TEST_BOOL(file.Open(path, plFileOpenMode::Read).Succeeded()))
      file.ReadAll(bytes);
    return bytes;
  };
  const auto globals = Read("globals.bin");
  const auto draws = Read("draws.bin");
  auto depthBytes = Read("depth-d24.bin");
  if (!PL_TEST_INT(globals.GetCount(), sizeof(plGlobalConstants)) || !PL_TEST_BOOL(!draws.IsEmpty()))
    return;
  plGlobalConstants camera;
  plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&camera), globals.GetData(), globals.GetCount());
  const plUInt32 width = plUInt32(camera.ViewportSize.x), height = plUInt32(camera.ViewportSize.y);
  if (!PL_TEST_INT(depthBytes.GetCount(), width * height * 4))
    return;
  for (plUInt32 i = 0; i < width * height; ++i)
  {
    plUInt32 packed;
    plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&packed), depthBytes.GetData() + i * 4, 4);
    const float depth = float(packed & 0xFFFFFFu) / 16777215.0f;
    plMemoryUtils::Copy(depthBytes.GetData() + i * 4, reinterpret_cast<const plUInt8*>(&depth), 4);
  }
  plDynamicArray<plGALBufferHandle> buffers;
  auto Buffer = [&](const char* file, plUInt32 stride)
  {
    const auto bytes = Read(file);
    plGALBufferCreationDescription desc;
    desc.m_uiTotalSize = bytes.GetCount();
    desc.m_uiStructSize = stride;
    desc.m_BufferFlags = plGALBufferUsageFlags::ShaderResource | plGALBufferUsageFlags::StructuredBuffer;
    auto handle = device->CreateBuffer(desc, bytes);
    buffers.PushBack(handle);
    return handle;
  };
  const auto roots = Buffer("roots.bin", 32);
  const auto pages = Buffer("pages.bin", 16);
  const auto sources = Buffer("sources.bin", 256);
  plGALTextureCreationDescription desc;
  desc.SetAsRenderTarget(width, height, plGALResourceFormat::RFloat);
  plGALSystemMemoryDescription memory;
  memory.m_pData = depthBytes;
  memory.m_uiRowPitch = width * 4;
  memory.m_uiSlicePitch = width * height * 4;
  auto depth = device->CreateTexture(desc, plMakeArrayPtr(&memory, 1));
  desc.m_Format = plGALResourceFormat::RGHalf;
  auto target = device->CreateTexture(desc);
  const bool volumes = plCommandLineUtils::GetGlobalInstance()->GetBoolOption("-grassAOReplayVolumes", false);
  auto shader = plResourceManager::LoadResource<plShaderResource>(volumes ? "Shaders/Vegetation/Grass/GrassContactAO.plShader" : "GrassContactAOBaseline.plShader");
  plGALBufferHandle indices;
  if (volumes)
  {
    const plUInt16 box[] = {0,2,1, 1,2,3, 4,5,6, 5,7,6, 0,1,4, 1,5,4, 2,6,3, 3,6,7, 0,4,2, 2,4,6, 1,3,5, 3,7,5};
    plGALBufferCreationDescription indexDesc;
    indexDesc.m_uiTotalSize = sizeof(box);
    indexDesc.m_uiStructSize = sizeof(plUInt16);
    indexDesc.m_BufferFlags = plGALBufferUsageFlags::IndexBuffer;
    indices = device->CreateBuffer(indexDesc, plMakeArrayPtr(box).ToByteArray());
  }
  struct Sample { plGALTimestampHandle start, end; };
  plDynamicArray<Sample> pending;
  plDynamicArray<double> times;
  auto* context = plRenderContext::GetDefaultInstance();
  for (plUInt32 frame = 0; frame < 36; ++frame)
  {
    device->BeginFrame();
    for (plUInt32 i = pending.GetCount(); i-- > 0;)
    {
      plTime start, end;
      if (device->GetTimestampResult(pending[i].start, start) == plGALAsyncResult::Ready &&
          device->GetTimestampResult(pending[i].end, end) == plGALAsyncResult::Ready)
      {
        times.PushBack((end - start).GetMilliseconds());
        pending.RemoveAtAndCopy(i);
      }
    }
    auto* encoder = device->BeginCommands("Grass contact replay");
    plGALRenderingSetup rendering;
    rendering.SetColorTarget(0, device->GetDefaultRenderTargetView(target));
    rendering.SetClearColor(0, plColor::White);
    context->BeginRendering(rendering, plRectFloat(0, 0, float(width), float(height)));
    context->WriteGlobalConstants() = camera;
    context->SetShaderPermutationVariable("CAMERA_MODE", "CAMERA_MODE_PERSPECTIVE");
    auto& group = context->GetBindGroup(PL_GAL_BIND_GROUP_DRAW_CALL);
    group.BindBuffer("GrassStatic", roots);
    group.BindBuffer("GrassPages", pages);
    group.BindBuffer("GrassSources", sources);
    group.BindTexture("ContactDepth", depth);
    context->BindShader(shader);
    if (volumes)
      context->BindMeshBuffer({}, indices, {}, plGALPrimitiveTopology::Triangles, 12);
    else
      context->BindNullMeshBuffer(plGALPrimitiveTopology::Triangles, 4096 * 2);
    const auto start = encoder->InsertTimestamp();
    for (plUInt32 offset = 0; offset < draws.GetCount(); offset += sizeof(plGrassContactAOConstants))
    {
      plGrassContactAOConstants constants;
      plMemoryUtils::Copy(reinterpret_cast<plUInt8*>(&constants), draws.GetData() + offset, sizeof(constants));
      context->SetPushConstants("plGrassContactAOConstants", constants);
      PL_TEST_BOOL(context->DrawMeshBuffer(volumes ? 12 : 8192, 0, volumes ? 4096 : 1).Succeeded());
    }
    const auto end = encoder->InsertTimestamp();
    if (frame >= 4)
      pending.PushBack({start, end});
    context->EndRendering();
    plGALReadbackTextureHelper readback;
    if (frame == 35)
      readback.ReadbackTexture(*encoder, target);
    encoder->Flush();
    device->EndCommands(encoder);
    device->EndFrame();
    if (frame == 35 && PL_TEST_BOOL(readback.GetReadbackResult(plTime::MakeFromSeconds(30)) == plGALAsyncResult::Ready))
    {
      plGALTextureSubresource sub;
      plDynamicArray<plGALSystemMemoryDescription> result;
      auto lock = readback.LockTexture(plMakeArrayPtr(&sub, 1), result);
      plImageHeader header;
      header.SetWidth(width);
      header.SetHeight(height);
      header.SetImageFormat(plImageFormat::R8G8B8A8_UNORM);
      plImage image;
      image.ResetAndAlloc(header);
      for (plUInt32 y = 0; y < height; ++y)
      {
        auto* out = image.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
        const auto* in = reinterpret_cast<const plFloat16*>(result[0].m_pData.GetPtr() + y * result[0].m_uiRowPitch);
        for (plUInt32 x = 0; x < width; ++x)
        {
          out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = plUInt8(plMath::Clamp(float(in[x * 2]), 0.0f, 1.0f) * 255);
          out[x * 4 + 3] = 255;
        }
      }
      plStringBuilder output(volumes ? ":shadercache/grass-ao-replay-after.png" : ":shadercache/grass-ao-replay-before.png");
      PL_TEST_BOOL(image.SaveTo(output).Succeeded());
      if (volumes)
      {
        plImage before;
        if (PL_TEST_BOOL(before.LoadFrom(":shadercache/grass-ao-replay-before.png").Succeeded()) &&
            PL_TEST_INT(before.GetWidth(), width) && PL_TEST_INT(before.GetHeight(), height))
        {
          plUInt32 changed = 0, affected = 0, maximumDifference = 0;
          for (plUInt32 y = 0; y < height; ++y)
          {
            const auto* oldPixels = before.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
            const auto* newPixels = image.GetPixelPointer<plUInt8>(0, 0, 0, 0, y);
            for (plUInt32 x = 0; x < width; ++x)
            {
              const auto difference = plUInt32(plMath::Abs(plInt32(oldPixels[x * 4]) - plInt32(newPixels[x * 4])));
              changed += difference > 2;
              affected += oldPixels[x * 4] < 253;
              maximumDifference = plMath::Max(maximumDifference, difference);
            }
          }
          plLog::Warning("Grass contact replay image: {} pixels differ by more than 2/255, {} AO pixels, maximum difference {}/255", changed, affected, maximumDifference);
          PL_TEST_BOOL(affected > 100);
          PL_TEST_BOOL(changed <= affected / 100 + 32);
        }
      }
    }
  }
  times.Sort();
  if (PL_TEST_BOOL(times.GetCount() >= 20))
    plLog::Warning("Grass contact dump replay: median {} ms, {} samples, {} draws", times[times.GetCount() / 2], times.GetCount(), draws.GetCount() / sizeof(plGrassContactAOConstants));
  context->ResetContextState();
  if (!indices.IsInvalidated())
    device->DestroyBuffer(indices);
  device->DestroyTexture(target);
  device->DestroyTexture(depth);
  for (auto buffer : buffers)
    device->DestroyBuffer(buffer);
}
