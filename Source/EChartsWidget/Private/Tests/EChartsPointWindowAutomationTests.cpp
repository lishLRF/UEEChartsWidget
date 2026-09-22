#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "EChartsPayloadBuilder.h"
#include "EChartsWidget.h"
#include "EChartsDataTableTestRow.h"
#include "EChartsWidgetTestSink.h"

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace EChartsPointWindowTests
{
	static constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter;

	bool InvokeSetWindow(UEChartsWidget* Widget, const bool bEnabled, const int32 MaxPoints)
	{
		UFunction* Function = Widget->FindFunction(TEXT("Set2DPointWindow"));
		if (!Function) return false;
		FStructOnScope Parameters(Function);
		FBoolProperty* EnabledProperty = FindFProperty<FBoolProperty>(Function, TEXT("bEnabled"));
		FIntProperty* MaxProperty = FindFProperty<FIntProperty>(Function, TEXT("MaxPoints"));
		if (!EnabledProperty || !MaxProperty) return false;
		EnabledProperty->SetPropertyValue_InContainer(Parameters.GetStructMemory(), bEnabled);
		MaxProperty->SetPropertyValue_InContainer(Parameters.GetStructMemory(), MaxPoints);
		Widget->ProcessEvent(Function, Parameters.GetStructMemory());
		return true;
	}

	bool InvokeResetWindow(UEChartsWidget* Widget)
	{
		UFunction* Function = Widget->FindFunction(TEXT("Reset2DPointWindow"));
		if (!Function) return false;
		Widget->ProcessEvent(Function, nullptr);
		return true;
	}

	bool ReadBool(UEChartsWidget* Widget, const FName Name, bool& OutValue)
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Widget->GetClass(), Name);
		if (!Property) return false;
		OutValue = Property->GetPropertyValue_InContainer(Widget);
		return true;
	}

	bool ReadInt(UEChartsWidget* Widget, const FName Name, int32& OutValue)
	{
		const FIntProperty* Property = FindFProperty<FIntProperty>(Widget->GetClass(), Name);
		if (!Property) return false;
		OutValue = Property->GetPropertyValue_InContainer(Widget);
		return true;
	}

	bool Same3D(const TArray<FEChartsDataPoint3D>& Actual, const TArray<FEChartsDataPoint3D>& Expected)
	{
		if (Actual.Num() != Expected.Num()) return false;
		for (int32 Index = 0; Index < Actual.Num(); ++Index)
		{
			const FEChartsDataPoint3D& A = Actual[Index];
			const FEChartsDataPoint3D& E = Expected[Index];
			if (A.X != E.X || A.Y != E.Y || A.Z != E.Z || A.ColorValue != E.ColorValue ||
				A.SymbolSizeValue != E.SymbolSizeValue) return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowContractTest,
	"EChartsWidget.PointWindow.ContractDefaultsClampReset", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowContractTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	UFunction* SetFunction = Widget->FindFunction(TEXT("Set2DPointWindow"));
	UFunction* ResetFunction = Widget->FindFunction(TEXT("Reset2DPointWindow"));
	TestNotNull(TEXT("Set 2D Point Window is reflected"), SetFunction);
	TestNotNull(TEXT("Reset 2D Point Window is reflected"), ResetFunction);
	if (!SetFunction || !ResetFunction) return true;
	TestEqual(TEXT("Set node display name"), SetFunction->GetMetaData(TEXT("DisplayName")), FString(TEXT("Set 2D Point Window")));
	TestEqual(TEXT("Set node category"), SetFunction->GetMetaData(TEXT("Category")), FString(TEXT("ECharts|Data")));
	TestEqual(TEXT("Reset node display name"), ResetFunction->GetMetaData(TEXT("DisplayName")), FString(TEXT("Reset 2D Point Window")));

	const FBoolProperty* EnabledProperty = FindFProperty<FBoolProperty>(Widget->GetClass(), TEXT("b2DPointWindowEnabled"));
	const FIntProperty* MaxProperty = FindFProperty<FIntProperty>(Widget->GetClass(), TEXT("Max2DPointWindowPoints"));
	TestNotNull(TEXT("Enabled state is reflected"), EnabledProperty);
	TestNotNull(TEXT("Max state is reflected"), MaxProperty);
	if (!EnabledProperty || !MaxProperty) return true;
	TestTrue(TEXT("Enabled state is Blueprint read only"), EnabledProperty->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
		EnabledProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	TestTrue(TEXT("Max state is Blueprint read only"), MaxProperty->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
		MaxProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	bool bEnabled = true;
	int32 MaxPoints = 0;
	TestTrue(TEXT("Enabled default readable"), EChartsPointWindowTests::ReadBool(Widget, TEXT("b2DPointWindowEnabled"), bEnabled));
	TestTrue(TEXT("Max default readable"), EChartsPointWindowTests::ReadInt(Widget, TEXT("Max2DPointWindowPoints"), MaxPoints));
	TestFalse(TEXT("Window defaults disabled"), bEnabled);
	TestEqual(TEXT("Window default max"), MaxPoints, 1000);

	TestTrue(TEXT("Set invocation succeeds"), EChartsPointWindowTests::InvokeSetWindow(Widget, true, 0));
	EChartsPointWindowTests::ReadBool(Widget, TEXT("b2DPointWindowEnabled"), bEnabled);
	EChartsPointWindowTests::ReadInt(Widget, TEXT("Max2DPointWindowPoints"), MaxPoints);
	TestTrue(TEXT("Set enables"), bEnabled);
	TestEqual(TEXT("Max clamps low"), MaxPoints, 1);
	EChartsPointWindowTests::InvokeSetWindow(Widget, true, 100001);
	EChartsPointWindowTests::ReadInt(Widget, TEXT("Max2DPointWindowPoints"), MaxPoints);
	TestEqual(TEXT("Max clamps high"), MaxPoints, 100000);
	TestTrue(TEXT("Reset invocation succeeds"), EChartsPointWindowTests::InvokeResetWindow(Widget));
	EChartsPointWindowTests::ReadBool(Widget, TEXT("b2DPointWindowEnabled"), bEnabled);
	EChartsPointWindowTests::ReadInt(Widget, TEXT("Max2DPointWindowPoints"), MaxPoints);
	TestFalse(TEXT("Reset disables"), bEnabled);
	TestEqual(TEXT("Reset restores max"), MaxPoints, 1000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowCoreTest,
	"EChartsWidget.PointWindow.NumericCategoryAndIndependentSeries", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowCoreTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	if (!EChartsPointWindowTests::InvokeSetWindow(Widget, true, 3))
	{
		AddError(TEXT("Set2DPointWindow is missing or has the wrong reflected parameters."));
		return true;
	}
	for (int32 SeriesIndex = 0; SeriesIndex < 4; ++SeriesIndex)
	{
		TArray<FEChartsDataPoint2D> Data;
		for (int32 Point = 0; Point < 5; ++Point) Data.Add({double(SeriesIndex * 10 + Point), double(Point)});
		TestTrue(TEXT("Set numeric series"), Widget->SetSeriesData(SeriesIndex, Data));
		const TArray<FEChartsDataPoint2D> Actual = Widget->GetSeriesData(SeriesIndex);
		TestEqual(TEXT("Each series has its own three point window"), Actual.Num(), 3);
		if (Actual.Num() == 3) TestEqual(TEXT("Each series keeps its own suffix"), Actual[0].X, double(SeriesIndex * 10 + 2));
	}
	TestTrue(TEXT("Add overwrites oldest"), Widget->AddDataPoint(0, 100.0, 100.0));
	TestTrue(TEXT("Append overwrites oldest stably"), Widget->AppendSeriesData(0, {{101.0, 101.0}, {102.0, 102.0}}));
	const TArray<FEChartsDataPoint2D> Numeric = Widget->GetSeriesData(0);
	TestEqual(TEXT("Add and append retain exact capacity"), Numeric.Num(), 3);
	if (Numeric.Num() == 3)
	{
		TestEqual(TEXT("Add and append logical order 0"), Numeric[0].X, 100.0);
		TestEqual(TEXT("Add and append logical order 1"), Numeric[1].X, 101.0);
		TestEqual(TEXT("Add and append logical order 2"), Numeric[2].X, 102.0);
	}

	TestTrue(TEXT("Category set"), Widget->SetCategorySeriesData(1,
		{{TEXT("A"), 1.0}, {TEXT("B"), 2.0}, {TEXT("C"), 3.0}, {TEXT("D"), 4.0}, {TEXT("E"), 5.0}}));
	TestTrue(TEXT("Category add"), Widget->AddCategoryDataPoint(1, TEXT("F"), 6.0));
	TestTrue(TEXT("Category append"), Widget->AppendCategorySeriesData(1, {{TEXT("G"), 7.0}, {TEXT("H"), 8.0}}));
	const TArray<FEChartsCategoryDataPoint> Category = Widget->GetCategorySeriesData(1);
	TestEqual(TEXT("Category exact capacity"), Category.Num(), 3);
	if (Category.Num() == 3)
	{
		TestEqual(TEXT("Category logical order 0"), Category[0].X, FString(TEXT("F")));
		TestEqual(TEXT("Category logical order 2"), Category[2].X, FString(TEXT("H")));
	}

	TestTrue(TEXT("Disable invocation"), EChartsPointWindowTests::InvokeSetWindow(Widget, false, 9));
	for (int32 Index = 0; Index < 5; ++Index) TestTrue(TEXT("Disabled window permits growth"), Widget->AddDataPoint(0, 200.0 + Index, Index));
	TestEqual(TEXT("Disable retains old window and grows from it"), Widget->GetSeriesData(0).Num(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowLargeInputTest,
	"EChartsWidget.PointWindow.Large2DInputUsesFinalSuffix", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowLargeInputTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	if (!EChartsPointWindowTests::InvokeSetWindow(Widget, true, 2))
	{
		AddError(TEXT("Set2DPointWindow is missing or has the wrong reflected parameters."));
		return true;
	}
	TArray<FEChartsDataPoint2D> Large;
	Large.SetNum(100001);
	for (int32 Index = 0; Index < Large.Num(); ++Index) Large[Index] = {double(Index), double(Index)};
	TestTrue(TEXT("Set accepts raw input above the global limit when its final window fits"), Widget->SetSeriesData(0, Large));
	auto Actual = Widget->GetSeriesData(0);
	TestEqual(TEXT("Large Set stores only final window"), Actual.Num(), 2);
	if (Actual.Num() == 2) TestEqual(TEXT("Large Set keeps suffix"), Actual[0].X, 99999.0);
	TestTrue(TEXT("Large Append accepts raw input above the global limit when final window fits"), Widget->AppendSeriesData(0, Large));
	Actual = Widget->GetSeriesData(0);
	TestEqual(TEXT("Large Append stores only final window"), Actual.Num(), 2);
	if (Actual.Num() == 2) TestEqual(TEXT("Large Append keeps suffix"), Actual[0].X, 99999.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindow3DIsolationTest,
	"EChartsWidget.PointWindow.Data3DIsolationAndMixedTotals", EChartsPointWindowTests::Flags)
bool FEChartsPointWindow3DIsolationTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	const TArray<FEChartsDataPoint3D> Original = {
		{1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}, {11, 12, 13, 14, 15}, {16, 17, 18, 19, 20}};
	TestTrue(TEXT("Initial 3D Set"), Widget->Set3DData(2, Original));
	if (!EChartsPointWindowTests::InvokeSetWindow(Widget, true, 2))
	{
		AddError(TEXT("Set2DPointWindow is missing or has the wrong reflected parameters."));
		return true;
	}
	TestTrue(TEXT("Enable does not change any 3D field"), EChartsPointWindowTests::Same3D(Widget->Get3DData(2), Original));
	EChartsPointWindowTests::InvokeSetWindow(Widget, true, 1);
	TestTrue(TEXT("Shrink does not change any 3D field"), EChartsPointWindowTests::Same3D(Widget->Get3DData(2), Original));
	EChartsPointWindowTests::InvokeResetWindow(Widget);
	TestTrue(TEXT("Reset does not change any 3D field"), EChartsPointWindowTests::Same3D(Widget->Get3DData(2), Original));

	EChartsPointWindowTests::InvokeSetWindow(Widget, true, 1);
	TArray<FEChartsDataPoint3D> Replaced = {{21, 22, 23, 24, 25}, {26, 27, 28, 29, 30}};
	TestTrue(TEXT("3D Set ignores enabled 2D window"), Widget->Set3DData(2, Replaced));
	TestTrue(TEXT("3D Set keeps every field"), EChartsPointWindowTests::Same3D(Widget->Get3DData(2), Replaced));
	const TArray<FEChartsDataPoint3D> Added = {{31, 32, 33, 34, 35}, {36, 37, 38, 39, 40}};
	TestTrue(TEXT("3D Append ignores enabled 2D window"), Widget->Append3DData(2, Added));
	Replaced.Append(Added);
	TestTrue(TEXT("3D Append keeps count, order, color, and symbol size"), EChartsPointWindowTests::Same3D(Widget->Get3DData(2), Replaced));

	TArray<FEChartsDataPoint3D> TooLarge;
	TooLarge.SetNum(100001);
	TestFalse(TEXT("3D Set still rejects 100001 points"), Widget->Set3DData(3, TooLarge));
	TestTrue(TEXT("Clear earlier 3D fixture before total-limit scenario"), Widget->ClearSeries(2));
	TArray<FEChartsDataPoint3D> Full;
	Full.SetNum(99999);
	TestTrue(TEXT("3D Set can fill 99999 points"), Widget->Set3DData(3, Full));
	TestTrue(TEXT("Small-window 2D suffix can coexist at the global total"), Widget->SetSeriesData(0,
		{{1, 1}, {2, 2}, {3, 3}, {4, 4}}));
	TestEqual(TEXT("Mixed 2D series is cropped"), Widget->GetSeriesData(0).Num(), 1);
	TestEqual(TEXT("Mixed 3D series remains intact"), Widget->Get3DData(3).Num(), 99999);
	TestFalse(TEXT("3D Append still rejects total 100001"), Widget->Append3DData(3, {{1, 2, 3, 4, 5}}));
	TestEqual(TEXT("Rejected 3D append leaves all 3D points"), Widget->Get3DData(3).Num(), 99999);
	return true;
}

class FEChartsPointWindowSnapshotCommand : public IAutomationLatentCommand
{
public:
	FEChartsPointWindowSnapshotCommand(FAutomationTestBase* InTest, const int32 InMode)
		: Test(InTest), Mode(InMode) {}

	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds();
			Widget.Reset(NewObject<UEChartsWidget>());
			Widget->Set2DPointWindow(true, Mode == 3 ? 1 : 2);
			Widget->InitializeECharts(Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
			Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
			if (Mode == 3) Widget->SetSeriesData(0, {{42.0, 42.0}});
			UDataTable* Table = NewObject<UDataTable>();
			Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			const int32 RowCount = Mode == 3 ? 1 : 5;
			for (int32 Index = 1; Index <= RowCount; ++Index)
			{
				FEChartsDataTableTestRow Row;
				Row.X = Index; Row.Y = Index; Row.Z = Index + 10;
				Row.Category = Mode == 3
					? FString::ChrN(FEChartsPayloadBuilder::MaxJsonBytes + 1, TEXT('X'))
					: FString::Chr(TEXT('A') + Index - 1);
				Table->AddRow(FName(*FString::FromInt(Index)), Row);
			}
			FEChartsDataTableMapping Mapping;
			Mapping.X = Mode == 1 || Mode == 3 ? TEXT("Category") : TEXT("X");
			Mapping.Y = TEXT("Y"); Mapping.Z = TEXT("Z");
			Test->TestTrue(TEXT("Snapshot mapping validates"), Widget->SetDataTableMapping(Table, Mapping));
			Widget->LoadDataTable(2);
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 15.0)
		{
			Test->AddError(TEXT("Point-window snapshot timed out."));
			Widget->CancelDataTableLoad();
			return true;
		}
		if (Widget->IsApplyInFlightForTesting()) Widget->AcknowledgeCurrentApplyForTesting();
		if (Widget->DataTableLoadState == EEChartsDataTableLoadState::Error)
		{
			if (Mode == 3)
			{
				const auto Restored = Widget->GetSeriesData(0);
				Test->TestEqual(TEXT("Oversized final suffix restores the previous cache"), Restored.Num(), 1);
				if (Restored.Num() == 1) Test->TestEqual(TEXT("Rollback keeps the previous value"), Restored[0].X, 42.0);
				Test->TestTrue(TEXT("Oversized final suffix reports the JSON safety limit"),
					Widget->LastDataTableError.Contains(TEXT("JSON safety limit")));
				Widget->ReleaseSlateResources(false);
				return true;
			}
			Test->AddError(FString::Printf(TEXT("Point-window snapshot failed: %s"), *Widget->LastDataTableError));
			return true;
		}
		if (Widget->DataTableLoadState != EEChartsDataTableLoadState::Completed) return false;
		const int32 Expected = Mode == 2 ? 5 : 2;
		Test->TestEqual(TEXT("Snapshot ACK reports the installed point count"), Widget->LastAppliedPointCount, Expected);
		if (Mode == 0)
		{
			const auto Data = Widget->GetSeriesData(0);
			Test->TestEqual(TEXT("Numeric snapshot is cropped after sorting"), Data.Num(), 2);
			if (Data.Num() == 2) Test->TestEqual(TEXT("Numeric snapshot keeps sorted suffix"), Data[0].X, 4.0);
		}
		else if (Mode == 1)
		{
			const auto Data = Widget->GetCategorySeriesData(0);
			Test->TestEqual(TEXT("Category snapshot is cropped after sorting"), Data.Num(), 2);
			if (Data.Num() == 2) Test->TestEqual(TEXT("Category snapshot keeps sorted suffix"), Data[0].X, FString(TEXT("D")));
		}
		else
		{
			Test->TestEqual(TEXT("3D snapshot is never cropped by the 2D window"), Widget->Get3DData(0).Num(), 5);
		}
		Widget->ReleaseSlateResources(false);
		return true;
	}

private:
	FAutomationTestBase* Test;
	int32 Mode;
	double Start = 0.0;
	TStrongObjectPtr<UEChartsWidget> Widget;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowSnapshotTest,
	"EChartsWidget.PointWindow.DataTableSnapshots", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowSnapshotTest::RunTest(const FString& Parameters)
{
	for (int32 Mode = 0; Mode < 4; ++Mode)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FEChartsPointWindowSnapshotCommand(this, Mode));
	}
	return true;
}

class FEChartsPointWindowStreamCompletionCommand : public IAutomationLatentCommand
{
public:
	explicit FEChartsPointWindowStreamCompletionCommand(FAutomationTestBase* InTest) : Test(InTest) {}

	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds();
			Widget.Reset(NewObject<UEChartsWidget>());
			Widget->Set2DPointWindow(true, 2);
			Widget->SetTimeSeriesEnabled(true);
			Widget->SetTimeSeriesWindow(5);
			Widget->InitializeECharts();
			Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
			UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			for (int32 Index = 1; Index <= 7; ++Index)
			{
				FEChartsDataTableTestRow Row; Row.X = Index; Row.Y = Index;
				Table->AddRow(FName(*FString::FromInt(Index)), Row);
			}
			FEChartsDataTableMapping Mapping; Mapping.X = TEXT("X"); Mapping.Y = TEXT("Y");
			Test->TestTrue(TEXT("Stream mapping validates"), Widget->SetDataTableMapping(Table, Mapping));
			Test->TestTrue(TEXT("Stream starts"), Widget->StartDataTableStreaming(0.01f, 1, false, 7));
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 15.0)
		{
			Test->AddError(TEXT("Point-window stream completion timed out."));
			Widget->StopDataTableStreaming();
			return true;
		}
		if (Widget->StreamState == EEChartsDataTableStreamState::Playing ||
			Widget->StreamState == EEChartsDataTableStreamState::Paused)
		{
			MaxActiveCount = FMath::Max(MaxActiveCount, Widget->GetSeriesData(0).Num());
		}
		if (Widget->IsApplyInFlightForTesting()) Widget->AcknowledgeCurrentApplyForTesting();
		if (Widget->StreamState == EEChartsDataTableStreamState::Error)
		{
			Test->AddError(FString::Printf(TEXT("Point-window stream failed: %s"), *Widget->LastError));
			return true;
		}
		if (Widget->StreamState != EEChartsDataTableStreamState::Completed || Widget->bIsDirty ||
			Widget->IsApplyInFlightForTesting()) return false;
		Test->TestEqual(TEXT("Active stream uses TimeSeriesWindow instead of the 2D window"), MaxActiveCount, 5);
		const auto Data = Widget->GetSeriesData(0);
		Test->TestEqual(TEXT("Completed stream restores the ordinary 2D window"), Data.Num(), 2);
		if (Data.Num() == 2) Test->TestEqual(TEXT("Completed stream retains the newest suffix"), Data[0].X, 6.0);
		Test->TestEqual(TEXT("Final post-completion full apply ACK reports the ordinary window"), Widget->LastAppliedPointCount, 2);
		Widget->ReleaseSlateResources(false);
		return true;
	}

private:
	FAutomationTestBase* Test;
	double Start = 0.0;
	int32 MaxActiveCount = 0;
	TStrongObjectPtr<UEChartsWidget> Widget;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowStreamCompletionTest,
	"EChartsWidget.PointWindow.TimeSeriesPrecedenceAndCompletion", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowStreamCompletionTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FEChartsPointWindowStreamCompletionCommand(this));
	return true;
}

class FEChartsPointWindowFailedRestartCommand : public IAutomationLatentCommand
{
public:
	explicit FEChartsPointWindowFailedRestartCommand(FAutomationTestBase* InTest) : Test(InTest) {}
	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds(); Widget.Reset(NewObject<UEChartsWidget>());
			Table.Reset(NewObject<UDataTable>()); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			for (int32 Index = 1; Index <= 5; ++Index)
			{
				FEChartsDataTableTestRow Row; Row.X = Index; Row.Y = Index;
				Table->AddRow(FName(*FString::FromInt(Index)), Row);
			}
			Widget->Set2DPointWindow(true, 2); Widget->SetTimeSeriesEnabled(true); Widget->SetTimeSeriesWindow(5);
			Widget->InitializeECharts(); Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
			FEChartsDataTableMapping Mapping; Mapping.X = TEXT("X"); Mapping.Y = TEXT("Y");
			Widget->SetDataTableMapping(Table.Get(), Mapping);
			Test->TestTrue(TEXT("Loop stream starts"), Widget->StartDataTableStreaming(0.01f, 5, true, 5));
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 15.0)
		{
			Test->AddError(TEXT("Failed-restart point-window regression timed out."));
			Widget->StopDataTableStreaming(); return true;
		}
		if (Stage == 0 && Widget->IsApplyInFlightForTesting()) Widget->AcknowledgeCurrentApplyForTesting();
		if (Stage == 0 && Widget->StreamState == EEChartsDataTableStreamState::Playing &&
			Widget->GetSeriesData(0).Num() == 5 && !Widget->bIsDirty)
		{
			Widget->PauseDataTableStreaming();
			Table->RowStruct = nullptr;
			Test->TestFalse(TEXT("Restart with an invalidated source fails"), Widget->StartDataTableStreaming(0.01f, 1, false, 1));
			Test->TestEqual(TEXT("Failed restart restores the ordinary cache window"), Widget->GetSeriesData(0).Num(), 2);
			Test->TestTrue(TEXT("Failed restart marks the cropped cache dirty"), Widget->bIsDirty);
			Widget->ApplyEChartsChanges(); Stage = 1;
		}
		if (Stage == 1 && Widget->IsApplyInFlightForTesting())
		{
			Widget->AcknowledgeCurrentApplyForTesting();
			Test->TestEqual(TEXT("Failed restart can submit the cropped full payload"), Widget->LastAppliedPointCount, 2);
			Test->TestFalse(TEXT("Cropped payload ACK clears dirty state"), Widget->bIsDirty);
			Widget->ReleaseSlateResources(false); return true;
		}
		return false;
	}
