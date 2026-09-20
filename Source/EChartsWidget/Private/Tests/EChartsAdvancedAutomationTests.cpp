#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EChartsWidget.h"
#include "EChartsWidgetTestSink.h"

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"

namespace EChartsAdvancedTests
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

	struct FAdvancedCEFState
	{
		UEChartsWidget* Widget = nullptr;
		UEChartsWidgetTestSink* Sink = nullptr;
		TSharedPtr<SWidget> SlateWidget;
		double Deadline = 0.0;
		int32 Stage = 0;
		int32 ExpectedOptionResults = 0;
		int32 ExpectedInteractionResults = 0;
		int32 ExpectedJavaScriptResults = 0;
		int32 ExpectedProbes = 0;
		int64 RawRequestId = 0;

		void Cleanup()
		{
			SlateWidget.Reset();
			if (Widget) { DestroyWidget(Widget); Widget = nullptr; }
			if (Sink) { Sink->RemoveFromRoot(); Sink = nullptr; }
		}
	};

	class FAdvancedCEFCommand final : public IAutomationLatentCommand
	{
	public:
		FAdvancedCEFCommand(const TSharedRef<FAdvancedCEFState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (State->Stage == 0)
			{
				State->Widget = MakeWidget();
				State->Sink = NewObject<UEChartsWidgetTestSink>();
				State->Sink->AddToRoot();
				State->Widget->OnOptionApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleOptionApplied);
				State->Widget->OnInteractionModeApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleInteractionModeApplied);
				State->Widget->OnJavaScriptResult.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleJavaScriptResult);
				State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
				State->SlateWidget = State->Widget->TakeWidget();
				State->Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
				Advance();
				return false;
			}

			if (State->Stage == 1 && State->Widget->RuntimeState == EEChartsRuntimeState::Ready)
			{
				const FString Option = TEXT("{\"title\":{\"text\":\"</script> \\\"quoted\\\" \\\\ slash 数据 😀\"},\"tooltip\":{},\"legend\":{},\"xAxis\":{\"type\":\"category\",\"data\":[\"A\",\"B\"]},\"yAxis\":{\"type\":\"value\"},\"series\":[{\"type\":\"line\",\"data\":[1,2]}]}");
				Test->TestTrue(TEXT("CEF accepts hostile CustomOption JSON"), State->Widget->SetEChartsOptionJSON(Option));
				State->ExpectedOptionResults = State->Sink->OptionResultCount + 1;
				Advance();
				return false;
			}
			if (State->Stage == 2 && State->Sink->OptionResultCount >= State->ExpectedOptionResults)
			{
				Test->TestTrue(TEXT("CEF applied hostile CustomOption"), State->Sink->bLastOptionSuccess);
				Probe(TEXT("OPTION"), TEXT("o.title[0].text.indexOf('</script>')===0&&o.title[0].text.indexOf('数据')>=0"));
				Advance();
				return false;
			}
			if (State->Stage == 3 && ProbeSucceeded(TEXT("OPTION")))
			{
				State->ExpectedInteractionResults = State->Sink->InteractionResultCount + 1;
				State->Widget->SetInteractionMode(EEChartsInteractionMode::Disabled);
				Advance();
				return false;
			}
			if (State->Stage == 4 && State->Sink->InteractionResultCount >= State->ExpectedInteractionResults)
			{
				Test->TestTrue(TEXT("CEF applied runtime Disabled interaction"), State->Sink->bLastInteractionSuccess);
				Probe(TEXT("INTERACTION"), TEXT("o.tooltip[0].triggerOn==='none'&&o.series[0].silent===true"));
				Advance();
				return false;
			}
			if (State->Stage == 5 && ProbeSucceeded(TEXT("INTERACTION")))
			{
				State->Widget->SetInteractionMode(EEChartsInteractionMode::FullHover);
				State->ExpectedInteractionResults = State->Sink->InteractionResultCount + 1;
				Advance();
				return false;
			}
			if (State->Stage == 6 && State->Sink->InteractionResultCount >= State->ExpectedInteractionResults)
			{
				State->ExpectedJavaScriptResults = State->Sink->JavaScriptResultCount + 1;
				Test->TestTrue(TEXT("CEF queues raw JavaScript chart mutation"), State->Widget->ExecuteEChartsJavaScript(
					TEXT("chart.setOption({title:{text:'raw-modified'}});"), State->RawRequestId));
				Advance();
				return false;
			}
			if (State->Stage == 7 && State->Sink->JavaScriptResultCount >= State->ExpectedJavaScriptResults)
			{
				Test->TestTrue(TEXT("CEF ACKs successful raw JavaScript"), State->Sink->bLastJavaScriptSuccess);
				Test->TestEqual(TEXT("CEF raw JavaScript ACK request id"), State->Sink->LastJavaScriptRequestId, State->RawRequestId);
				Probe(TEXT("RAW"), TEXT("o.title[0].text==='raw-modified'"));
				Advance();
				return false;
			}
			if (State->Stage == 8 && ProbeSucceeded(TEXT("RAW")))
			{
				State->ExpectedJavaScriptResults = State->Sink->JavaScriptResultCount + 1;
				Test->TestTrue(TEXT("CEF queues throwing raw JavaScript"), State->Widget->ExecuteEChartsJavaScript(
					TEXT("throw new Error('recoverable raw failure');"), State->RawRequestId));
				Advance();
				return false;
			}
			if (State->Stage == 9 && State->Sink->JavaScriptResultCount >= State->ExpectedJavaScriptResults)
			{
				Test->TestFalse(TEXT("CEF reports raw JavaScript exception"), State->Sink->bLastJavaScriptSuccess);
				Test->TestEqual(TEXT("Raw JavaScript exception is non-terminal"), State->Widget->RuntimeState, EEChartsRuntimeState::Ready);
				State->ExpectedJavaScriptResults = State->Sink->JavaScriptResultCount + 1;
				Test->TestTrue(TEXT("CEF accepts raw JavaScript after exception"), State->Widget->ExecuteEChartsJavaScript(
					TEXT("chart.resize();"), State->RawRequestId));
				Advance();
				return false;
			}
			if (State->Stage == 10 && State->Sink->JavaScriptResultCount >= State->ExpectedJavaScriptResults)
			{
				Test->TestTrue(TEXT("CEF recovers after raw JavaScript exception"), State->Sink->bLastJavaScriptSuccess);
				State->ExpectedOptionResults = State->Sink->OptionResultCount + 1;
				State->ExpectedInteractionResults = State->Sink->InteractionResultCount + 1;
				State->SlateWidget.Reset();
				State->Widget->ReleaseSlateResources(false);
				State->SlateWidget = State->Widget->TakeWidget();
				Advance();
				return false;
			}
			if (State->Stage == 11 && State->Widget->RuntimeState == EEChartsRuntimeState::Ready &&
				State->Sink->OptionResultCount >= State->ExpectedOptionResults &&
				State->Sink->InteractionResultCount >= State->ExpectedInteractionResults)
			{
				Test->TestTrue(TEXT("CEF rebuild replayed cached option"), State->Sink->bLastOptionSuccess);
				Test->TestEqual(TEXT("CEF rebuild retained FullHover"), State->Widget->InteractionMode, EEChartsInteractionMode::FullHover);
				Probe(TEXT("REBUILD"), TEXT("o.title[0].text.indexOf('</script>')===0&&o.title[0].text!=='raw-modified'&&o.tooltip[0].triggerOn==='mousemove|click'&&o.series[0].silent===false"));
				Advance();
				return false;
			}
			if (State->Stage == 12 && ProbeSucceeded(TEXT("REBUILD")))
			{
				State->Cleanup();
				return true;
			}

			if (FPlatformTime::Seconds() >= State->Deadline)
			{
				Test->AddError(FString::Printf(TEXT("Timed out in advanced CEF stage %d; state=%d error=%s"),
					State->Stage, static_cast<int32>(State->Widget->RuntimeState), *State->Widget->LastError));
				State->Cleanup();
				return true;
			}
			return false;
		}

	private:
		void Advance()
		{
			++State->Stage;
			State->Deadline = FPlatformTime::Seconds() + 30.0;
		}

		void Probe(const FString& Name, const FString& Condition)
		{
			State->ExpectedProbes = State->Sink->AdvancedProbeCount + 1;
			State->Widget->ExecuteJavascript(FString::Printf(TEXT(
				"(function(){var o=window.UEEChartsHost.getOptionForTesting();console.log('__UE_ECHARTS_TEST_ADVANCED__:%s:'+((%s)?'OK':'BAD'));}());"),
				*Name, *Condition));
		}

		bool ProbeSucceeded(const FString& Name)
		{
			if (State->Sink->AdvancedProbeCount < State->ExpectedProbes) return false;
			const FString Expected = Name + TEXT(":OK");
			Test->TestEqual(*FString::Printf(TEXT("CEF %s probe"), *Name), State->Sink->LastAdvancedProbe, Expected);
			return true;
		}

		TSharedRef<FAdvancedCEFState> State;
		FAutomationTestBase* Test;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAdvancedReflectionTest,
	"EChartsWidget.Advanced.Reflection", EChartsAdvancedTests::Flags)
