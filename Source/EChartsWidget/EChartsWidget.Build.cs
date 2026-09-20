using UnrealBuildTool;
using System.IO;

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
			"Projects",
			"Json"
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("HTTP");
		}

		AddRuntimeDependenciesForDirectory("Resources/Web");
		AddRuntimeDependenciesForDirectory("ThirdPartyLicenses");
		RuntimeDependencies.Add("$(PluginDir)/LICENSE", StagedFileType.NonUFS);
		RuntimeDependencies.Add("$(PluginDir)/THIRD_PARTY_NOTICES.md", StagedFileType.NonUFS);
	}

	private void AddRuntimeDependenciesForDirectory(string RelativeDirectory)
	{
		string SourceDirectory = Path.Combine(PluginDirectory, RelativeDirectory);
		if (!Directory.Exists(SourceDirectory))
		{
			return;
		}

		foreach (string SourceFile in Directory.GetFiles(SourceDirectory, "*", SearchOption.AllDirectories))
		{
			string RelativeFile = Path.GetRelativePath(PluginDirectory, SourceFile).Replace('\\', '/');
			RuntimeDependencies.Add("$(PluginDir)/" + RelativeFile, StagedFileType.NonUFS);
		}
	}
}
