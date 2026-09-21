#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EChartsPayloadBuilder.h"
#include "EChartsWidget.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"

#include "EChartsWidgetTestSink.h"

#include <limits>

namespace EChartsDataTests
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

	struct FDataBrowserState
	{
		UEChartsWidget* Widget = nullptr;
		UEChartsWidgetTestSink* Sink = nullptr;
		TSharedPtr<SWidget> SlateWidget;
		double DeadlineSeconds = 0.0;
		bool bFailed = false;

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

	class FStartDataBrowserCommand : public IAutomationLatentCommand
	{
	public:
		FStartDataBrowserCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnEChartsApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleApplied);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
			State->SlateWidget = State->Widget->TakeWidget();

			TArray<FEChartsCategoryDataPoint> First = {{TEXT("A"), 1.0}, {TEXT("B"), 2.0}};
			TArray<FEChartsCategoryDataPoint> Second = {{TEXT("B"), 3.0}, {TEXT("A"), 4.0}};
			State->Widget->SetCategorySeriesData(0, First);
			State->Widget->SetCategorySeriesData(1, Second);
			State->Widget->SetXAxisMode(EEChartsXAxisMode::Category);
			State->Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
			State->Widget->ApplyEChartsChanges();
			State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
			return true;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FWaitForDataAppliedCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForDataAppliedCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("CEF data apply failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount > 0)
			{
				Test->TestEqual(TEXT("Real CEF emitted one APPLIED marker"), State->Sink->AppliedCount, 1);
				Test->TestEqual(TEXT("Real CEF APPLIED point count"), State->Sink->LastAppliedPointCount, 4);
				Test->TestEqual(TEXT("Real CEF APPLIED revision"), State->Sink->LastAppliedRevision, int64(3));
				State->Widget->ExecuteJavascript(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();"
					"var x=Array.isArray(o.xAxis)?o.xAxis[0]:o.xAxis;"
					"var ok=x.type==='category'&&JSON.stringify(x.data)===JSON.stringify(['A','B'])&&"
					"JSON.stringify(o.series[0].data)===JSON.stringify([1,2])&&"
					"JSON.stringify(o.series[1].data)===JSON.stringify([4,3]);"
					"console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"));
				State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for real CEF APPLIED marker."));
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FStart3DTemplateCommand : public IAutomationLatentCommand
	{
	public:
		FStart3DTemplateCommand(
			const TSharedRef<FDataBrowserState>& InState,
			const EEChartsTemplate InTemplate,
			const bool bInForceFallback)
			: State(InState), Template(InTemplate), bForceFallback(bInForceFallback) {}

		virtual bool Update() override
		{
			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnEChartsApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleApplied);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
			State->Widget->SetForceWebGLUnavailableForTesting(bForceFallback);
			State->SlateWidget = State->Widget->TakeWidget();
			TArray<FEChartsDataPoint3D> Data3D = {
				{10.5, 200.0, 3.0, 10.0, 5.0},
				{11.5, 201.0, 6.0, 100.0, 8.0}
			};
			State->Widget->Set3DData(0, Data3D);
			State->Widget->InitializeECharts(Template, EEChartsInteractionMode::ClickOnly);
			State->Widget->ApplyEChartsChanges();
			State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
			return true;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		EEChartsTemplate Template;
		bool bForceFallback;
	};

	class FWaitFor3DTemplateAppliedCommand : public IAutomationLatentCommand
	{
	public:
		FWaitFor3DTemplateAppliedCommand(
			const TSharedRef<FDataBrowserState>& InState,
			FAutomationTestBase* InTest,
			const FString& InExpectedType)
			: State(InState), Test(InTest), ExpectedType(InExpectedType) {}

		virtual bool Update() override
		{
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("CEF 3D data apply failed for %s: %s"), *ExpectedType, *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount > 0)
			{
				Test->TestEqual(TEXT("3D CEF APPLIED point count"), State->Sink->LastAppliedPointCount, 2);
				Test->TestEqual(TEXT("3D CEF APPLIED revision"), State->Sink->LastAppliedRevision, int64(1));
				const bool bExpect3D = ExpectedType.EndsWith(TEXT("3D"));
				const bool bExpectHeatmap = ExpectedType == TEXT("heatmap");
				const FString DataCheck = bExpectHeatmap
					? TEXT("JSON.stringify(s.data[0])===JSON.stringify([0,0,3])&&JSON.stringify(s.ueOriginalData[0])===JSON.stringify([10.5,200,3,10,5])")
					: TEXT("JSON.stringify(s.data[0])===JSON.stringify([10.5,200,3,10,5])&&s.data[1][3]===100");
				const bool bExpectBar3D = ExpectedType == TEXT("bar3D");
				const FString CoordinateCheck = bExpectHeatmap
					? TEXT("o.xAxis[0].type==='category'&&o.yAxis[0].type==='category'&&o.xAxis[0].data[0]===10.5&&o.yAxis[0].data[0]===200")
					: (bExpectBar3D
						? TEXT("hasGrid&&o.xAxis3D[0].type==='value'&&o.yAxis3D[0].type==='value'&&o.zAxis3D[0].type==='value'&&window.UEEChartsHost.getSeriesCoordinateStatsForTesting([10.5,200,3]).allFinite")
						: (bExpect3D ? TEXT("hasGrid") : TEXT("!hasGrid&& !/3D$/.test(s.type)")));
				const FString GraphicCheck = bExpectHeatmap
					? TEXT("(function(){var g=window.UEEChartsHost.getGraphicShapeStatsForTesting();return g.heatmapRectCount>0&&g.allFinite;}())")
					: (bExpect3D
						? TEXT("!o.xAxis&&!o.yAxis&&!o.grid&&o.visualMap[0].dimension===3&&o.visualMap[0].min===10&&o.visualMap[0].max===100&&(o.visualMap[0].seriesIndex===0||o.visualMap[0].seriesIndex[0]===0)")
						: TEXT("true"));
				State->Widget->ExecuteJavascript(FString::Printf(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();var s=o.series[0];"
					"var hasGrid=!!o.grid3D;var ok=s.type==='%s'&&%s&&%s&&%s;"
					"console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"),
					*ExpectedType,
					*DataCheck,
					*CoordinateCheck,
					*GraphicCheck));
				State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Timed out waiting for %s APPLIED marker."), *ExpectedType));
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
		FString ExpectedType;
	};

	class FWaitForDataProbeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForDataProbeCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (State->bFailed || State->Sink->DataOptionReportCount > 0)
			{
				if (!State->bFailed) Test->TestTrue(TEXT("CEF getOption preserves mapped data semantics"), State->Sink->bLastDataOptionSucceeded);
				State->Cleanup();
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				Test->AddError(TEXT("Timed out waiting for CEF getOption data probe."));
				State->Cleanup();
				return true;
			}
			return false;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FNative3DStateRegressionCommand final : public IAutomationLatentCommand
	{
	public:
		FNative3DStateRegressionCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (Stage == 0)
			{
				State->Widget = MakeWidget();
				State->Sink = NewObject<UEChartsWidgetTestSink>(); State->Sink->AddToRoot();
				State->Widget->OnEChartsApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleApplied);
				State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
				State->Widget->OnJavaScriptResult.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleJavaScriptResult);
				State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
				FEChartsLegendSettings Legend;
				Legend.Position = EEChartsLegendPosition::Right;
				Legend.Orientation = EEChartsLegendOrientation::Vertical;
				Legend.FontSize = 18;
				Legend.ItemGap = 22;
				State->Widget->SetLegendSettings(Legend);
				State->SlateWidget = State->Widget->TakeWidget();
				State->Widget->Set3DData(0, {{1.0, 2.0, 3.0, 10.0, 7.0}, {2.0, 3.0, 4.0, 100.0, 9.0}});
				State->Widget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap, EEChartsInteractionMode::ClickOnly);
				State->Widget->ApplyEChartsChanges();
				State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
				Stage = 1;
				return false;
			}
			if (State->Sink->ErrorCount > 0)
			{
				Test->AddError(FString::Printf(TEXT("Native 3D state regression host error: %s"), *State->Sink->LastError));
				State->Cleanup(); return true;
			}
			if (Stage == 1 && State->Sink->AppliedCount >= 1)
			{
				int64 RequestId = 0;
				if (!State->Widget->ExecuteEChartsJavaScript(
					TEXT("if(!host.setViewControlForTesting(17,23,180)) throw new Error('camera helper failed');"), RequestId))
				{
					Test->AddError(TEXT("Could not dispatch deterministic CEF camera setup.")); State->Cleanup(); return true;
				}
				Stage = 2; State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0; return false;
			}
			if (Stage == 2 && State->Sink->JavaScriptResultCount >= 1)
			{
				State->Widget->Set3DData(0, {{3.0, 4.0, 5.0, 20.0, 8.0}, {4.0, 5.0, 6.0, 100.0, 10.0}});
				State->Widget->ApplyEChartsChanges();
				Stage = 3; State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0; return false;
			}
			if (Stage == 3 && State->Sink->AppliedCount >= 2)
			{
				int64 RequestId = 0;
				const FString Probe = TEXT(
					"(function(){var o=host.getOptionForTesting(),v=o.grid3D[0].viewControl,vm=o.visualMap[0],l=o.legend[0],s=o.series[0];"
					"var si=vm.seriesIndex;var ok=v.alpha===17&&v.beta===23&&v.distance===180&&vm.dimension===3&&vm.min===20&&vm.max===100&&"
					"(si===0||si[0]===0)&&s.data[1][3]===100&&!o.xAxis&&!o.yAxis&&!o.grid&&l.right==='2%'&&l.orient==='vertical'&&"
					"l.textStyle.fontSize===18&&l.itemGap===22;console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());");
				if (!State->Widget->ExecuteEChartsJavaScript(Probe, RequestId))
				{
					Test->AddError(TEXT("Could not dispatch native 3D CEF state probe.")); State->Cleanup(); return true;
				}
				Stage = 4; State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0; return false;
			}
			if (Stage == 4 && State->Sink->DataOptionReportCount > 0)
			{
				Test->TestTrue(TEXT("Real CEF preserves camera and applies ColorValue/legend semantics"), State->Sink->bLastDataOptionSucceeded);
				State->Cleanup(); return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				Test->AddError(FString::Printf(TEXT("Timed out in native 3D state regression stage %d."), Stage));
				State->Cleanup(); return true;
			}
			return false;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
		int32 Stage = 0;
	};

	class FStartCacheReplayCommand : public IAutomationLatentCommand
	{
	public:
		FStartCacheReplayCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnEChartsApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleApplied);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
			State->SlateWidget = State->Widget->TakeWidget();
			TArray<FEChartsDataPoint2D> Data = {{10.0, 20.0}, {30.0, 40.0}};
			State->Widget->SetSeriesData(0, Data);
			State->Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
			State->Widget->ApplyEChartsChanges();
			State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
			return true;
		}

	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FWaitFirstApplyAndRebuildCommand : public IAutomationLatentCommand
	{
	public:
		FWaitFirstApplyAndRebuildCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Initial cache apply failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount >= 1)
			{
				Test->TestEqual(TEXT("Initial cache revision"), State->Sink->LastAppliedRevision, int64(1));
				State->SlateWidget.Reset();
				State->Widget->ReleaseSlateResources(false);
				State->SlateWidget = State->Widget->TakeWidget();
				State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for initial cache APPLIED marker."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FWaitRebuildReplayAndProbeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitRebuildReplayAndProbeCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Automatic rebuild replay failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount >= 2)
			{
				Test->TestEqual(TEXT("Rebuild replay keeps revision"), State->Sink->LastAppliedRevision, int64(1));
				Test->TestEqual(TEXT("Rebuild replay keeps point count"), State->Sink->LastAppliedPointCount, 2);
				State->Widget->ExecuteJavascript(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();var s=o.series[0];"
					"var ok=s.type==='line'&&JSON.stringify(s.data)===JSON.stringify([[10,20],[30,40]]);"
					"console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"));
				State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for automatic rebuild replay APPLIED marker."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FWaitReplayProbeAndChangeTemplateCommand : public IAutomationLatentCommand
	{
	public:
		FWaitReplayProbeAndChangeTemplateCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Sink->DataOptionReportCount > 0)
			{
				Test->TestTrue(TEXT("Automatic rebuild getOption preserves cache"), State->Sink->bLastDataOptionSucceeded);
				State->Sink->DataOptionReportCount = 0;
				State->Sink->bLastDataOptionSucceeded = false;
				State->Widget->InitializeECharts(EEChartsTemplate::DataTableScatter3D, EEChartsInteractionMode::ClickOnly);
				State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for rebuild cache getOption probe."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FWaitTemplateReplayAndProbeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitTemplateReplayAndProbeCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Template-change replay failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount >= 3)
			{
				Test->TestEqual(TEXT("Template replay keeps revision"), State->Sink->LastAppliedRevision, int64(1));
				State->Widget->ExecuteJavascript(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();var s=o.series[0];"
					"var ok=s.type==='scatter'&&JSON.stringify(s.data)===JSON.stringify([[10,20],[30,40]]);"
					"console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"));
				State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for template-change replay APPLIED marker."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FStartScatterTransitionCommand : public IAutomationLatentCommand
	{
	public:
		FStartScatterTransitionCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			State->Widget = MakeWidget();
			State->Sink = NewObject<UEChartsWidgetTestSink>();
			State->Sink->AddToRoot();
			State->Widget->OnEChartsApplied.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleApplied);
			State->Widget->OnEChartsError.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleError);
			State->Widget->OnConsoleMessage.AddDynamic(State->Sink, &UEChartsWidgetTestSink::HandleConsoleMessage);
			State->Widget->SetForceWebGLUnavailableForTesting(true);
			State->SlateWidget = State->Widget->TakeWidget();
			TArray<FEChartsDataPoint2D> Data = {{10.0, 20.0}, {30.0, 40.0}};
			State->Widget->SetSeriesData(0, Data);
			State->Widget->InitializeECharts(EEChartsTemplate::DataTableScatter3D, EEChartsInteractionMode::ClickOnly);
			State->Widget->ApplyEChartsChanges();
			State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
			return true;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};

	class FAdvanceScatterTransitionCommand : public IAutomationLatentCommand
	{
	public:
		FAdvanceScatterTransitionCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest, int32 InExpectedAppliedCount)
			: State(InState), Test(InTest), ExpectedAppliedCount(InExpectedAppliedCount) {}
		virtual bool Update() override
		{
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Scatter transition failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount >= ExpectedAppliedCount)
			{
				if (ExpectedAppliedCount == 1)
				{
					TArray<FEChartsDataPoint3D> Data3D = {{1.0, 2.0, 3.0, 4.0, 18.0}};
					State->Widget->Set3DData(0, Data3D);
				}
				else
				{
					TArray<FEChartsDataPoint2D> Data = {{10.0, 20.0}, {30.0, 40.0}};
					State->Widget->SetSeriesData(0, Data);
				}
				State->Widget->ApplyEChartsChanges();
				State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out advancing scatter type transition."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
		int32 ExpectedAppliedCount;
	};

	class FWaitScatterTransitionProbeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitScatterTransitionProbeCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}
		virtual bool Update() override
		{
			if (State->bFailed) return true;
			if (State->Sink->ErrorCount > 0)
			{
				State->bFailed = true;
				Test->AddError(FString::Printf(TEXT("Final scatter transition failed: %s"), *State->Sink->LastError));
				return true;
			}
			if (State->Sink->AppliedCount >= 3)
			{
				State->Widget->ExecuteJavascript(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();var s=o.series[0];"
					"var g=window.UEEChartsHost.getGraphicBoundsStatsForTesting();"
					"var ex=Array.isArray(s.encode.x)?s.encode.x[0]:s.encode.x;var ey=Array.isArray(s.encode.y)?s.encode.y[0]:s.encode.y;"
					"var ok=s.type==='scatter'&&typeof s.symbolSize!=='function'&&!s.dimensions&&!s.ueOriginalData&&"
					"ex===0&&ey===1&&g.count>0&&g.allFinite&&g.hasNonZero;"
					"console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"));
				State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
				return true;
			}
			if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
			{
				State->bFailed = true;
				Test->AddError(TEXT("Timed out waiting for final scatter transition APPLIED marker."));
				return true;
			}
			return false;
		}
	private:
		TSharedRef<FDataBrowserState> State;
		FAutomationTestBase* Test;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataReflectionTest,
	"EChartsWidget.Data.Reflection", EChartsDataTests::Flags)
