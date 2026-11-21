using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

/// Headless CPU tests of the vegetation runtime: scatter sampling, file format compatibility, clearings.
public class VegetationTestsTarget : TargetRules
{
    public VegetationTestsTarget(BuildContext context) : base(context)
    {
        Type = TargetType.Executable;
        OutputName = "VegetationTests";
        OutputDirectory = PlasmaPackageSdk.PackageBinaryDirectory(context);
        UsePCHFiles = false;
        UseUnityBuild = false;
        TargetDependencies.Add("VegetationPlugin");
        ExtraModules.Add("VegetationTestsModule");
    }
}
