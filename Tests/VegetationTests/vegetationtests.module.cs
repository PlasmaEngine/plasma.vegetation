using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class VegetationTestsModule : ModuleRules
{
    public VegetationTestsModule(BuildContext context) : base(context)
    {
        Type = ModuleType.Runtime;
        SourceDirectory = "Tests/VegetationTests";
        CppStandard = CppStandard.Cpp20;
        PlasmaPackageSdk.ConfigureSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/VegetationPlugin", "plVegetationPlugin");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PublicIncludePaths.Add(System.IO.Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests"));
        PublicIncludePaths.Add(System.IO.Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests/TestFramework/Platform/Win"));
        PrivateLibraries.Add("Dbghelp.lib");
    }
}