bool FEChartsDataReflectionTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("2D data point reflected"), FEChartsDataPoint2D::StaticStruct());
	TestNotNull(TEXT("Category data point reflected"), FEChartsCategoryDataPoint::StaticStruct());
	TestNotNull(TEXT("3D data point reflected"), FEChartsDataPoint3D::StaticStruct());
	TestNotNull(TEXT("X axis mode reflected"), StaticEnum<EEChartsXAxisMode>());
	TestEqual(TEXT("Four public x-axis modes including sentinel is not exposed"), StaticEnum<EEChartsXAxisMode>()->NumEnums() - 1, 3);
	TestEqual(TEXT("Maximum series count"), FEChartsPayloadBuilder::MaxSeriesCount, 4);
	TestEqual(TEXT("Maximum point count"), FEChartsPayloadBuilder::MaxPointCount, 100000);
	TestEqual(TEXT("Maximum JSON byte count"), FEChartsPayloadBuilder::MaxJsonBytes, 16 * 1024 * 1024);

	FEChartsDataPoint2D Point2D;
	FEChartsCategoryDataPoint CategoryPoint;
	FEChartsDataPoint3D Point3D;
	TestTrue(TEXT("2D defaults are finite"), FMath::IsFinite(Point2D.X) && FMath::IsFinite(Point2D.Y));
	TestTrue(TEXT("Category Y default is finite"), FMath::IsFinite(CategoryPoint.Y));
	TestTrue(TEXT("3D defaults are finite"), FMath::IsFinite(Point3D.X) && FMath::IsFinite(Point3D.Y) &&
		FMath::IsFinite(Point3D.Z) && FMath::IsFinite(Point3D.ColorValue) && FMath::IsFinite(Point3D.SymbolSizeValue));

	const FName Functions[] = {
		TEXT("AddDataPoint"), TEXT("AddCategoryDataPoint"), TEXT("SetSeriesData"), TEXT("AppendSeriesData"),
		TEXT("GetSeriesData"), TEXT("SetCategorySeriesData"), TEXT("AppendCategorySeriesData"),
		TEXT("GetCategorySeriesData"), TEXT("Set3DData"), TEXT("Append3DData"), TEXT("Get3DData"),
		TEXT("ClearSeries"), TEXT("ClearAll"), TEXT("SetSeriesName"), TEXT("SetXAxisMode"),
		TEXT("ApplyEChartsChanges"), TEXT("SetAutoApplyEnabled")
	};
	for (const FName FunctionName : Functions)
	{
		const UFunction* Function = UEChartsWidget::StaticClass()->FindFunctionByName(FunctionName);
		TestNotNull(*FString::Printf(TEXT("Blueprint node %s exists"), *FunctionName.ToString()), Function);
		TestTrue(*FString::Printf(TEXT("%s is Blueprint callable"), *FunctionName.ToString()),
			Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	UEChartsWidget* Widget = EChartsDataTests::MakeWidget();
	TestEqual(TEXT("Default x-axis mode"), Widget->XAxisMode, EEChartsXAxisMode::ShowAll);
	TestFalse(TEXT("Auto apply defaults disabled"), Widget->bAutoApplyEnabled);
	TestEqual(TEXT("Default max updates per second"), Widget->MaxUpdatesPerSecond, 10.0f);
	TestFalse(TEXT("Cache defaults clean"), Widget->bIsDirty);
	TestEqual(TEXT("No applied revision by default"), Widget->LastAppliedRevision, int64(0));
	TestEqual(TEXT("No applied points by default"), Widget->LastAppliedPointCount, 0);
	const FProperty* DirtyProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, bIsDirty));
	const FProperty* RevisionProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, LastAppliedRevision));
	const FProperty* PointCountProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, LastAppliedPointCount));
	const FProperty* AppliedEventProperty = UEChartsWidget::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UEChartsWidget, OnEChartsApplied));
	TestTrue(TEXT("Dirty state is Blueprint read-only"), DirtyProperty && DirtyProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("Applied revision is Blueprint read-only"), RevisionProperty && RevisionProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("Applied point count is Blueprint read-only"), PointCountProperty && PointCountProperty->HasAllPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintReadOnly));
	TestTrue(TEXT("Applied event is Blueprint assignable"), AppliedEventProperty && AppliedEventProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	EChartsDataTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataCacheValidationTest,
	"EChartsWidget.Data.CacheValidation", EChartsDataTests::Flags)
bool FEChartsDataCacheValidationTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsDataTests::MakeWidget();
	TestFalse(TEXT("Negative series rejected"), Widget->AddDataPoint(-1, 1.0, 2.0));
	TestFalse(TEXT("Series four rejected"), Widget->AddDataPoint(4, 1.0, 2.0));
	TestFalse(TEXT("NaN rejected"), Widget->AddDataPoint(0, std::numeric_limits<double>::quiet_NaN(), 2.0));
	TestFalse(TEXT("Infinity rejected"), Widget->AddDataPoint(0, 1.0, std::numeric_limits<double>::infinity()));
	TestFalse(TEXT("Empty category rejected"), Widget->AddCategoryDataPoint(0, TEXT(""), 1.0));
	TestFalse(TEXT("Whitespace category rejected"), Widget->AddCategoryDataPoint(0, TEXT(" \t"), 1.0));
	TestFalse(TEXT("Invalid input never dirties cache"), Widget->bIsDirty);

	TestTrue(TEXT("Numeric add succeeds"), Widget->AddDataPoint(0, 1.0, 2.0));
	TArray<FEChartsDataPoint2D> AppendedNumeric = {{9.0, 10.0}};
	TestTrue(TEXT("Numeric append establishes an unset slot"), Widget->AppendSeriesData(1, AppendedNumeric));
	TestFalse(TEXT("Category add cannot mix into numeric slot"), Widget->AddCategoryDataPoint(0, TEXT("A"), 3.0));
	TestEqual(TEXT("Numeric cache remains intact"), Widget->GetSeriesData(0).Num(), 1);
	TestEqual(TEXT("Wrong typed category getter is empty"), Widget->GetCategorySeriesData(0).Num(), 0);

	TArray<FEChartsCategoryDataPoint> Categories;
	Categories.Add({TEXT("A"), 5.0});
	Categories.Add({TEXT("B"), 6.0});
	TestTrue(TEXT("Category append establishes an unset slot"), Widget->AppendCategorySeriesData(2, Categories));
	TestTrue(TEXT("Category Set switches slot type"), Widget->SetCategorySeriesData(0, Categories));
	TestEqual(TEXT("Category values stored"), Widget->GetCategorySeriesData(0).Num(), 2);
	TestEqual(TEXT("Old numeric values removed"), Widget->GetSeriesData(0).Num(), 0);
	TestFalse(TEXT("Numeric add now rejected"), Widget->AddDataPoint(0, 7.0, 8.0));

	TArray<FEChartsDataPoint3D> Points3D;
	Points3D.Add({1.0, 2.0, 3.0, 4.0, 5.0});
	TestTrue(TEXT("3D append establishes an unset slot"), Widget->Append3DData(3, Points3D));
	TestTrue(TEXT("3D Set switches slot type"), Widget->Set3DData(0, Points3D));
	TestEqual(TEXT("3D values stored"), Widget->Get3DData(0).Num(), 1);
	TestEqual(TEXT("Old category values removed"), Widget->GetCategorySeriesData(0).Num(), 0);
	TestTrue(TEXT("Clear valid series succeeds"), Widget->ClearSeries(0));
	TestEqual(TEXT("Clear removes points"), Widget->Get3DData(0).Num(), 0);
	TestFalse(TEXT("Clear invalid series fails"), Widget->ClearSeries(4));
	EChartsDataTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPayloadRoundTripTest,
	"EChartsWidget.Data.PayloadRoundTrip", EChartsDataTests::Flags)
