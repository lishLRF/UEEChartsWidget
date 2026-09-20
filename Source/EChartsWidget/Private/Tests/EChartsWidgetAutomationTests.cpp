#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EChartsWidget.h"
#include "EChartsWidgetTestSink.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "WebBrowser.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"

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

	struct FBrowserIntegrationState
	{
		UEChartsWidget* Widget = nullptr;
		UEChartsWidgetTestSink* Sink = nullptr;
		TSharedPtr<SWidget> SlateWidget;
		double DeadlineSeconds = 0.0;
		int32 ExpectedReadyCount = 0;
		uint64 ExpectedGeneration = 0;
		bool bFailed = false;
		FString ExpectedEffectiveTemplate;
		int32 ExpectedRenderedCount = 0;
		int32 ExpectedWarningCount = 0;
		bool bExpectSingleSeries = false;
		int32 ResizeReportsBefore = 0;

		void Cleanup()
		{
			SlateWidget.Reset();
			if (Widget)
			{
				Widget->ReleaseSlateResources(false);
				Widget->RemoveFromRoot();
				Widget = nullptr;
			}
			if (Sink)
			{
				Sink->RemoveFromRoot();
				Sink = nullptr;
			}
		}
	};

	class FStartTemplateRenderCommand : public IAutomationLatentCommand
	{
	public:
		FStartTemplateRenderCommand(
			const TSharedRef<FBrowserIntegrationState>& InState,
			FAutomationTestBase* InTest,
			const EEChartsTemplate InTemplate,
			const FString& InExpectedEffectiveTemplate,
			const bool bInForceWebGLUnavailable,
			const FString& InPayloadJson = TEXT("{}"),
			const bool bInExpectSingleSeries = false)
			: State(InState), Test(InTest), Template(InTemplate),
			  ExpectedEffectiveTemplate(InExpectedEffectiveTemplate),
			  bForceWebGLUnavailable(bInForceWebGLUnavailable),
			  PayloadJson(InPayloadJson), bExpectSingleSeries(bInExpectSingleSeries)
		{
		}

		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (!State->Widget)
			{
				State->Widget = MakeWidget();
				State->Sink = NewObject<UEChartsWidgetTestSink>();
				State->Sink->AddToRoot();
				State->Widget->OnChartReady.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleReady);
				State->Widget->OnChartRendered.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleRendered);
				State->Widget->OnEChartsWarning.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleWarning);
				State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
				State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
				State->SlateWidget = State->Widget->TakeWidget();
			}
			State->Widget->SetForceWebGLUnavailableForTesting(bForceWebGLUnavailable);
			State->Widget->SetInitializationPayloadForTesting(PayloadJson, bExpectSingleSeries);
			State->Widget->InitializeECharts(Template, EEChartsInteractionMode::ClickOnly);
			State->ExpectedEffectiveTemplate = ExpectedEffectiveTemplate;
			State->bExpectSingleSeries = bExpectSingleSeries;
			++State->ExpectedRenderedCount;
			if (bForceWebGLUnavailable &&
				(Template == EEChartsTemplate::Bar3DHeightMap || Template == EEChartsTemplate::DataTableScatter3D))
			{
				++State->ExpectedWarningCount;
			}
			State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
			return true;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
		EEChartsTemplate Template;
		FString ExpectedEffectiveTemplate;
		bool bForceWebGLUnavailable;
		FString PayloadJson;
		bool bExpectSingleSeries;
	};

	class FWaitForTemplateRenderCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForTemplateRenderCommand(const TSharedRef<FBrowserIntegrationState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Widget->RuntimeState == EEChartsRuntimeState::Error)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("CEF template render failed: %s"), *State->Widget->LastError));
				return true;
			}
			if (State->Sink->RenderedCount >= State->ExpectedRenderedCount)
			{
				Test->TestEqual(TEXT("CEF emitted one rendered marker per requested template"), State->Sink->RenderedCount, State->ExpectedRenderedCount);
				Test->TestEqual(TEXT("CEF rendered the expected WebGL or fallback template"), State->Widget->EffectiveTemplate, State->ExpectedEffectiveTemplate);
				Test->TestEqual(TEXT("CEF emitted expected fallback warnings"), State->Sink->WarningCount, State->ExpectedWarningCount);
				if (State->bExpectSingleSeries)
				{
					Test->TestEqual(TEXT("CEF reported the applied CustomOption series"), State->Sink->SeriesCountReportCount, 1);
					Test->TestEqual(TEXT("CEF preserved one CustomOption series"), State->Sink->LastSeriesCount, 1);
				}
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Timed out waiting for CEF rendered marker; URL: %s"), *State->Widget->GetUrl()));
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FFinishTemplateRenderCommand : public IAutomationLatentCommand
	{
	public:
		explicit FFinishTemplateRenderCommand(const TSharedRef<FBrowserIntegrationState>& InState) : State(InState) {}
		virtual bool Update() override { State->Cleanup(); return true; }
	private:
		TSharedRef<FBrowserIntegrationState> State;
	};

	class FTriggerBrowserResizeCommand : public IAutomationLatentCommand
	{
	public:
		FTriggerBrowserResizeCommand(const TSharedRef<FBrowserIntegrationState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			State->ResizeReportsBefore = State->Sink->ResizeReportCount;
			State->Widget->ExecuteJavascript(TEXT(
				"window.__UE_ECHARTS_TEST_RESIZE_PROBE__=true;"
				"document.getElementById('chart').style.width='320px';"
				"document.getElementById('chart').style.height='200px';"));
			State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
			return true;
		}
	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FWaitForBrowserResizeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForBrowserResizeCommand(const TSharedRef<FBrowserIntegrationState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Sink->ResizeReportCount > State->ResizeReportsBefore)
			{
				Test->TestEqual(TEXT("CEF ResizeObserver applied the requested width"), State->Sink->LastResizeWidth, 320);
				Test->TestEqual(TEXT("CEF ResizeObserver applied the requested height"), State->Sink->LastResizeHeight, 200);
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for event-driven CEF ResizeObserver callback."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FStartHeightMapLimitErrorCommand : public IAutomationLatentCommand
	{
	public:
		FStartHeightMapLimitErrorCommand(const TSharedRef<FBrowserIntegrationState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnChartRendered.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleRendered);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->SlateWidget = State->Widget->TakeWidget();
			State->Widget->SetInitializationPayloadForTesting(TEXT("{\"size\":129}"), false);
			State->Widget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap, EEChartsInteractionMode::ClickOnly);
			State->DeadlineSeconds = FPlatformTime::Seconds() + 15.0;
			return true;
		}
	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FWaitForHeightMapLimitErrorCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForHeightMapLimitErrorCommand(const TSharedRef<FBrowserIntegrationState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->Sink->ErrorCount > 0)
			{
				Test->TestTrue(TEXT("CEF size-limit ERROR is clear"), State->Sink->LastError.Contains(TEXT("size")) && State->Sink->LastError.Contains(TEXT("128")));
				return true;
			}
			if (State->Sink->RenderedCount > 0)
			{
				Test->AddError(TEXT("Oversized generated height map rendered instead of emitting ERROR."));
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				Test->AddError(TEXT("Timed out waiting for oversized height-map ERROR marker."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FStartBrowserGenerationCommand : public IAutomationLatentCommand
	{
	public:
		FStartBrowserGenerationCommand(
			const TSharedRef<FBrowserIntegrationState>& InState,
			FAutomationTestBase* InTest,
			const bool bInRebuild)
			: State(InState)
			, Test(InTest)
			, bRebuild(bInRebuild)
		{
		}

		virtual bool Update() override
		{
			if (State->bFailed)
			{
				return true;
			}

			if (!State->Widget)
			{
				State->Widget = MakeWidget();
				State->Sink = NewObject<UEChartsWidgetTestSink>();
				State->Sink->AddToRoot();
				State->Widget->OnChartReady.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleReady);
				State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			}
			if (bRebuild)
			{
				State->SlateWidget.Reset();
				State->Widget->ReleaseSlateResources(false);
				State->SlateWidget = State->Widget->TakeWidget();
				const int32 ReadyCountBeforeStaleMarker = State->Sink->ReadyCount;
				State->Widget->OnConsoleMessage.Broadcast(
					FString::Printf(TEXT("__UE_ECHARTS_READY__:%llu"), State->ExpectedGeneration),
					FString(),
					0);
				Test->TestEqual(
					TEXT("Old CEF generation marker is ignored after automatic rebuild"),
					State->Sink->ReadyCount,
					ReadyCountBeforeStaleMarker);
				Test->TestEqual(
					TEXT("Automatic rebuild enters Loading for a new generation"),
					State->Widget->RuntimeState,
					EEChartsRuntimeState::Loading);
			}
			else
			{
				State->SlateWidget = State->Widget->TakeWidget();
				State->Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
			}
			++State->ExpectedGeneration;
			++State->ExpectedReadyCount;
			State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
			return true;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
		bool bRebuild;
	};

	class FStartBrowserErrorRecoveryCommand : public IAutomationLatentCommand
	{
	public:
		FStartBrowserErrorRecoveryCommand(
			const TSharedRef<FBrowserIntegrationState>& InState,
			FAutomationTestBase* InTest)
			: State(InState)
			, Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (State->bFailed)
			{
				return true;
			}

			Test->TestEqual(TEXT("Ready rebuild scenario emitted two Ready events"), State->Sink->ReadyCount, 2);
			Test->TestEqual(TEXT("Ready rebuild scenario emitted no Error events"), State->Sink->ErrorCount, 0);
			State->Cleanup();
			State->ExpectedGeneration = 0;
			State->ExpectedReadyCount = 0;

			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnChartReady.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleReady);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->SlateWidget = State->Widget->TakeWidget();
			State->Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::FullHover);
			State->Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:integration failure"), FString(), 0);
			Test->TestEqual(TEXT("First error generation is terminal"), State->Widget->RuntimeState, EEChartsRuntimeState::Error);
			Test->TestEqual(TEXT("First error generation broadcasts once"), State->Sink->ErrorCount, 1);

			State->SlateWidget.Reset();
			State->Widget->ReleaseSlateResources(false);
			State->SlateWidget = State->Widget->TakeWidget();
			Test->TestEqual(TEXT("Rebuild after Error enters Loading"), State->Widget->RuntimeState, EEChartsRuntimeState::Loading);
			Test->TestTrue(TEXT("Rebuild after Error clears LastError"), State->Widget->LastError.IsEmpty());
			State->Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
			State->Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:stale"), FString(), 0);
			Test->TestEqual(TEXT("Stale error-generation Ready is ignored"), State->Sink->ReadyCount, 0);
			Test->TestEqual(TEXT("Stale error-generation Error is ignored"), State->Sink->ErrorCount, 1);

			State->ExpectedGeneration = 2;
			State->ExpectedReadyCount = 1;
			State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
			return true;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FWaitForBrowserReadyCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForBrowserReadyCommand(
			const TSharedRef<FBrowserIntegrationState>& InState,
			FAutomationTestBase* InTest)
			: State(InState)
			, Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (State->bFailed)
			{
				return true;
			}

			if (State->Widget->RuntimeState == EEChartsRuntimeState::Error)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(
					TEXT("CEF page generation %llu reported Error: %s"),
					State->ExpectedGeneration,
					*State->Widget->LastError));
				return true;
			}

			if (State->Widget->RuntimeState == EEChartsRuntimeState::Ready)
			{
				Test->TestEqual(
					TEXT("CEF Ready broadcasts exactly once per generation"),
					State->Sink->ReadyCount,
					State->ExpectedReadyCount);
				const FString LoadedUrl = State->Widget->GetUrl();
				Test->TestTrue(TEXT("CEF loaded the local file URL"), LoadedUrl.StartsWith(TEXT("file:///")));
				Test->TestTrue(
					TEXT("CEF loaded the expected generation query"),
					LoadedUrl.Contains(FString::Printf(TEXT("generation=%llu"), State->ExpectedGeneration)));
				return true;
			}

			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(
					TEXT("Timed out waiting for real CEF Ready for generation %llu; current URL: %s"),
					State->ExpectedGeneration,
					*State->Widget->GetUrl()));
				return true;
			}

			return false;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};

	class FFinishBrowserIntegrationCommand : public IAutomationLatentCommand
	{
	public:
		FFinishBrowserIntegrationCommand(
			const TSharedRef<FBrowserIntegrationState>& InState,
			FAutomationTestBase* InTest)
			: State(InState)
			, Test(InTest)
		{
		}

		virtual bool Update() override
		{
			if (!State->bFailed)
			{
				Test->TestEqual(TEXT("Error rebuild generation emitted one Ready"), State->Sink->ReadyCount, 1);
				Test->TestEqual(TEXT("Only the first error generation emitted Error"), State->Sink->ErrorCount, 1);
			}
			State->Cleanup();
			return true;
		}

	private:
		TSharedRef<FBrowserIntegrationState> State;
		FAutomationTestBase* Test;
	};
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
	TestTrue(TEXT("Default last warning is empty"), Widget->LastWarning.IsEmpty());
	TestTrue(TEXT("Default effective template is empty"), Widget->EffectiveTemplate.IsEmpty());

	const FProperty* TemplateProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, CurrentTemplate));
	const FProperty* InteractionProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, InteractionMode));
	const FProperty* StateProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, RuntimeState));
	const FProperty* ErrorProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, LastError));
	const FProperty* WarningProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, LastWarning));
	const FProperty* EffectiveTemplateProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, EffectiveTemplate));
	TestTrue(TEXT("CurrentTemplate is Blueprint read-only"), TemplateProperty && TemplateProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("InteractionMode is Blueprint read-only"), InteractionProperty && InteractionProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("RuntimeState is Blueprint read-only"), StateProperty && StateProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("LastError is Blueprint read-only"), ErrorProperty && ErrorProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("LastWarning is Blueprint read-only"), WarningProperty && WarningProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("EffectiveTemplate is Blueprint read-only"), EffectiveTemplateProperty && EffectiveTemplateProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsJavascriptMappingTest,
	"EChartsWidget.JavascriptMapping", EChartsWidgetTests::Flags)
