#include <EnginePluginVegetation/EnginePluginVegetationPCH.h>

#include <EnginePluginVegetation/Brush/VegetationBrushHandler.h>
#include <Foundation/Configuration/Plugin.h>

PL_PLUGIN_ON_LOADED()
{
  plVegetationBrushHandler::Startup();
}

PL_PLUGIN_ON_UNLOADED()
{
  plVegetationBrushHandler::Shutdown();
}