bool FEChartsPayloadRoundTripTest::RunTest(const FString& Parameters)
{
	TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> Series;
	Series[0].Name = TEXT("quote=\" slash=\\ newline=\n 数据 😀 </script>");
	Series[0].Type = EEChartsSeriesDataType::Numeric2D;
	Series[0].Numeric2D.Add({1.25, -2.5});
	Series[1].Name = TEXT("类别");
	Series[1].Type = EEChartsSeriesDataType::Category;
	Series[1].Category.Add({TEXT("A\"\\\n😀</script>"), 3.5});
	Series[2].Name = TEXT("3D");
	Series[2].Type = EEChartsSeriesDataType::Data3D;
	Series[2].Data3D.Add({1.0, 2.0, 3.0, 4.0, 5.0});

	FString FirstBase64;
	FString SecondBase64;
	FString Error;
	int32 PointCount = 0;
	TestTrue(TEXT("Payload builds"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::DataTableScatter3D, EEChartsXAxisMode::Category, Series, 9, FirstBase64, PointCount, Error));
	TestEqual(TEXT("Point count sums all series"), PointCount, 3);
	TestTrue(TEXT("Same cache builds again"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::DataTableScatter3D, EEChartsXAxisMode::Category, Series, 9, SecondBase64, PointCount, Error));
	TestEqual(TEXT("Serialization is deterministic"), FirstBase64, SecondBase64);
	TestFalse(TEXT("JavaScript terminator is not present in command-safe Base64"), FirstBase64.Contains(TEXT("</script>")));
	const FString ApplyCommand = FEChartsWidgetJavascript::BuildApplyDataCommand(FirstBase64);
	TestTrue(TEXT("Apply command calls only the fixed Base64 bridge"), ApplyCommand.StartsWith(TEXT("window.UEEChartsHost.applyDataBase64(\"")));
	TestFalse(TEXT("Apply command contains no original user text"), ApplyCommand.Contains(Series[0].Name));
	TestTrue(TEXT("Non-Base64 input cannot be placed in JavaScript"),
		FEChartsWidgetJavascript::BuildApplyDataCommand(TEXT("x\");globalThis.injected=true;//")).IsEmpty());

	FString Json;
	TestTrue(TEXT("Base64 decodes as UTF-8"), FEChartsPayloadBuilder::DecodeBase64Payload(FirstBase64, Json, Error));
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TestTrue(TEXT("Decoded payload is JSON"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid());
	TestEqual(TEXT("Revision survives"), static_cast<int64>(Root->GetNumberField(TEXT("revision"))), int64(9));
	const TArray<TSharedPtr<FJsonValue>>& JsonSeries = Root->GetArrayField(TEXT("series"));
	TestEqual(TEXT("Only non-empty series are emitted"), JsonSeries.Num(), 3);
	TestEqual(TEXT("Dangerous name round trips exactly"), JsonSeries[0]->AsObject()->GetStringField(TEXT("name")), Series[0].Name);
	TestEqual(TEXT("Category round trips exactly"),
		JsonSeries[1]->AsObject()->GetArrayField(TEXT("data"))[0]->AsArray()[0]->AsString(),
		Series[1].Category[0].X);
	const TArray<TSharedPtr<FJsonValue>>& Json3D = JsonSeries[2]->AsObject()->GetArrayField(TEXT("data"))[0]->AsArray();
	TestEqual(TEXT("3D payload contains X"), Json3D[0]->AsNumber(), 1.0);
	TestEqual(TEXT("3D payload contains Y"), Json3D[1]->AsNumber(), 2.0);
	TestEqual(TEXT("3D payload contains Z"), Json3D[2]->AsNumber(), 3.0);
	TestEqual(TEXT("3D payload contains ColorValue"), Json3D[3]->AsNumber(), 4.0);
	TestEqual(TEXT("3D payload contains SymbolSizeValue"), Json3D[4]->AsNumber(), 5.0);

	TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> SparseSeries;
	SparseSeries[2].Name = TEXT("Original slot three");
	SparseSeries[2].Type = EEChartsSeriesDataType::Data3D;
	SparseSeries[2].Data3D.Add({6.0, 7.0, 8.0, 9.0, 10.0});
	FString SparseBase64;
	TestTrue(TEXT("Sparse payload builds"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::DataTableScatter3D, EEChartsXAxisMode::ShowAll, SparseSeries, 10, SparseBase64, PointCount, Error));
	TestEqual(TEXT("Sparse payload point count stays based on all four cache slots"), PointCount, 1);
	TestTrue(TEXT("Sparse Base64 decodes"), FEChartsPayloadBuilder::DecodeBase64Payload(SparseBase64, Json, Error));
	Root.Reset();
	const TSharedRef<TJsonReader<>> SparseReader = TJsonReaderFactory<>::Create(Json);
	TestTrue(TEXT("Sparse decoded payload is JSON"), FJsonSerializer::Deserialize(SparseReader, Root) && Root.IsValid());
	const TArray<TSharedPtr<FJsonValue>>& SparseJsonSeries = Root->GetArrayField(TEXT("series"));
	TestEqual(TEXT("Sparse payload compacts to one series"), SparseJsonSeries.Num(), 1);
	TestEqual(TEXT("Sparse payload compact index starts at zero"), SparseJsonSeries[0]->AsObject()->GetIntegerField(TEXT("index")), 0);
	TestEqual(TEXT("Sparse payload keeps original slot name"), SparseJsonSeries[0]->AsObject()->GetStringField(TEXT("name")), SparseSeries[2].Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPayloadLimitsTest,
	"EChartsWidget.Data.PayloadLimits", EChartsDataTests::Flags)
bool FEChartsPayloadLimitsTest::RunTest(const FString& Parameters)
{
	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> Series;
	Series[0].Type = EEChartsSeriesDataType::Numeric2D;
	Series[0].Numeric2D.SetNum(FEChartsPayloadBuilder::MaxPointCount + 1);
	FString Base64;
	FString Error;
	int32 PointCount = 0;
	TestFalse(TEXT("Oversized point batch fails"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, Series, 1, Base64, PointCount, Error));
	TestTrue(TEXT("Point limit error is clear"), Error.Contains(TEXT("100000")));
	TestTrue(TEXT("Failure does not allocate output"), Base64.IsEmpty());
	TestEqual(TEXT("Point preflight rejects before serializer"),
		FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting(), 0);
	Series[0].Numeric2D.Reset();
	Series[0].Name = FString::ChrN(FEChartsPayloadBuilder::MaxJsonBytes / 6 + 1, TCHAR(1));
	TestTrue(TEXT("Empty series name is not serialized or charged to the payload"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, Series, 1, Base64, PointCount, Error));
	TestEqual(TEXT("Empty payload has zero points"), PointCount, 0);
	FString EmptyJson;
	TestTrue(TEXT("Empty payload decodes"), FEChartsPayloadBuilder::DecodeBase64Payload(Base64, EmptyJson, Error));
	TestTrue(TEXT("Empty payload emits a compact empty series array"), EmptyJson.Contains(TEXT("\"series\":[]")));
	Series[0].Numeric2D.Add({1.0, 2.0});
	Base64.Reset();
	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	TestFalse(TEXT("Oversized active UTF-8 name fails before serialization"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, Series, 1, Base64, PointCount, Error));
	TestTrue(TEXT("JSON byte limit error is clear"), Error.Contains(TEXT("16777216")));
	TestEqual(TEXT("Long escaped control string rejects before serializer"),
		FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting(), 0);

	int64 EstimatedBytes = 0;
	TestTrue(TEXT("Non-BMP string estimate succeeds"), FEChartsPayloadBuilder::AccumulateJsonStringBytesForTesting(
		TEXT("A😀\n"), FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes));
	TestEqual(TEXT("ASCII + emoji + escaped newline byte estimate"), EstimatedBytes, int64(7));
	EstimatedBytes = TNumericLimits<int64>::Max() - 1;
	TestFalse(TEXT("Checked byte accumulation rejects integer overflow"), FEChartsPayloadBuilder::AccumulateJsonStringBytesForTesting(
		TEXT("AB"), TNumericLimits<int64>::Max(), EstimatedBytes));

	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	const int32 OversizedBase64Length = 4 * ((FEChartsPayloadBuilder::MaxJsonBytes + 3) / 3);
	const FString OversizedBase64 = FString::ChrN(OversizedBase64Length, TEXT('A'));
	FString Decoded;
	TestFalse(TEXT("Encoded length rejects oversized decode before allocation"),
		FEChartsPayloadBuilder::DecodeBase64Payload(OversizedBase64, Decoded, Error));
	TestTrue(TEXT("Oversized decode leaves output empty"), Decoded.IsEmpty());
	TestEqual(TEXT("Oversized encoded input never invokes Base64 decoder"),
		FEChartsPayloadBuilder::GetDecodeAttemptCountForTesting(), 0);

	Series = {};
	Series[0].Type = EEChartsSeriesDataType::Data3D;
	Series[0].Data3D.SetNum(FEChartsPayloadBuilder::MaxPointCount);
	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	const double ZeroBatchStart = FPlatformTime::Seconds();
	TestTrue(TEXT("100k zero-valued 3D points fit the JSON limit"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::Bar3DHeightMap, EEChartsXAxisMode::ShowAll, Series, 2, Base64, PointCount, Error));
	const double ZeroBatchSeconds = FPlatformTime::Seconds() - ZeroBatchStart;
	TestEqual(TEXT("100k 3D point count is retained"), PointCount, FEChartsPayloadBuilder::MaxPointCount);
	TestEqual(TEXT("Valid 100k batch reaches serializer once"),
		FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting(), 1);
	TestTrue(TEXT("100k batch completes within a reasonable automation budget"), ZeroBatchSeconds < 10.0);
	TestTrue(TEXT("100k batch Base64 decodes"), FEChartsPayloadBuilder::DecodeBase64Payload(Base64, Decoded, Error));
	const FTCHARToUTF8 ZeroBatchUtf8(*Decoded);
	TestTrue(TEXT("100k batch final UTF-8 JSON stays below 16 MiB"), ZeroBatchUtf8.Length() < FEChartsPayloadBuilder::MaxJsonBytes);
	AddInfo(FString::Printf(TEXT("100k zero-valued 3D payload built in %.3f seconds (%d UTF-8 bytes)."),
		ZeroBatchSeconds, ZeroBatchUtf8.Length()));

	const double LargestFinite = TNumericLimits<double>::Max();
	for (FEChartsDataPoint3D& Point : Series[0].Data3D)
	{
		Point = {LargestFinite, LargestFinite, LargestFinite, LargestFinite, LargestFinite};
	}
	Series[0].Name = FString::ChrN(800000, TCHAR(1));
	Base64.Reset();
	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	TestFalse(TEXT("Actual worst-value JSON plus escaped name is rejected before allocation"),
		FEChartsPayloadBuilder::BuildBase64Payload(
			EEChartsTemplate::Bar3DHeightMap, EEChartsXAxisMode::ShowAll, Series, 3, Base64, PointCount, Error));
	TestTrue(TEXT("Worst-value JSON byte limit error is clear"), Error.Contains(TEXT("16777216")));
	TestTrue(TEXT("Rejected worst-value payload leaves Base64 empty"), Base64.IsEmpty());
	TestEqual(TEXT("Rejected worst-value payload never reaches serializer"),
		FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsApplyRevisionStateTest,
	"EChartsWidget.Data.ApplyRevisionState", EChartsDataTests::Flags)
bool FEChartsApplyRevisionStateTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsDataTests::MakeWidget();
	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	TestTrue(TEXT("First mutation succeeds"), Widget->AddDataPoint(0, 1.0, 2.0));
	Widget->ApplyEChartsChanges();
	TestTrue(TEXT("Apply before Ready remains pending"), Widget->bIsDirty);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestTrue(TEXT("Submitted revision remains dirty until APPLIED"), Widget->bIsDirty);
	TestEqual(TEXT("Nothing acknowledged yet"), Widget->LastAppliedRevision, int64(0));
	Widget->CompletePayloadBuildForTesting();

	TestTrue(TEXT("Second mutation succeeds while first is in flight"), Widget->AddDataPoint(0, 3.0, 4.0));
	Widget->ApplyEChartsChanges();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:1:1"), FString(), 0);
	TestTrue(TEXT("Old revision cannot clear newer dirty data"), Widget->bIsDirty);
	TestEqual(TEXT("Old revision acknowledgement is recorded"), Widget->LastAppliedRevision, int64(1));
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:0:2:2"), FString(), 0);
	TestTrue(TEXT("Old generation cannot clear dirty state"), Widget->bIsDirty);
	Widget->CompletePayloadBuildForTesting();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:2:2"), FString(), 0);
	TestFalse(TEXT("Latest revision clears dirty state"), Widget->bIsDirty);
	TestEqual(TEXT("Latest applied revision stored"), Widget->LastAppliedRevision, int64(2));
	TestEqual(TEXT("Latest applied point count stored"), Widget->LastAppliedPointCount, 2);

	Widget->InitializeECharts(EEChartsTemplate::DataTableScatter3D, EEChartsInteractionMode::ClickOnly);
	TestTrue(TEXT("Explicit Initialize marks cached presentation for replay"), Widget->bIsDirty);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:2"), FString(), 0);
	Widget->CompletePayloadBuildForTesting();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:2:2:2"), FString(), 0);
	TestFalse(TEXT("Explicit Initialize replay acknowledges the unchanged revision"), Widget->bIsDirty);
	TestEqual(TEXT("Replay does not increment data revision"), Widget->LastAppliedRevision, int64(2));

	Widget->ClearAll();
	Widget->ApplyEChartsChanges();
	Widget->CompletePayloadBuildForTesting();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:2:3:0"), FString(), 0);
	TestFalse(TEXT("ClearAll empty presentation can be acknowledged"), Widget->bIsDirty);
	Widget->ReleaseSlateResources(false);
	Widget->PrepareRebuildForTesting();
	TestTrue(TEXT("Automatic rebuild marks cleared presentation for replay"), Widget->bIsDirty);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:3"), FString(), 0);
	Widget->CompletePayloadBuildForTesting();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:3:3:0"), FString(), 0);
	TestFalse(TEXT("Automatic rebuild replays and acknowledges empty state"), Widget->bIsDirty);
	TestEqual(TEXT("Empty replay keeps ClearAll revision"), Widget->LastAppliedRevision, int64(3));
	EChartsDataTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsAutoApplyLifecycleTest,
	"EChartsWidget.Data.AutoApplyLifecycle", EChartsDataTests::Flags)
bool FEChartsAutoApplyLifecycleTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = EChartsDataTests::MakeWidget();
	Widget->SetAutoApplyEnabled(true, 1000.0f);
	TestEqual(TEXT("Hz clamps to 30"), Widget->MaxUpdatesPerSecond, 30.0f);
	Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
	Widget->AddDataPoint(0, 1.0, 2.0);
	TestFalse(TEXT("Loading widget does not schedule ticker"), Widget->IsAutoApplyScheduledForTesting());
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
	TestTrue(TEXT("Ready dirty widget schedules one-shot ticker"), Widget->IsAutoApplyScheduledForTesting());
	Widget->AddDataPoint(0, 3.0, 4.0);
	TestTrue(TEXT("Further changes coalesce into same handle"), Widget->IsAutoApplyScheduledForTesting());
	Widget->ReleaseSlateResources(false);
	TestFalse(TEXT("Release cancels ticker"), Widget->IsAutoApplyScheduledForTesting());
	Widget->SetAutoApplyEnabled(true, 0.0f);
	TestEqual(TEXT("Hz clamps to 1"), Widget->MaxUpdatesPerSecond, 1.0f);
	EChartsDataTests::DestroyWidget(Widget);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFDataApplyTest,
	"EChartsWidget.Integration.CEFDataApply", EChartsDataTests::Flags)
bool FEChartsCEFDataApplyTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: real CEF data apply requires D3D12."));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Real CEF data apply requires initialized Slate."));
		return false;
	}
	const TSharedRef<EChartsDataTests::FDataBrowserState> State = MakeShared<EChartsDataTests::FDataBrowserState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FStartDataBrowserCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitForDataAppliedCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitForDataProbeCommand(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEF3DEffectiveTemplatesTest,
	"EChartsWidget.Integration.CEF3DEffectiveTemplates", EChartsDataTests::Flags)
