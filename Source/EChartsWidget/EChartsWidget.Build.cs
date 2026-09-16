using UnrealBuildTool;

public class EChartsWidget : ModuleRules
{
	public EChartsWidget(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore",
			"WebBrowserWidget",
			"WebBrowser",
			"Projects"
		});
	}
}
