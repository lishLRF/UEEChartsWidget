#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EChartsWidget.h"
#include "EChartsWidgetTestSink.h"
#include "WebBrowser.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "UObject/UnrealType.h"

namespace EChartsWidgetTests
{
	static constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::CommandletContext |
		EAutomationTestFlags::ProductFilter;

	static UEChartsWidget* MakeWidget()
	{
		UEChartsWidget* Widget = NewObject<UEChartsWidget>();
		Widget->AddToRoot();
		return Widget;
	}

	static void DestroyWidget(UEChartsWidget* Widget)
	{
		Widget->ReleaseSlateResources(false);
		Widget->RemoveFromRoot();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsReflectionDefaultsTest,
	"EChartsWidget.ReflectionDefaults", EChartsWidgetTests::Flags)
bool FEChartsReflectionDefaultsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Widget derives from UWebBrowser"), UEChartsWidget::StaticClass()->IsChildOf(UWebBrowser::StaticClass()));
	TestEqual(TEXT("Palette category"), UEChartsWidget::StaticClass()->GetMetaData(TEXT("DisplayName")), FString(TEXT("ECharts Widget")));

	const UEnum* TemplateEnum = StaticEnum<EEChartsTemplate>();
	const UEnum* InteractionEnum = StaticEnum<EEChartsInteractionMode>();
	const UEnum* RuntimeEnum = StaticEnum<EEChartsRuntimeState>();
	TestNotNull(TEXT("Template enum reflected"), TemplateEnum);
	TestNotNull(TEXT("Interaction enum reflected"), InteractionEnum);
	TestNotNull(TEXT("Runtime enum reflected"), RuntimeEnum);
	TestEqual(TEXT("Template enum has four public entries"), TemplateEnum->NumEnums() - 1, 4);
	TestEqual(TEXT("Interaction enum has three public entries"), InteractionEnum->NumEnums() - 1, 3);
	TestEqual(TEXT("Runtime enum has four public entries"), RuntimeEnum->NumEnums() - 1, 4);

	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	TestEqual(TEXT("Palette category is ECharts"), Widget->GetPaletteCategory().ToString(), FString(TEXT("ECharts")));
	TestEqual(TEXT("Default template"), Widget->CurrentTemplate, EEChartsTemplate::SegmentedAreaLine);
	TestEqual(TEXT("Default interaction"), Widget->InteractionMode, EEChartsInteractionMode::ClickOnly);
	TestEqual(TEXT("Default runtime state"), Widget->RuntimeState, EEChartsRuntimeState::Uninitialized);
	TestTrue(TEXT("Default last error is empty"), Widget->LastError.IsEmpty());

	const FProperty* TemplateProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, CurrentTemplate));
	const FProperty* InteractionProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, InteractionMode));
	const FProperty* StateProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, RuntimeState));
	const FProperty* ErrorProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, LastError));
	TestTrue(TEXT("CurrentTemplate is Blueprint read-only"), TemplateProperty && TemplateProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("InteractionMode is Blueprint read-only"), InteractionProperty && InteractionProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("RuntimeState is Blueprint read-only"), StateProperty && StateProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("LastError is Blueprint read-only"), ErrorProperty && ErrorProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsLocalResourceUrlTest,
	"EChartsWidget.LocalResourceUrl", EChartsWidgetTests::Flags)