bool FEChartsJavascriptMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("SegmentedAreaLine mapping"), FEChartsWidgetJavascript::TemplateName(EEChartsTemplate::SegmentedAreaLine), FString(TEXT("SegmentedAreaLine")));
	TestEqual(TEXT("Bar3DHeightMap mapping"), FEChartsWidgetJavascript::TemplateName(EEChartsTemplate::Bar3DHeightMap), FString(TEXT("Bar3DHeightMap")));
	TestEqual(TEXT("DataTableScatter3D mapping"), FEChartsWidgetJavascript::TemplateName(EEChartsTemplate::DataTableScatter3D), FString(TEXT("DataTableScatter3D")));
	TestEqual(TEXT("CustomOption mapping"), FEChartsWidgetJavascript::TemplateName(EEChartsTemplate::CustomOption), FString(TEXT("CustomOption")));
	TestEqual(TEXT("Disabled mapping"), FEChartsWidgetJavascript::InteractionModeName(EEChartsInteractionMode::Disabled), FString(TEXT("Disabled")));
	TestEqual(TEXT("ClickOnly mapping"), FEChartsWidgetJavascript::InteractionModeName(EEChartsInteractionMode::ClickOnly), FString(TEXT("ClickOnly")));
	TestEqual(TEXT("FullHover mapping"), FEChartsWidgetJavascript::InteractionModeName(EEChartsInteractionMode::FullHover), FString(TEXT("FullHover")));

	const FString Script = FEChartsWidgetJavascript::BuildRenderCommand(
		EEChartsTemplate::DataTableScatter3D,
		EEChartsInteractionMode::ClickOnly,
		TEXT("{}"));
	TestEqual(
		TEXT("Render command uses the page-local minimal API"),
		Script,
		FString(TEXT("window.UEEChartsHost.renderTemplate(\"DataTableScatter3D\",{},\"ClickOnly\");")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFLocalPageLifecycleTest,
	"EChartsWidget.Integration.CEFLocalPageLifecycle", EChartsWidgetTests::Flags)
bool FEChartsCEFLocalPageLifecycleTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: real CEF page loading requires a rendered Slate browser. Run this test with -d3d12; the D3D12 run is required by verification."));
		return true;
	}

	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Real CEF integration requires an initialized Slate application."));
		return false;
	}

	const TSharedRef<EChartsWidgetTests::FBrowserIntegrationState> State =
		MakeShared<EChartsWidgetTests::FBrowserIntegrationState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartBrowserGenerationCommand(State, this, false));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForBrowserReadyCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartBrowserGenerationCommand(State, this, true));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForBrowserReadyCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartBrowserErrorRecoveryCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForBrowserReadyCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FFinishBrowserIntegrationCommand(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFTemplateRenderingTest,
	"EChartsWidget.Integration.CEFTemplateRendering", EChartsWidgetTests::Flags)
bool FEChartsCEFTemplateRenderingTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: template rendering is covered by the required D3D12 run."));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Real CEF template rendering requires initialized Slate."));
		return false;
	}
	const TSharedRef<EChartsWidgetTests::FBrowserIntegrationState> State = MakeShared<EChartsWidgetTests::FBrowserIntegrationState>();
#define ADD_RENDER_CASE(TemplateValue, EffectiveName, ForceFallback) \
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartTemplateRenderCommand(State, this, TemplateValue, TEXT(EffectiveName), ForceFallback)); \
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForTemplateRenderCommand(State, this))
	ADD_RENDER_CASE(EEChartsTemplate::SegmentedAreaLine, "SegmentedAreaLine", false);
	ADD_RENDER_CASE(EEChartsTemplate::Bar3DHeightMap, "Bar3DHeightMap", false);
	ADD_RENDER_CASE(EEChartsTemplate::DataTableScatter3D, "DataTableScatter3D", false);
	ADD_RENDER_CASE(EEChartsTemplate::Bar3DHeightMap, "Bar3DHeightMap2D", true);
	ADD_RENDER_CASE(EEChartsTemplate::DataTableScatter3D, "DataTableScatter2D", true);
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartTemplateRenderCommand(
		State,
		this,
		EEChartsTemplate::CustomOption,
		TEXT("CustomOption"),
		false,
		TEXT("{\"option\":{\"xAxis\":{\"type\":\"category\",\"data\":[\"A\",\"B\",\"C\"]},\"yAxis\":{\"type\":\"value\"},\"series\":{\"name\":\"Only\",\"type\":\"line\",\"data\":[3,1,4],\"smooth\":true}}}"),
		true));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForTemplateRenderCommand(State, this));
