using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginVegetationModule : ModuleRules
{
    public EnginePluginVegetationModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EnginePluginVegetation", "EnginePluginVegetationPCH.h",
            "BUILDSYSTEM_BUILDING_ENGINEPLUGINVEGETATION_LIB");

        PlasmaPackageSdk.AddImportLibrary(this, context, "EditorEngineProcessFramework");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/VegetationPlugin", "plVegetationPlugin");
    }
}
