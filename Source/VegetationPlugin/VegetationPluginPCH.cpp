#include <VegetationPlugin/VegetationPluginPCH.h>

#include <Core/ResourceManager/ResourceManager.h>
#include <Foundation/Configuration/Startup.h>
#include <VegetationPlugin/Grass/GrassGpuManager.h>
#include <VegetationPlugin/Grass/GrassTypeResource.h>
#include <VegetationPlugin/Resources/VegetationTypeResource.h>

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
  }

  ON_CORESYSTEMS_SHUTDOWN
  {
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
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassPipelineExtension);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassRenderComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassRenderer);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassSurfaceComponent);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Grass_GrassTypeResource);
  PL_STATICLINK_REFERENCE(VegetationPlugin_Resources_VegetationTypeResource);
  PL_STATICLINK_REFERENCE(VegetationPlugin_World_VegetationWorldModule);
}
