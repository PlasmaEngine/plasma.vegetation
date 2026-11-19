using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ForestCellsTarget : TargetRules
{
    public ForestCellsTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "ForestCellsTest";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = false;
        UseUnityBuild = false;
        TargetDependencies.Add("VegetationPlugin");
        ExtraModules.Add("ForestCellsModule");
    }
}
