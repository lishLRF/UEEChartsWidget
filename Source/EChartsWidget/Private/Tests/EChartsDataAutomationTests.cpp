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

			State->Widget->AddDataPoint(0, 1.0, 2.0);
			State->Widget->AddDataPoint(0, 3.0, 4.0);
			State->Widget->AddDataPoint(1, 5.0, 6.0);
			TArray<FEChartsCategoryDataPoint> Category = {{TEXT("A"), 7.0}, {TEXT("B"), 8.0}};
			State->Widget->SetCategorySeriesData(2, Category);
			TArray<FEChartsDataPoint3D> Data3D = {{9.0, 10.0, 11.0, 12.0, 13.0}};
			State->Widget->Set3DData(3, Data3D);
			State->Widget->SetXAxisMode(EEChartsXAxisMode::Category);
			State->Widget->InitializeECharts(EEChartsTemplate::DataTableScatter3D, EEChartsInteractionMode::ClickOnly);
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
				Test->TestEqual(TEXT("Real CEF APPLIED point count"), State->Sink->LastAppliedPointCount, 6);
				Test->TestEqual(TEXT("Real CEF APPLIED revision"), State->Sink->LastAppliedRevision, int64(6));
				State->Widget->ExecuteJavascript(TEXT(
					"(function(){var o=window.UEEChartsHost.getOptionForTesting();"
					"var x=Array.isArray(o.xAxis)?o.xAxis[0]:o.xAxis;"
					"var ok=o.series.length===4&&o.series[0].data.length===2&&o.series[0].data[1][1]===4&&"
					"o.series[1].data.length===1&&o.series[2].data[0]===7&&o.series[3].type==='scatter3D'&&"
					"o.series[3].data[0][4]===13&&x.type==='category'&&x.data[1]==='B';"
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

	class FWaitForDataProbeCommand : public IAutomationLatentCommand
	{
	public:
		FWaitForDataProbeCommand(const TSharedRef<FDataBrowserState>& InState, FAutomationTestBase* InTest)
			: State(InState), Test(InTest) {}

		virtual bool Update() override
		{
			if (State->bFailed || State->Sink->DataOptionReportCount > 0)
			{
				if (!State->bFailed) Test->TestTrue(TEXT("CEF getOption contains all 2D/category/3D data"), State->Sink->bLastDataOptionSucceeded);
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
	TestEqual(TEXT("All four deterministic slots emitted"), JsonSeries.Num(), 4);
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
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPayloadLimitsTest,
	"EChartsWidget.Data.PayloadLimits", EChartsDataTests::Flags)
bool FEChartsPayloadLimitsTest::RunTest(const FString& Parameters)
{
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
	Series[0].Numeric2D.Reset();
	Series[0].Name = FString::ChrN(FEChartsPayloadBuilder::MaxJsonBytes / 2 + 1, TEXT('X'));
	TestFalse(TEXT("Oversized UTF-8 JSON estimate fails before serialization"), FEChartsPayloadBuilder::BuildBase64Payload(
		EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, Series, 1, Base64, PointCount, Error));
	TestTrue(TEXT("JSON byte limit error is clear"), Error.Contains(TEXT("16777216")));
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

	TestTrue(TEXT("Second mutation succeeds while first is in flight"), Widget->AddDataPoint(0, 3.0, 4.0));
	Widget->ApplyEChartsChanges();
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:1:1"), FString(), 0);
	TestTrue(TEXT("Old revision cannot clear newer dirty data"), Widget->bIsDirty);
	TestEqual(TEXT("Old revision acknowledgement is recorded"), Widget->LastAppliedRevision, int64(1));
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:0:2:2"), FString(), 0);
	TestTrue(TEXT("Old generation cannot clear dirty state"), Widget->bIsDirty);
	Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:2:2"), FString(), 0);
	TestFalse(TEXT("Latest revision clears dirty state"), Widget->bIsDirty);
	TestEqual(TEXT("Latest applied revision stored"), Widget->LastAppliedRevision, int64(2));
	TestEqual(TEXT("Latest applied point count stored"), Widget->LastAppliedPointCount, 2);
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

#endif