bool FEChartsCEF3DEffectiveTemplatesTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: real CEF 3D template mapping requires D3D12."));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Real CEF 3D template mapping requires initialized Slate."));
		return false;
	}

#define ADD_3D_CEF_CASE(TemplateValue, ForceFallback, ExpectedTypeValue) \
	{ \
		const TSharedRef<EChartsDataTests::FDataBrowserState> State = MakeShared<EChartsDataTests::FDataBrowserState>(); \
		ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FStart3DTemplateCommand(State, TemplateValue, ForceFallback)); \
		ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitFor3DTemplateAppliedCommand(State, this, TEXT(ExpectedTypeValue))); \
		ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitForDataProbeCommand(State, this)); \
	}
	ADD_3D_CEF_CASE(EEChartsTemplate::Bar3DHeightMap, false, "bar3D");
	ADD_3D_CEF_CASE(EEChartsTemplate::DataTableScatter3D, false, "scatter3D");
	ADD_3D_CEF_CASE(EEChartsTemplate::Bar3DHeightMap, true, "heatmap");
	ADD_3D_CEF_CASE(EEChartsTemplate::DataTableScatter3D, true, "scatter");
#undef ADD_3D_CEF_CASE
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFNative3DStateRegressionTest,
	"EChartsWidget.Integration.CEFNative3DStateRegression", EChartsDataTests::Flags)
