using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class GrassUpgradeTarget : TargetRules
{
    public GrassUpgradeTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "GrassUpgradeTest";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = false;
        UseUnityBuild = false;
        TargetDependencies.Add("VegetationPlugin");
        ExtraModules.Add("GrassUpgradeModule");
    }
}