#undef ADD_RENDER_CASE
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FFinishTemplateRenderCommand(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFResizeObserverTest,
	"EChartsWidget.Integration.CEFResizeObserver", EChartsWidgetTests::Flags)
bool FEChartsCEFResizeObserverTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI"))) return true;
	const TSharedRef<EChartsWidgetTests::FBrowserIntegrationState> State = MakeShared<EChartsWidgetTests::FBrowserIntegrationState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartTemplateRenderCommand(State, this, EEChartsTemplate::SegmentedAreaLine, TEXT("SegmentedAreaLine"), false));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForTemplateRenderCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FTriggerBrowserResizeCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForBrowserResizeCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FFinishTemplateRenderCommand(State));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFHeightMapLimitTest,
	"EChartsWidget.Integration.CEFHeightMapLimit", EChartsWidgetTests::Flags)
bool FEChartsCEFHeightMapLimitTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI"))) return true;
	const TSharedRef<EChartsWidgetTests::FBrowserIntegrationState> State = MakeShared<EChartsWidgetTests::FBrowserIntegrationState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FStartHeightMapLimitErrorCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FWaitForHeightMapLimitErrorCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsWidgetTests::FFinishTemplateRenderCommand(State));
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
	FString DecodedHostPath = FGenericPlatformHttp::UrlDecode(HostUrl.RightChop(8));
	FString NormalizedHostPath = HostPath;
	FPaths::NormalizeFilename(DecodedHostPath);
	FPaths::NormalizeFilename(NormalizedHostPath);
	TestEqual(TEXT("Decoded host URL preserves the absolute host path"), DecodedHostPath, NormalizedHostPath);

	FString SyntheticAbsolutePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPlatformMisc::RootDir(),
		TEXT("ECharts URL Tests"),
		TEXT("space # percent% Unicode-数据-Emoji-😀-𠮷"),
		TEXT("chart-host.html")));
	FPaths::NormalizeFilename(SyntheticAbsolutePath);
	const FString SyntheticUrl = FEChartsWidgetResourceLocator::BuildHostPageUrlForPath(SyntheticAbsolutePath);
	TestTrue(TEXT("Synthetic URL uses an absolute file URI"), SyntheticUrl.StartsWith(TEXT("file:///")));
	TestFalse(TEXT("Synthetic URL contains no parent traversal"), SyntheticUrl.Contains(TEXT("../")));
	TestFalse(TEXT("Synthetic URL contains no backslashes"), SyntheticUrl.Contains(TEXT("\\")));
	TestFalse(TEXT("Forward slashes remain URI separators"), SyntheticUrl.Contains(TEXT("%2F"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Drive colon remains a URI separator"), SyntheticUrl.Contains(TEXT("%3A"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("Spaces are percent encoded"), SyntheticUrl.Contains(TEXT("%20")));
	TestTrue(TEXT("Hash is percent encoded"), SyntheticUrl.Contains(TEXT("%23")));
	TestTrue(TEXT("Percent is percent encoded"), SyntheticUrl.Contains(TEXT("%25")));
	TestFalse(TEXT("Unicode is UTF-8 percent encoded"), SyntheticUrl.Contains(TEXT("数据")));
	TestFalse(TEXT("Emoji is UTF-8 percent encoded"), SyntheticUrl.Contains(TEXT("😀")));
	TestFalse(TEXT("Non-BMP CJK is UTF-8 percent encoded"), SyntheticUrl.Contains(TEXT("𠮷")));
	FString DecodedSyntheticPath = FGenericPlatformHttp::UrlDecode(SyntheticUrl.RightChop(8));
	FPaths::NormalizeFilename(DecodedSyntheticPath);
	TestEqual(TEXT("Decoded synthetic URL preserves the complete absolute path"), DecodedSyntheticPath, SyntheticAbsolutePath);

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
	Widget->OnChartRendered.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleRendered);
	Widget->OnEChartsWarning.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleWarning);
	Widget->OnEChartsError.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleError);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_WARNING__:1:local fallback"), TEXT("chart-host.html"), 8);
	TestEqual(TEXT("Warning broadcasts once"), Sink->WarningCount, 1);
	TestEqual(TEXT("Warning strips marker"), Sink->LastWarning, FString(TEXT("local fallback")));
	TestEqual(TEXT("Warning does not change Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	TestEqual(TEXT("LastWarning retained"), Widget->LastWarning, FString(TEXT("local fallback")));

	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), TEXT("chart-host.html"), 9);
	TestEqual(TEXT("Ready broadcasts once"), Sink->ReadyCount, 1);
	TestEqual(TEXT("Ready state"), Widget->RuntimeState, EEChartsRuntimeState::Ready);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_RENDERED__:0:SegmentedAreaLine:SegmentedAreaLine"), TEXT("chart-host.js"), 10);
	TestEqual(TEXT("Stale rendered marker is ignored"), Sink->RenderedCount, 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_RENDERED__:1:SegmentedAreaLine:SegmentedAreaLine"), TEXT("chart-host.js"), 10);
	TestEqual(TEXT("Rendered marker broadcasts once"), Sink->RenderedCount, 1);
	TestEqual(TEXT("Rendered marker preserves requested template"), Sink->LastRequestedTemplate, EEChartsTemplate::SegmentedAreaLine);
	TestEqual(TEXT("Rendered marker stores effective template"), Widget->EffectiveTemplate, FString(TEXT("SegmentedAreaLine")));
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_RENDERED__:1:Bar3DHeightMap:Bar3DHeightMap"), TEXT("chart-host.js"), 10);
	TestEqual(TEXT("Mismatched requested template marker is ignored"), Sink->RenderedCount, 1);

	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:bad option"), TEXT("chart-host.html"), 10);
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
	FString HostJavascript;
	const FString HostPath = FEChartsWidgetResourceLocator::GetChartHostPath();
	TestTrue(TEXT("Host page can be read"), FFileHelper::LoadFileToString(Html, *HostPath));
	TestTrue(TEXT("External host runtime can be read"), FFileHelper::LoadFileToString(
		HostJavascript,
		*FPaths::Combine(FPaths::GetPath(HostPath), TEXT("chart-host.js"))));
	TestTrue(TEXT("CSP defaults to local/data/blob"), Html.Contains(TEXT("default-src 'self' data: blob:")));
	TestTrue(TEXT("CSP disables network connections"), Html.Contains(TEXT("connect-src 'none'")));
	TestFalse(TEXT("No HTTP resources"), Html.Contains(TEXT("http://"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("No HTTPS resources"), Html.Contains(TEXT("https://"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("No protocol-relative resources"), Html.Contains(TEXT("src=\"//"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("Page emits ready marker"), HostJavascript.Contains(TEXT("'READY'")) && HostJavascript.Contains(TEXT("__UE_ECHARTS_")));
	TestTrue(TEXT("Page reads the load generation query"), HostJavascript.Contains(TEXT("URLSearchParams")) && HostJavascript.Contains(TEXT("generation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStagingRulesTest,
	"EChartsWidget.StagingRules", EChartsWidgetTests::Flags)
bool FEChartsStagingRulesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EChartsWidget"));
	TestTrue(TEXT("EChartsWidget plugin is discoverable"), Plugin.IsValid());
	if (!Plugin.IsValid())
	{
		return false;
	}

	FString BuildRules;
	const FString BuildRulesPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/EChartsWidget/EChartsWidget.Build.cs"));
	TestTrue(TEXT("Build rules can be read"), FFileHelper::LoadFileToString(BuildRules, *BuildRulesPath));
	TestTrue(TEXT("Resources/Web files are enumerated for staging"), BuildRules.Contains(TEXT("Resources/Web")));
	TestTrue(TEXT("ThirdPartyLicenses files are enumerated for staging"), BuildRules.Contains(TEXT("ThirdPartyLicenses")));
	TestTrue(TEXT("Runtime dependencies are registered"), BuildRules.Contains(TEXT("RuntimeDependencies.Add")));
	TestTrue(TEXT("Runtime dependencies use NonUFS staging"), BuildRules.Contains(TEXT("StagedFileType.NonUFS")));
	TestTrue(TEXT("Staging target remains plugin-relative"), BuildRules.Contains(TEXT("$(PluginDir)")));
	TestTrue(TEXT("Root LICENSE is explicitly staged as NonUFS"), BuildRules.Contains(
		TEXT("RuntimeDependencies.Add(\"$(PluginDir)/LICENSE\", StagedFileType.NonUFS);")));

	FString FilterRules;
	const FString FilterRulesPath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config/FilterPlugin.ini"));
	TestTrue(TEXT("Plugin filter can be read"), FFileHelper::LoadFileToString(FilterRules, *FilterRulesPath));
	TestTrue(TEXT("Plugin filter includes all third-party licenses"), FilterRules.Contains(TEXT("/ThirdPartyLicenses/...")));
	TestTrue(TEXT("Plugin filter includes the root LICENSE"), FilterRules.Contains(TEXT("/LICENSE")));
	TestTrue(TEXT("Apache ECharts NOTICE is present for staging"), IFileManager::Get().FileExists(
		*FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdPartyLicenses/ECharts-NOTICE.txt"))));
	TestTrue(TEXT("D3 BSD license is present for staging"), IFileManager::Get().FileExists(
		*FPaths::Combine(Plugin->GetBaseDir(), TEXT("ThirdPartyLicenses/ECharts-LICENSE-d3.txt"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsErrorTerminalStateTest,
	"EChartsWidget.ErrorTerminalState", EChartsWidgetTests::Flags)
bool FEChartsErrorTerminalStateTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	UEChartsWidgetTestSink* Sink = NewObject<UEChartsWidgetTestSink>();
	Sink->AddToRoot();
	Widget->OnChartReady.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleReady);
	Widget->OnEChartsError.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleError);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:first failure"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestEqual(TEXT("Error broadcasts once"), Sink->ErrorCount, 1);
	TestEqual(TEXT("Ready after Error is ignored"), Sink->ReadyCount, 0);
	TestEqual(TEXT("Error is terminal for its generation"), Widget->RuntimeState, EEChartsRuntimeState::Error);
	TestEqual(TEXT("Terminal error is retained"), Widget->LastError, FString(TEXT("first failure")));

	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::FullHover);
	TestEqual(TEXT("Reinitialize returns to Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	TestTrue(TEXT("Reinitialize clears LastError"), Widget->LastError.IsEmpty());
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:stale failure"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	TestEqual(TEXT("Stale Error does not rebroadcast"), Sink->ErrorCount, 1);
	TestEqual(TEXT("New generation can become Ready"), Sink->ReadyCount, 1);
	TestEqual(TEXT("New generation reaches Ready"), Widget->RuntimeState, EEChartsRuntimeState::Ready);

	Sink->RemoveFromRoot();
	EChartsWidgetTests::DestroyWidget(Widget);
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
	Widget->OnEChartsError.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleError);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap, EEChartsInteractionMode::FullHover);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:1:stale"), FString(), 0);
	TestEqual(TEXT("Old generation Ready is ignored"), Sink->ReadyCount, 0);
	TestEqual(TEXT("Old generation Error is ignored"), Sink->ErrorCount, 0);
	TestEqual(TEXT("Old generation messages leave current load Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	TestEqual(TEXT("Duplicate marker broadcasts once per load"), Sink->ReadyCount, 1);

	Widget->ReleaseSlateResources(false);
	TestEqual(TEXT("Release resets runtime state"), Widget->RuntimeState, EEChartsRuntimeState::Uninitialized);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	TestEqual(TEXT("Released widget ignores marker"), Sink->ReadyCount, 1);

	Widget->RebindConsoleMessageForTesting();
	Widget->RebindConsoleMessageForTesting();
	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::Disabled);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:3"), FString(), 0);
	TestEqual(TEXT("Rebinding twice does not duplicate callbacks"), Sink->ReadyCount, 2);

	Sink->RemoveFromRoot();
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAutomaticRebuildGenerationTest,
	"EChartsWidget.AutomaticRebuildGeneration", EChartsWidgetTests::Flags)
bool FEChartsAutomaticRebuildGenerationTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsWidgetTests::MakeWidget();
	UEChartsWidgetTestSink* Sink = NewObject<UEChartsWidgetTestSink>();
	Sink->AddToRoot();
	Widget->OnChartReady.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleReady);
	Widget->OnEChartsError.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleError);

	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestEqual(TEXT("Initial generation becomes Ready once"), Sink->ReadyCount, 1);
	Widget->ReleaseSlateResources(false);
	Widget->PrepareRebuildForTesting();
	TestEqual(TEXT("Automatic rebuild starts Loading"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestEqual(TEXT("Old Ready remains ignored after rebuild"), Sink->ReadyCount, 1);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	TestEqual(TEXT("Automatic rebuild generation broadcasts Ready once"), Sink->ReadyCount, 2);

	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::FullHover);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:3:terminal"), FString(), 0);
	TestEqual(TEXT("Current generation enters terminal Error"), Widget->RuntimeState, EEChartsRuntimeState::Error);
	Widget->ReleaseSlateResources(false);
	Widget->PrepareRebuildForTesting();
	TestEqual(TEXT("Rebuild after Error starts a new Loading generation"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	TestTrue(TEXT("Rebuild after Error clears LastError"), Widget->LastError.IsEmpty());
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:3"), FString(), 0);
	TestEqual(TEXT("Errored generation cannot recover after rebuild"), Sink->ReadyCount, 2);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:4"), FString(), 0);
	TestEqual(TEXT("New rebuild generation can recover from prior Error"), Sink->ReadyCount, 3);

	Sink->RemoveFromRoot();
	EChartsWidgetTests::DestroyWidget(Widget);
	return true;
}

#endif
