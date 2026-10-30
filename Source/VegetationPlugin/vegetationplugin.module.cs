using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class VegetationPluginModule : ModuleRules
{
    public VegetationPluginModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/VegetationPlugin", "VegetationPluginPCH.h",
            "BUILDSYSTEM_BUILDING_VEGETATIONPLUGIN_LIB");

        // Shader headers shared with the engine's data directories (constants structs).
        PrivateIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Data", "Base"));
        PrivateIncludePaths.Add(Path.Combine(context.ProjectRoot, "Assets"));
    }
}
