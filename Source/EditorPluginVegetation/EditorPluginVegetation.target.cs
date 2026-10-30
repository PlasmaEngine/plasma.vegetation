using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginVegetationTarget : TargetRules
{
    public EditorPluginVegetationTarget(BuildContext context) : base(context)
    {
        Type = TargetType.SharedLibrary;
        OutputName = "plEditorPluginVegetation";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = true;
        UseUnityBuild = true;
        UseAdaptiveUnityBuild = true;
        UseIncrementalLinking = true;
        TargetDependencies.Add("VegetationPlugin");
        TargetDependencies.Add("EnginePluginVegetation");
        ExtraModules.Add("EditorPluginVegetationModule");
    }
}
