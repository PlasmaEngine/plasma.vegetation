#include <Core/World/World.h>
#include <Core/WorldSerializer/WorldReader.h>
#include <Core/WorldSerializer/WorldWriter.h>
#include <Foundation/Basics.h>
#include <Foundation/Configuration/Startup.h>
#include <Foundation/IO/FileSystem/FileSystem.h>
#include <Foundation/IO/MemoryStream.h>
#include <RendererCore/Material/MaterialResource.h>
#include <RendererCore/Meshes/CpuMeshResource.h>
#include <RendererCore/Meshes/MeshResource.h>
#include <RendererFoundation/Device/Device.h>
#include <RendererFoundation/Device/DeviceFactory.h>
#include <TestFramework/Framework/TestFramework.h>
#include <TestFramework/Utilities/TestSetup.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>
#include <VegetationPlugin/Grass/GrassSurfaceComponent.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>

#include <Shaders/Vegetation/Grass/GrassGpuData.h>

PL_TESTFRAMEWORK_ENTRY_POINT("GrassTest", "Grass Tests")
PL_CREATE_SIMPLE_TEST_GROUP(Grass);

void TestGrassMaterial(plGALDevice* pDevice);

// plGrassSurfaceComponent grows its roots on the CPU and hands them to the world's GPU strand pool. These tests cover
// the CPU side: strand counts from area and density, deterministic regeneration, vertex-length masks, the pool
// bookkeeping and scene serialization. Simulation, culling and drawing run on the GPU and are not read back here.
PL_CREATE_SIMPLE_TEST(Grass, SurfaceGenerationAndSerialization)
{
  plStartup::StartupCoreSystems();
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(">sdk/Data/Base", "GrassTests", "base").Succeeded());
  PL_TEST_BOOL(plFileSystem::AddDataDirectory(">appdir/../../Assets", "GrassTests", "grass").Succeeded());

  plGALDeviceCreationDescription deviceDesc;
  plGALDevice* pDevice = plGALDeviceFactory::CreateDevice("DX12", plFoundation::GetDefaultAllocator(), deviceDesc);
  if (!PL_TEST_BOOL(pDevice != nullptr && pDevice->Init().Succeeded()))
    return;
  plGALDevice::SetDefaultDevice(pDevice);
  plStartup::StartupHighLevelSystems();

  {
    // One triangle of 2 m^2.
    plMeshResourceDescriptor meshDesc;
    auto& buffer = meshDesc.MeshBufferDesc();
    buffer.AddCommonStreams();
    buffer.AllocateStreams(3, plGALPrimitiveTopology::Triangles, 1);
    buffer.SetPosition(0, plVec3(-1, -1, 0));
    buffer.SetPosition(1, plVec3(1, -1, 0));
    buffer.SetPosition(2, plVec3(0, 1, 0));
    for (plUInt32 i = 0; i < 3; ++i)
    {
      buffer.SetNormal(i, plVec3(0, 0, 1));
      buffer.SetTangent(i, plVec4(1, 0, 0, 1));
      buffer.SetTexCoord0(i, plVec2(0));
    }
    buffer.SetTriangleIndices(0, 0, 1, 2);
    meshDesc.AddSubMesh(1, 0, 0);
    meshDesc.ComputeBounds();
    auto cpuDesc = meshDesc;
    const auto hCpuMesh = plResourceManager::CreateResource<plCpuMeshResource>("GrassTestEmitter", std::move(cpuDesc));
    const auto hMesh = plResourceManager::CreateResource<plMeshResource>("GrassTestEmitter", std::move(meshDesc));

    // 16 strands per m^2 on 2 m^2.
    constexpr plUInt32 uiExpectedStrands = 32;
    plGrassTypeResourceDescriptor typeDesc;
    typeDesc.m_Settings.m_fStrandsPerSquareMeter = 16.0f;
    typeDesc.m_Settings.m_uiSegmentCount = 4;
    typeDesc.m_Settings.m_uiBillboardCount = 2;
    typeDesc.m_Settings.m_fRandomness = 0.0f;
    typeDesc.m_Settings.m_fWidth = 0.05f;
    plGrassAtlasRect atlas;
    atlas.m_vTexMulAdd = plVec4(0.5f, 0.5f, 0.25f, 0.25f);
    typeDesc.m_Settings.m_AtlasRects.PushBack(atlas);
    const auto hType = plResourceManager::CreateResource<plGrassTypeResource>("GrassTestType", std::move(typeDesc));

    plWorldDesc worldDesc("GrassTest");
    plWorld world(worldDesc);
    PL_LOCK(world.GetWriteMarker());
    world.SetWorldSimulationEnabled(false);
    world.GetClock().SetFixedTimeStep(plTime::MakeFromSeconds(1.0 / 60.0));
    plGameObject* pOwner;
    plGameObjectDesc objectDesc;
    objectDesc.m_bDynamic = true;
    world.CreateObject(objectDesc, pOwner);
    plGrassSurfaceComponent* pGrass;
    world.GetOrCreateComponentManager<plGrassSurfaceComponentManager>()->CreateComponent(pOwner, pGrass);
    pGrass->SetMesh(hMesh);
    pGrass->SetGrassType(hType);
    world.Update();

    auto UsedPages = [&]()
    {
      plUInt32 uiUsed = 0;
      plUInt32 uiBudget = 0;
      plGrassGpuManager::GetPoolUsage(world.GetIndex(), uiUsed, uiBudget);
      return uiUsed;
    };

    PL_TEST_BLOCK(plTestBlock::Enabled, "Strand count, pool pages and deterministic regeneration")
    {
      PL_TEST_INT(pGrass->GetStrandCount(), uiExpectedStrands);
      PL_TEST_INT(pGrass->GetSettings().m_uiSegmentCount, 4);
      PL_TEST_INT(UsedPages(), 1);

      pGrass->SetMesh(hMesh);
      world.Update();
      PL_TEST_INT(pGrass->GetStrandCount(), uiExpectedStrands);
      PL_TEST_INT(UsedPages(), 1);

      // Strand packing is a pure function of its inputs.
      plGrassStrandStatic a;
      plGrassStrandStatic b;
      plGrassGpuManager::MakeStrandStatic(plVec3(0.25f, -0.5f, 0.0f), plVec3(0.1f, 0.2f, 0.97f), 0.8f, 0.3f, 1, 12345, a);
      plGrassGpuManager::MakeStrandStatic(plVec3(0.25f, -0.5f, 0.0f), plVec3(0.1f, 0.2f, 0.97f), 0.8f, 0.3f, 1, 12345, b);
      PL_TEST_BOOL(plMemoryUtils::IsEqual(&a, &b, 1));
      PL_TEST_INT((a.AtlasFlags >> 8) & 0xFF, GRASS_STRAND_FLAG_ALIVE);
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "Vertex masks, empty geometry and recovery")
    {
      for (plUInt32 i = 0; i < 3; ++i)
        pGrass->VertexLengths_Insert(i, 0.0f);
      world.Update();
      PL_TEST_INT(pGrass->GetStrandCount(), 0);
      PL_TEST_INT(UsedPages(), 0);

      for (plUInt32 i = 0; i < 3; ++i)
        pGrass->VertexLengths_SetValue(i, 1.0f);
      world.Update();
      PL_TEST_INT(pGrass->GetStrandCount(), uiExpectedStrands);

      pOwner->SetLocalScaling(plVec3(0));
      world.Update();
      PL_TEST_INT(UsedPages(), 0);
      pOwner->SetLocalScaling(plVec3(1));
      world.Update();
      PL_TEST_INT(pGrass->GetStrandCount(), uiExpectedStrands);
      PL_TEST_INT(UsedPages(), 1);
    }

    PL_TEST_BLOCK(plTestBlock::Enabled, "Scene serialization preserves settings")
    {
      plDefaultMemoryStreamStorage storage;
      plMemoryStreamWriter stream(&storage);
      plWorldWriter writer;
      writer.WriteWorld(stream, world);
      plMemoryStreamReader input(&storage);
      plWorldReader reader;
      PL_TEST_BOOL(reader.ReadWorldDescription(input).Succeeded());
      plWorld restored(worldDesc);
      PL_LOCK(restored.GetWriteMarker());
      restored.SetWorldSimulationEnabled(false);
      reader.InstantiateWorld(restored);
      restored.Update();
      auto components = restored.GetOrCreateComponentManager<plGrassSurfaceComponentManager>()->GetComponents();
      PL_TEST_BOOL(components.IsValid());
      PL_TEST_BOOL(components->GetMesh() == hMesh);
      PL_TEST_BOOL(components->GetGrassType() == hType);
      PL_TEST_INT(components->VertexLengths_GetCount(), 3);
      PL_TEST_INT(components->GetStrandCount(), uiExpectedStrands);
      PL_TEST_INT(components->GetSettings().m_AtlasRects.GetCount(), 1);
      PL_TEST_BOOL(components->GetSettings().m_AtlasRects[0].m_vTexMulAdd == atlas.m_vTexMulAdd);
      PL_TEST_INT(restored.GetObjectCount(), world.GetObjectCount());
    }

    PL_IGNORE_UNUSED(hCpuMesh);
  }

  TestGrassMaterial(pDevice);
  plStartup::ShutdownHighLevelSystems();
  plResourceManager::FreeAllUnusedResources();
  pDevice->Shutdown().IgnoreResult();
  PL_DEFAULT_DELETE(pDevice);
  plFileSystem::RemoveDataDirectoryGroup("GrassTests");
  plStartup::ShutdownCoreSystems();
}
