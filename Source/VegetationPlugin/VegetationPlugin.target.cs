using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class VegetationPluginTarget : TargetRules
{
    public VegetationPluginTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plVegetationPlugin";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        ExtraModules.Add("VegetationPluginModule");
    }
}
