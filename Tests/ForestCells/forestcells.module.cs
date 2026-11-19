using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class ForestCellsModule : ModuleRules
{
    public ForestCellsModule(BuildContext context) : base(context)
    {
        Type = ModuleType.Runtime;
        SourceDirectory = "Tests/ForestCells";
        CppStandard = CppStandard.Cpp20;
        PlasmaPackageSdk.ConfigureSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/VegetationPlugin", "plVegetationPlugin");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests/TestFramework/Platform/Win"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Data/Base"));
        PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, "Assets"));
        PrivateLibraries.Add("Dbghelp.lib");
    }
}