bool FEChartsAdvancedReflectionTest::RunTest(const FString& Parameters)
{
	UClass* Class = UEChartsWidget::StaticClass();
	const UFunction* SetInteraction = Class->FindFunctionByName(TEXT("SetInteractionMode"));
	const UFunction* SetOption = Class->FindFunctionByName(TEXT("SetEChartsOptionJSON"));
	const UFunction* Execute = Class->FindFunctionByName(TEXT("ExecuteEChartsJavaScript"));
	TestNotNull(TEXT("Set Interaction Mode Blueprint node exists"), SetInteraction);
	TestNotNull(TEXT("Set ECharts Option JSON Blueprint node exists"), SetOption);
	TestNotNull(TEXT("Execute ECharts JavaScript Blueprint node exists"), Execute);
	if (SetInteraction) TestEqual(TEXT("Interaction node display name"), SetInteraction->GetMetaData(TEXT("DisplayName")), FString(TEXT("Set Interaction Mode")));
	if (SetOption) TestEqual(TEXT("Option node display name"), SetOption->GetMetaData(TEXT("DisplayName")), FString(TEXT("Set ECharts Option JSON")));
	if (Execute) TestEqual(TEXT("JavaScript node display name"), Execute->GetMetaData(TEXT("DisplayName")), FString(TEXT("Execute ECharts JavaScript")));
	TestNotNull(TEXT("Option result event reflected"), Class->FindPropertyByName(TEXT("OnOptionApplied")));
	TestNotNull(TEXT("Interaction result event reflected"), Class->FindPropertyByName(TEXT("OnInteractionModeApplied")));
	TestNotNull(TEXT("JavaScript result event reflected"), Class->FindPropertyByName(TEXT("OnJavaScriptResult")));

	UEChartsWidget* Widget = EChartsAdvancedTests::MakeWidget();
	TestEqual(TEXT("Advanced interaction default remains ClickOnly"), Widget->InteractionMode, EEChartsInteractionMode::ClickOnly);
	TestTrue(TEXT("No custom option is cached by default"), Widget->GetCachedOptionBase64ForTesting().IsEmpty());
	TestEqual(TEXT("No advanced requests are pending by default"), Widget->GetPendingAdvancedRequestCountForTesting(), 0);
	EChartsAdvancedTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAdvancedValidationAndCommandTest,
	"EChartsWidget.Advanced.ValidationAndCommands", EChartsAdvancedTests::Flags)
bool FEChartsAdvancedValidationAndCommandTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsAdvancedTests::MakeWidget();
	TestFalse(TEXT("Empty option is rejected"), Widget->SetEChartsOptionJSON(TEXT("")));
	TestFalse(TEXT("Whitespace option is rejected"), Widget->SetEChartsOptionJSON(TEXT("  \r\n")));
	TestFalse(TEXT("Malformed option is rejected"), Widget->SetEChartsOptionJSON(TEXT("{bad")));
	TestFalse(TEXT("Top-level array option is rejected"), Widget->SetEChartsOptionJSON(TEXT("[]")));
	TestEqual(TEXT("Invalid option does not change template"), Widget->CurrentTemplate, EEChartsTemplate::SegmentedAreaLine);

	const FString Hostile = TEXT("{\"title\":{\"text\":\"</script> \\\"quote\\\" \\\\ slash 数据 😀\"},\"series\":[{\"type\":\"line\",\"data\":[1]}]}");
	TestTrue(TEXT("Valid hostile option is accepted"), Widget->SetEChartsOptionJSON(Hostile));
	TestEqual(TEXT("Valid option selects CustomOption"), Widget->CurrentTemplate, EEChartsTemplate::CustomOption);
	const FString Cached = Widget->GetCachedOptionBase64ForTesting();
	TestFalse(TEXT("Cached option is Base64, not raw JSON"), Cached.Contains(TEXT("</script>")));
	TestTrue(TEXT("Cached option is non-empty"), !Cached.IsEmpty());
	TestFalse(TEXT("Oversized UTF-8 option is rejected"), Widget->SetEChartsOptionJSON(
		FString(TEXT("{\"x\":\"")) + FString::ChrN(16 * 1024 * 1024 + 1, TEXT('a')) + TEXT("\"}")));
	TestEqual(TEXT("Rejected option preserves prior cache"), Widget->GetCachedOptionBase64ForTesting(), Cached);

	const FString OptionCommand = FEChartsWidgetJavascript::BuildApplyOptionCommand(41, Cached);
	TestTrue(TEXT("Option command uses fixed Host API"), OptionCommand.StartsWith(TEXT("window.UEEChartsHost.applyOptionBase64(41,\"")));
	TestFalse(TEXT("Option command excludes raw hostile content"), OptionCommand.Contains(TEXT("</script>")));
	TestTrue(TEXT("Unsafe option payload cannot enter command"), FEChartsWidgetJavascript::BuildApplyOptionCommand(41, TEXT("x\");alert(1)//")).IsEmpty());
	TestTrue(TEXT("Zero request id cannot enter option command"), FEChartsWidgetJavascript::BuildApplyOptionCommand(0, Cached).IsEmpty());
	TestEqual(TEXT("Interaction command is enum constrained"),
		FEChartsWidgetJavascript::BuildSetInteractionModeCommand(42, EEChartsInteractionMode::FullHover),
		FString(TEXT("window.UEEChartsHost.setInteractionMode(42,\"FullHover\");")));

	int64 RequestId = -1;
	TestFalse(TEXT("Raw JavaScript is rejected before Ready"), Widget->ExecuteEChartsJavaScript(TEXT("chart.resize();"), RequestId));
	TestEqual(TEXT("Rejected raw JavaScript has no request id"), RequestId, int64(0));
	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestFalse(TEXT("Whitespace JavaScript is rejected"), Widget->ExecuteEChartsJavaScript(TEXT(" \r\n"), RequestId));
	TestFalse(TEXT("Oversized JavaScript is rejected"), Widget->ExecuteEChartsJavaScript(FString::ChrN(1024 * 1024 + 1, TEXT('a')), RequestId));
	TestTrue(TEXT("Ready raw JavaScript is queued"), Widget->ExecuteEChartsJavaScript(TEXT("chart.setOption({title:{text:'数据 😀'}});"), RequestId));
	TestTrue(TEXT("Accepted JavaScript has a positive request id"), RequestId > 0);
	const FString JavaScriptCommand = FEChartsWidgetJavascript::BuildExecuteJavaScriptCommand(RequestId, TEXT("Y2hhcnQucmVzaXplKCk7"));
	TestTrue(TEXT("JavaScript command uses fixed Host API"), JavaScriptCommand.StartsWith(TEXT("window.UEEChartsHost.executeJavaScriptBase64(")));
	TestTrue(TEXT("Unsafe JavaScript payload cannot enter command"), FEChartsWidgetJavascript::BuildExecuteJavaScriptCommand(RequestId, TEXT("bad;alert(1)")).IsEmpty());
	EChartsAdvancedTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAdvancedStateMachineTest,
	"EChartsWidget.Advanced.StateMachine", EChartsAdvancedTests::Flags)
bool FEChartsAdvancedStateMachineTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsAdvancedTests::MakeWidget();
	UEChartsWidgetTestSink* Sink = NewObject<UEChartsWidgetTestSink>();
	Sink->AddToRoot();
	Widget->OnOptionApplied.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleOptionApplied);
	Widget->OnInteractionModeApplied.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleInteractionModeApplied);
	Widget->OnJavaScriptResult.AddDynamic(Sink, &UEChartsWidgetTestSink::HandleJavaScriptResult);

	TestTrue(TEXT("Loading option is safely cached"), Widget->SetEChartsOptionJSON(TEXT("{\"series\":[{\"type\":\"line\",\"data\":[1]}]}")));
	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::ClickOnly);
	Widget->SetInteractionMode(EEChartsInteractionMode::Disabled);
	TestEqual(TEXT("Pre-Ready interaction is cached"), Widget->InteractionMode, EEChartsInteractionMode::Disabled);
	TestEqual(TEXT("Pre-Ready requests are not sent"), Widget->GetPendingAdvancedRequestCountForTesting(), 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestEqual(TEXT("Ready replays cached option and interaction"), Widget->GetPendingAdvancedRequestCountForTesting(), 2);

	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_OPTION_RESULT__:0:1:1:stale"), FString(), 0);
	TestEqual(TEXT("Old-generation option result is ignored"), Sink->OptionResultCount, 0);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_OPTION_RESULT__:1:999:1:unknown"), FString(), 0);
	TestEqual(TEXT("Unknown request id is ignored"), Sink->OptionResultCount, 0);

	const int64 OptionRequest = Widget->GetPendingOptionRequestIdForTesting();
	const int64 InteractionRequest = Widget->GetPendingInteractionRequestIdForTesting();
	Widget->OnConsoleMessage.Broadcast(FString::Printf(TEXT("__UE_ECHARTS_OPTION_RESULT__:1:%lld:0:setOption failed"), OptionRequest), FString(), 0);
	TestEqual(TEXT("Option failure has a dedicated result"), Sink->OptionResultCount, 1);
	TestFalse(TEXT("Option failure reports false"), Sink->bLastOptionSuccess);
	TestEqual(TEXT("Option failure is non-terminal"), Widget->RuntimeState, EEChartsRuntimeState::Ready);
	Widget->OnConsoleMessage.Broadcast(FString::Printf(TEXT("__UE_ECHARTS_INTERACTION_RESULT__:1:%lld:1:Disabled"), InteractionRequest), FString(), 0);
	TestEqual(TEXT("Interaction ACK has a dedicated result"), Sink->InteractionResultCount, 1);
	TestTrue(TEXT("Interaction ACK reports success"), Sink->bLastInteractionSuccess);

	int64 JavaScriptRequest = 0;
	TestTrue(TEXT("Raw JavaScript can be retried after option error"), Widget->ExecuteEChartsJavaScript(TEXT("throw new Error('x')"), JavaScriptRequest));
	Widget->OnConsoleMessage.Broadcast(FString::Printf(TEXT("__UE_ECHARTS_JAVASCRIPT_RESULT__:1:%lld:0:raw failure: detail"), JavaScriptRequest), FString(), 0);
	TestEqual(TEXT("JavaScript error has a dedicated result"), Sink->JavaScriptResultCount, 1);
	TestEqual(TEXT("JavaScript result preserves colon detail"), Sink->LastAdvancedMessage, FString(TEXT("raw failure: detail")));
	TestEqual(TEXT("JavaScript error is non-terminal"), Widget->RuntimeState, EEChartsRuntimeState::Ready);

	const FString Cached = Widget->GetCachedOptionBase64ForTesting();
	Widget->ReleaseSlateResources(false);
	TestEqual(TEXT("Release clears pending advanced callbacks"), Widget->GetPendingAdvancedRequestCountForTesting(), 0);
	Widget->PrepareRebuildForTesting();
	TestEqual(TEXT("Rebuild starts a new loading generation"), Widget->RuntimeState, EEChartsRuntimeState::Loading);
	TestEqual(TEXT("Rebuild retains interaction mode"), Widget->InteractionMode, EEChartsInteractionMode::Disabled);
	TestEqual(TEXT("Rebuild retains custom option"), Widget->GetCachedOptionBase64ForTesting(), Cached);
	Widget->OnConsoleMessage.Broadcast(FString::Printf(TEXT("__UE_ECHARTS_JAVASCRIPT_RESULT__:1:%lld:1:late"), JavaScriptRequest), FString(), 0);
	TestEqual(TEXT("Old-generation JavaScript result is ignored"), Sink->JavaScriptResultCount, 1);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	TestEqual(TEXT("Rebuild replays option and interaction only"), Widget->GetPendingAdvancedRequestCountForTesting(), 2);

	Widget->ReleaseSlateResources(false);
	Widget->RemoveFromRoot();
	Sink->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCustomOptionDataContinuityTest,
	"EChartsWidget.Advanced.CustomOptionDataContinuity", EChartsAdvancedTests::Flags)
bool FEChartsCustomOptionDataContinuityTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsAdvancedTests::MakeWidget();
	TArray<FEChartsDataPoint2D> Initial = {{1.0, 2.0}, {3.0, 4.0}};
	TestTrue(TEXT("Series data is accepted before CustomOption"), Widget->SetSeriesData(0, Initial));
	TestTrue(TEXT("CustomOption is accepted without clearing series cache"), Widget->SetEChartsOptionJSON(
		TEXT("{\"xAxis\":{},\"yAxis\":{},\"series\":[{\"type\":\"line\",\"data\":[]}]}")));
	TestEqual(TEXT("CustomOption preserves cached series points"), Widget->GetSeriesData(0).Num(), 2);
	Widget->InitializeECharts(EEChartsTemplate::CustomOption, EEChartsInteractionMode::ClickOnly);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestTrue(TEXT("Cached data is safely submitted after CustomOption replay"), Widget->IsApplyInFlightForTesting());
	Widget->AcknowledgeCurrentApplyForTesting();
	TestTrue(TEXT("Add remains usable after CustomOption"), Widget->AddDataPoint(0, 5.0, 6.0));
	Widget->ApplyEChartsChanges();
	TestTrue(TEXT("Apply remains usable after CustomOption"), Widget->IsApplyInFlightForTesting());
	EChartsAdvancedTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAdvancedCEFIntegrationTest,
	"EChartsWidget.Advanced.Integration.CEF", EChartsAdvancedTests::Flags)
bool FEChartsAdvancedCEFIntegrationTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Advanced real CEF coverage requires D3D12."));
		return true;
	}
	const TSharedRef<EChartsAdvancedTests::FAdvancedCEFState> State = MakeShared<EChartsAdvancedTests::FAdvancedCEFState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsAdvancedTests::FAdvancedCEFCommand(State, this));
	return true;
}

#endif
