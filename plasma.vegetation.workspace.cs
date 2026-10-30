// #pl-version 1

using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public class PlasmaVegetationWorkspace : WorkspaceRules
{
    public PlasmaVegetationWorkspace(BuildContext context) : base(context)
    {
        TargetNames.Add("VegetationPlugin");
        TargetNames.Add("EnginePluginVegetation");
        TargetNames.Add("EditorPluginVegetation");
    }
}
