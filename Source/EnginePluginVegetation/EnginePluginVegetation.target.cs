using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EnginePluginVegetationTarget : TargetRules
{
    public EnginePluginVegetationTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEnginePluginVegetation";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("VegetationPlugin");
        ExtraModules.Add("EnginePluginVegetationModule");
    }
}
