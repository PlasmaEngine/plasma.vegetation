using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class EditorPluginVegetationModule : ModuleRules
{
    public EditorPluginVegetationModule(BuildContext context) : base(context)
    {
        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/EditorPluginVegetation", "EditorPluginVegetationPCH.h",
            "BUILDSYSTEM_BUILDING_EDITORPLUGINVEGETATION_LIB");

        PlasmaPackageSdk.ConfigureEditorSdkConsumer(this, context);

        // The asset-document framework the type asset builds on, and the scene plugin for the paint tool.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorPluginAssets");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plSharedPluginAssets");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorPluginScene");

        // The Vegetation panel is engine-drawn: EditorUi and the retained UI runtime.
        PlasmaPackageSdk.AddImportLibrary(this, context, "plEditorUi");
        PlasmaPackageSdk.AddImportLibrary(this, context, "plGameUiPlugin");

        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/VegetationPlugin", "plVegetationPlugin");
        PlasmaPackageSdk.AddPackagePluginImport(this, context, "Source/EnginePluginVegetation", "plEnginePluginVegetation");
    }
}
