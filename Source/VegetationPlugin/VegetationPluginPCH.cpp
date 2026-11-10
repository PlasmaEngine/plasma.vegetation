#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Startup.h>
#include <RendererCore/Pipeline/RenderPipelineInjection.h>
#include <VegetationPlugin/Grass/GrassContactAOPass.h>
#include <VegetationPlugin/Grass/GrassCullPass.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

namespace
{
  const char* const s_szInjectionOwner = "plasma.vegetation";

  /// Grass needs two passes in the pipelines that draw it. The engine places them and keeps the pipeline working if
  /// one cannot be placed or fails. Pipeline assets never contain them, so they load without this plugin.
  void RegisterPipelineInjections()
  {
    // Culls the grass of a view before any pass draws it: the opaque and G-buffer passes, and the depth-only pass of
    // the shadow pipelines, so grass casts shadows. FrameStart is found in exactly those pipelines and orders the pass
    // ahead of every other pass.
    plRenderPipelineInjectionDesc cull;
    cull.m_sName.Assign("plasma.vegetation/GrassCull");
    cull.m_sOwner.Assign(s_szInjectionOwner);
    cull.m_sPoint.Assign(plRenderPipelineInjectionPoints::FrameStart);
    cull.m_pPassType = plGetStaticRTTI<plGrassCullPass>();
    plRenderPipelineInjections::Register(cull).IgnoreResult();

    // Darkens the scene's ambient occlusion where grass stands, behind the AO pass and in front of all its readers.
    // Shadow and picking pipelines have no AO pass and get none.
    plRenderPipelineInjectionDesc contactAO;
    contactAO.m_sName.Assign("plasma.vegetation/GrassContactAO");
    contactAO.m_sOwner.Assign(s_szInjectionOwner);
    contactAO.m_sPoint.Assign(plRenderPipelineInjectionPoints::AfterAmbientOcclusion);
    contactAO.m_pPassType = plGetStaticRTTI<plGrassContactAOPass>();
    contactAO.Bind("AO", "AO", "AO");
    contactAO.Read("Depth", "Depth");
    plRenderPipelineInjections::Register(contactAO).IgnoreResult();
  }
} // namespace

// clang-format off
PL_BEGIN_SUBSYSTEM_DECLARATION(Vegetation, VegetationPlugin)

  BEGIN_SUBSYSTEM_DEPENDENCIES
    "Foundation",
    "Core",
    "RenderWorld",
    "WindFieldManager",
    "VegetationTouchMapManager"
  END_SUBSYSTEM_DEPENDENCIES

  ON_CORESYSTEMS_STARTUP
  {
    plResourceManager::RegisterResourceForAssetType("Vegetation Type", plGetStaticRTTI<plVegetationTypeResource>());

    {
      plVegetationTypeResourceDescriptor desc;
      plVegetationTypeResourceHandle hResource = plResourceManager::CreateResource<plVegetationTypeResource>("Missing Vegetation Type", std::move(desc), "Empty Vegetation Type");
      plResourceManager::SetResourceTypeMissingFallback<plVegetationTypeResource>(hResource);
    }

    plResourceManager::RegisterResourceForAssetType("Grass Type", plGetStaticRTTI<plGrassTypeResource>());

    {
      plGrassTypeResourceDescriptor desc;
      plGrassTypeResourceHandle hResource = plResourceManager::CreateResource<plGrassTypeResource>("Missing Grass Type", std::move(desc), "Empty Grass Type");
      plResourceManager::SetResourceTypeMissingFallback<plGrassTypeResource>(hResource);
    }

    RegisterPipelineInjections();
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
    plRenderPipelineInjections::UnregisterOwner(s_szInjectionOwner);

    plResourceManager::SetResourceTypeMissingFallback<plVegetationTypeResource>(plVegetationTypeResourceHandle());
    plVegetationTypeResource::CleanupDynamicPluginReferences();
    plResourceManager::SetResourceTypeMissingFallback<plGrassTypeResource>(plGrassTypeResourceHandle());
    plGrassTypeResource::CleanupDynamicPluginReferences();
  }

  ON_HIGHLEVELSYSTEMS_STARTUP
  {
    // After the wind field and touch map managers, so grass simulates against this frame's fields.
    plGrassGpuManager::OnStartup();
  }

  ON_HIGHLEVELSYSTEMS_SHUTDOWN
  {
    plGrassGpuManager::OnShutdown();
  }

PL_END_SUBSYSTEM_DECLARATION;
// clang-format on

PL_STATICLINK_LIBRARY(VegetationPlugin)
{
  if (bReturn)
    return;

  PL_STATICLINK_REFERENCE(VegetationPlugin_Components_VegetationCellComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Components_VegetationInstanceComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Components_VegetationLayerComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Components_VegetationTouchComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassCullPass);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassGpuManager);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassRenderComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassRenderer);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassSurfaceComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassTypeResource);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Resources_VegetationTypeResource);
  PL_STATICLINK_REFERENCE(VegetationPlugin_World_VegetationWorldModule);
}
