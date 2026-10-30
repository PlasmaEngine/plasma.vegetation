using System.IO;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class GrassUpgradeModule : ModuleRules
{
    public GrassUpgradeModule(BuildContext context) : base(context)
    {
        Type = ModuleType.Runtime;
        SourceDirectory = "Tests/GrassUpgrade";
        CppStandard = CppStandard.Cpp20;
        PlasmaPackageSdk.ConfigureSdkConsumer(this, context);
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/VegetationPlugin", "plVegetationPlugin");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plTestFramework");
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code/UnitTests/TestFramework/Platform/Win"));
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Data/Base"));
        PublicIncludePaths.Add(Path.Combine(context.ProjectRoot, "Assets"));
        PrivateLibraries.Add("Dbghelp.lib");
        PrivateDefinitions.Add("GRASS_TEST_SDK=\"" + PlasmaPackageSdk.Root.Replace('\\', '/') + "\"");
        PrivateDefinitions.Add("GRASS_TEST_PACKAGE=\"" + context.ProjectRoot.Replace('\\', '/') + "\"");
        PrivateDefinitions.Add("GRASS_TEST_BIN=\"" + PlasmaPackageSdk.SdkBinaryDirectory(context).Replace('\\', '/') + "\"");
    }
}