bool FEChartsLocalResourceUrlTest::RunTest(const FString& Parameters)
{
	const FString HostPath = FEChartsWidgetResourceLocator::GetChartHostPath();
	const FString HostUrl = FEChartsWidgetResourceLocator::GetChartHostUrl();
	TestTrue(TEXT("Host page exists"), IFileManager::Get().FileExists(*HostPath));
	TestTrue(TEXT("Host path is absolute"), !FPaths::IsRelative(HostPath));
	TestTrue(TEXT("URL uses file scheme"), HostUrl.StartsWith(TEXT("file:///")));
	TestFalse(TEXT("URL contains no backslashes"), HostUrl.Contains(TEXT("\\")));
	TestFalse(TEXT("URL is not HTTP"), HostUrl.StartsWith(TEXT("http://")) || HostUrl.StartsWith(TEXT("https://")));

	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	Widget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap, EEChartsInteractionMode::FullHover);
	TestEqual(TEXT("Initialize stores template"), Widget->CurrentTemplate, EEChartsTemplate::Bar3DHeightMap);
	TestEqual(TEXT("Initialize stores interaction mode"), Widget->InteractionMode, EEChartsInteractionMode::FullHover);
	TestEqual(TEXT("Initialize enters Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	TestTrue(TEXT("Initialize clears errors"), Widget->LastError.IsEmpty());
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsConsoleHandshakeTest,
	"EChartsWidget.ConsoleHandshake", EChartsWidgetTests::Flags)
bool FEChartsConsoleHandshakeTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	UEChartsWidgetTestSink* Sink = NewObject<UEChartsWidgetTestSink>();
	Sink->AddToRoot();
	Widget->OnChartReady.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleReady);
	Widget->OnEChartsWarning.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleWarning);
	Widget->OnEChartsError.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleError);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_WARNING__:local fallback"), TEXT("chart-host.html"), 8);
	TestEqual(TEXT("Warning broadcasts once"), Sink->WarningCount, 1);
	TestEqual(TEXT("Warning strips marker"), Sink->LastWarning, FString(TEXT("local fallback")));
	TestEqual(TEXT("Warning does not change Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);

	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__"), TEXT("chart-host.html"), 9);
	TestEqual(TEXT("Ready broadcasts once"), Sink->ReadyCount, 1);
	TestEqual(TEXT("Ready state"), Widget->RuntimeState, EEChartsRuntimeState::Ready);

	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:bad option"), TEXT("chart-host.html"), 10);
	TestEqual(TEXT("Error broadcasts once"), Sink->ErrorCount, 1);
	TestEqual(TEXT("Error strips marker"), Sink->LastError, FString(TEXT("bad option")));
	TestEqual(TEXT("Error state"), Widget->RuntimeState, EEChartsRuntimeState::Error);
	TestEqual(TEXT("LastError retained"), Widget->LastError, FString(TEXT("bad option")));

	Sink->RemoveFromRoot();
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCSPNoNetworkTest,
	"EChartsWidget.CSPNoNetwork", EChartsWidgetTests::Flags)
bool FEChartsCSPNoNetworkTest::RunTest(const FString& Parameters)
{
	FString Html;
	const FString HostPath = FEChartsWidgetResourceLocator::GetChartHostPath();
	TestTrue(TEXT("Host page can be read"), FFileHelper::LoadFileToString(Html, *HostPath));
	TestTrue(TEXT("CSP defaults to local/data/blob"), Html.Contains(TEXT("default-src 'self' data: blob:")));
	TestTrue(TEXT("CSP disables network connections"), Html.Contains(TEXT("connect-src 'none'")));
	TestFalse(TEXT("No HTTP resources"), Html.Contains(TEXT("http://"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("No HTTPS resources"), Html.Contains(TEXT("https://"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("No protocol-relative resources"), Html.Contains(TEXT("src=\"//"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("Page emits ready marker"), Html.Contains(TEXT("__UE_ECHARTS_READY__")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsLifecycleNoDuplicateReadyTest,
	"EChartsWidget.LifecycleNoDuplicateReady", EChartsWidgetTests::Flags)
bool FEChartsLifecycleNoDuplicateReadyTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	UEChartsWidgetTestSink* Sink = NewObject<UEChartsWidgetTestSink>();
	Sink->AddToRoot();
	Widget->OnChartReady.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleReady);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__"), FString(), 0);
	TestEqual(TEXT("Duplicate marker broadcasts once per load"), Sink->ReadyCount, 1);

	Widget->ReleaseSlateResources(false);
	TestEqual(TEXT("Release resets runtime state"), Widget->RuntimeState, EEChartsRuntimeState::Uninitialized);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__"), FString(), 0);
	TestEqual(TEXT("Released widget ignores marker"), Sink->ReadyCount, 1);

	Widget->RebindConsoleMessageForTesting();
	Widget->RebindConsoleMessageForTesting();
	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::Disabled);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__"), FString(), 0);
	TestEqual(TEXT("Rebinding twice does not duplicate callbacks"), Sink->ReadyCount, 2);

	Sink->RemoveFromRoot();
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

#endif