private:
	FAutomationTestBase* Test;
	int32 Stage = 0;
	double Start = 0.0;
	TStrongObjectPtr<UEChartsWidget> Widget;
	TStrongObjectPtr<UDataTable> Table;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowFailedRestartTest,
	"EChartsWidget.PointWindow.FailedStreamRestartSynchronizesCrop", EChartsPointWindowTests::Flags)
bool FEChartsPointWindowFailedRestartTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FEChartsPointWindowFailedRestartCommand(this));
	return true;
}

class FEChartsPointWindowCEFCommand : public IAutomationLatentCommand
{
public:
	FEChartsPointWindowCEFCommand(FAutomationTestBase* InTest, const int32 InMode, const bool bInSnapshot)
		: Test(InTest), Mode(InMode), bSnapshot(bInSnapshot) {}

	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds();
			Widget.Reset(NewObject<UEChartsWidget>());
			Sink.Reset(NewObject<UEChartsWidgetTestSink>());
			Widget->OnConsoleMessage.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleConsoleMessage);
			Widget->OnEChartsError.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleError);
			Widget->OnEChartsApplied.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleApplied);
			Slate = Widget->TakeWidget();
			Widget->Set2DPointWindow(true, bSnapshot ? 2 : 3);
			Widget->InitializeECharts(Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
			if (bSnapshot)
			{
				UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
				for (int32 Index = 1; Index <= 5; ++Index)
				{
					FEChartsDataTableTestRow Row; Row.X = Index; Row.Y = Index; Row.Z = Index + 10;
					Row.Category = FString::Chr(TEXT('A') + Index - 1);
					Table->AddRow(FName(*FString::FromInt(Index)), Row);
				}
				FEChartsDataTableMapping Mapping; Mapping.X = Mode == 1 ? TEXT("Category") : TEXT("X");
				Mapping.Y = TEXT("Y"); Mapping.Z = TEXT("Z");
				Widget->SetDataTableMapping(Table, Mapping); Widget->LoadDataTable(2);
			}
			else Widget->SetAutoApplyEnabled(true, 30.0f);
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 40.0 || Sink->ErrorCount > 0)
		{
			Test->AddError(FString::Printf(TEXT("Real CEF point-window test failed/timeout: %s"), *Sink->LastError));
			Cleanup(); return true;
		}
		if (!bSnapshot && Stage == 0 && Widget->RuntimeState == EEChartsRuntimeState::Ready)
		{
			if (Mode == 0)
			{
				for (int32 Index = 1; Index <= 5; ++Index) Widget->AddDataPoint(0, Index, Index * 10);
				Widget->AppendSeriesData(0, {{6, 60}, {7, 70}});
			}
			else
			{
				for (int32 Index = 1; Index <= 5; ++Index)
					Widget->AddCategoryDataPoint(0, FString::Chr(TEXT('A') + Index - 1), Index * 10);
				Widget->AppendCategorySeriesData(0, {{TEXT("F"), 60}, {TEXT("G"), 70}});
				Widget->SetXAxisMode(EEChartsXAxisMode::Category);
			}
			Stage = 1;
		}
		const int32 Expected = bSnapshot ? (Mode == 2 ? 5 : 2) : 3;
		const bool bReadyToProbe = bSnapshot
			? Widget->DataTableLoadState == EEChartsDataTableLoadState::Completed
			: Stage == 1 && Sink->AppliedCount > 0 && !Widget->bIsDirty;
		if (!bProbed && bReadyToProbe)
		{
			Test->TestEqual(TEXT("Real CEF ACK uses the final installed point count"), Sink->LastAppliedPointCount, Expected);
			FString Condition;
			if (bSnapshot && Mode == 2)
			{
				Condition = TEXT("o.series[0].data.length===5&&o.series[0].data[0][0]===1&&o.series[0].data[4][4]===12");
			}
			else if (Mode == 1)
			{
				const FString Labels = bSnapshot ? TEXT("['D','E']") : TEXT("['E','F','G']");
				const FString Values = bSnapshot ? TEXT("[4,5]") : TEXT("[50,60,70]");
				Condition = FString::Printf(TEXT("JSON.stringify(o.xAxis[0].data)===JSON.stringify(%s)&&JSON.stringify(o.series[0].data)===JSON.stringify(%s)&&o.yAxis[0].min==null&&o.yAxis[0].max==null"), *Labels, *Values);
			}
			else
			{
				const FString Values = bSnapshot ? TEXT("[[4,4],[5,5]]") : TEXT("[[5,50],[6,60],[7,70]]");
				Condition = FString::Printf(TEXT("JSON.stringify(o.series[0].data)===JSON.stringify(%s)&&o.xAxis[0].min==null&&o.xAxis[0].max==null&&o.yAxis[0].min==null&&o.yAxis[0].max==null"), *Values);
			}
			Condition += TEXT("&&(!o.dataZoom||o.dataZoom.length===0)");
			Widget->ExecuteJavascript(FString::Printf(TEXT("(function(){var o=window.UEEChartsHost.getOptionForTesting();console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+((%s)?'OK':'BAD'));}());"), *Condition));
			bProbed = true;
		}
		if (bProbed && Sink->DataOptionReportCount > 0)
		{
			Test->TestTrue(TEXT("Real CEF option contains only the final 2D window and auto-scaled axes"), Sink->bLastDataOptionSucceeded);
			Cleanup(); return true;
		}
		return false;
	}

private:
	void Cleanup()
	{
		Slate.Reset();
		if (Widget.IsValid()) Widget->ReleaseSlateResources(false);
	}
	FAutomationTestBase* Test;
	int32 Mode;
	bool bSnapshot;
	int32 Stage = 0;
	bool bProbed = false;
	double Start = 0.0;
	TStrongObjectPtr<UEChartsWidget> Widget;
	TStrongObjectPtr<UEChartsWidgetTestSink> Sink;
	TSharedPtr<SWidget> Slate;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsPointWindowCEFTest,
	"EChartsWidget.Integration.CEFPointWindow", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsPointWindowCEFTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Real CEF point-window coverage requires D3D12."));
		return true;
	}
	for (int32 Mode = 0; Mode < 3; ++Mode) ADD_LATENT_AUTOMATION_COMMAND(FEChartsPointWindowCEFCommand(this, Mode, true));
	for (int32 Mode = 0; Mode < 2; ++Mode) ADD_LATENT_AUTOMATION_COMMAND(FEChartsPointWindowCEFCommand(this, Mode, false));
	return true;
}

#endif