bool FEChartsCEFNative3DStateRegressionTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: native 3D camera state requires D3D12."));
		return true;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Native 3D camera state regression requires initialized Slate."));
		return false;
	}
	const TSharedRef<EChartsDataTests::FDataBrowserState> State = MakeShared<EChartsDataTests::FDataBrowserState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FNative3DStateRegressionCommand(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFCacheReplayTest,
	"EChartsWidget.Integration.CEFCacheReplay", EChartsDataTests::Flags)
bool FEChartsCEFCacheReplayTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: real CEF cache replay requires D3D12."));
		return true;
	}
	const TSharedRef<EChartsDataTests::FDataBrowserState> State = MakeShared<EChartsDataTests::FDataBrowserState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FStartCacheReplayCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitFirstApplyAndRebuildCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitRebuildReplayAndProbeCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitReplayProbeAndChangeTemplateCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitTemplateReplayAndProbeCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitForDataProbeCommand(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsCEFScatterTransitionTest,
	"EChartsWidget.Integration.CEFScatterTransition", EChartsDataTests::Flags)
bool FEChartsCEFScatterTransitionTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Not executed under NullRHI: real CEF scatter transitions require D3D12."));
		return true;
	}
	const TSharedRef<EChartsDataTests::FDataBrowserState> State = MakeShared<EChartsDataTests::FDataBrowserState>();
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FStartScatterTransitionCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FAdvanceScatterTransitionCommand(State, this, 1));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FAdvanceScatterTransitionCommand(State, this, 2));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitScatterTransitionProbeCommand(State, this));
	ADD_LATENT_AUTOMATION_COMMAND(EChartsDataTests::FWaitForDataProbeCommand(State, this));
	return true;
}

#endif
