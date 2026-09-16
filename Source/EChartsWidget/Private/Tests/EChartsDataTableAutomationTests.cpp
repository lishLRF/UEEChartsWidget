#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "EChartsDataTableLoader.h"
#include "EChartsDataTableTestRow.h"
#include "EChartsWidget.h"
#include "EChartsWidgetTestSink.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableContractTest, "EChartsWidget.DataTable.BlueprintContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableContractTest::RunTest(const FString& Parameters)
{
	for (const TCHAR* Name : {TEXT("GetEChartsDataTableColumns"), TEXT("SetDataTableMapping"), TEXT("LoadDataTable"),
	         TEXT("CancelDataTableLoad")})
	{
		const UFunction* Function = UEChartsWidget::StaticClass()->FindFunctionByName(Name);
		TestNotNull(Name, Function);
		if (Function)
			TestTrue(TEXT("Blueprint callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}
	for (const TCHAR* Name : {TEXT("DataTableLoadState"), TEXT("RowsProcessed"), TEXT("RowsSucceeded"),
	         TEXT("RowsSkipped"), TEXT("TotalRows"), TEXT("LastDataTableError"), TEXT("OnDataTableLoadProgress"),
	         TEXT("OnDataTableLoaded"), TEXT("OnDataTableLoadCancelled")})
		TestNotNull(Name, UEChartsWidget::StaticClass()->FindPropertyByName(Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableDiscoveryTest, "EChartsWidget.DataTable.DiscoveryAndPreflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableDiscoveryTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	UDataTable* Table = NewObject<UDataTable>();
	Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
	TArray<FEChartsDataTableColumn> Columns;
	FString Error;
	TestFalse(TEXT("Null table rejected"), Widget->GetEChartsDataTableColumns(nullptr, Columns, Error));
	TestTrue(TEXT("Columns discovered"), Widget->GetEChartsDataTableColumns(Table, Columns, Error));
	TestEqual(TEXT("Top-level columns only"), Columns.Num(), 11);
	for (const FEChartsDataTableColumn& C : Columns)
	{
		const bool bNumeric =
		    C.Name == TEXT("X") || C.Name == TEXT("Y") || C.Name == TEXT("Z") || C.Name == TEXT("Integer");
		const bool bUnsupported = C.Name == TEXT("Flag") || C.Name == TEXT("Nested") || C.Name == TEXT("Array");
		TestEqual(TEXT("Column classification"), C.ColumnType,
		    bNumeric       ? EEChartsDataTableColumnType::Numeric
		    : bUnsupported ? EEChartsDataTableColumnType::Unsupported
		                   : EEChartsDataTableColumnType::Category);
		TestEqual(TEXT("Numeric capability"), C.bCanUseAsNumeric, bNumeric);
		TestEqual(TEXT("X capability"), C.bCanUseAsX, !bUnsupported);
	}
	FEChartsDataTableMapping Mapping;
	Mapping.X = TEXT("X");
	Mapping.Y = TEXT("Y");
	TestTrue(TEXT("Numeric mapping"), Widget->SetDataTableMapping(Table, Mapping));
	Mapping.X = TEXT("Category");
	TestTrue(TEXT("Category mapping"), Widget->SetDataTableMapping(Table, Mapping));
	Mapping.Y = TEXT("Flag");
	TestFalse(TEXT("Bool Y rejected"), Widget->SetDataTableMapping(Table, Mapping));
	TestEqual(TEXT("Preflight does not scan"), Widget->RowsProcessed, 0);
	TestEqual(TEXT("Invalid mapping sets Error"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Error);
	Mapping.Y = TEXT("Y");
	Widget->CurrentTemplate = EEChartsTemplate::Bar3DHeightMap;
	TestFalse(TEXT("3D requires numeric X"), Widget->SetDataTableMapping(Table, Mapping));
	Mapping.X = TEXT("X");
	TestFalse(TEXT("3D requires Z"), Widget->SetDataTableMapping(Table, Mapping));
	Mapping.Z = TEXT("Z");
	TestTrue(TEXT("Valid 3D"), Widget->SetDataTableMapping(Table, Mapping));
	Mapping.Color = TEXT("Missing");
	TestFalse(TEXT("Missing optional mapped field rejected"), Widget->SetDataTableMapping(Table, Mapping));
	Widget->ReleaseSlateResources(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableBudgetTest, "EChartsWidget.DataTable.BudgetAndCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableBudgetTest::RunTest(const FString& Parameters)
{
	UEChartsWidget* Widget = NewObject<UEChartsWidget>();
	Widget->AddToRoot();
	TStrongObjectPtr<UEChartsWidgetTestSink> Sink(NewObject<UEChartsWidgetTestSink>());
	Widget->OnDataTableLoadProgress.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleTableProgress);
	Widget->OnDataTableLoadCancelled.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleTableCancelled);
	UDataTable* Table = NewObject<UDataTable>();
	Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
	for (int32 I = 0; I < 1000; ++I)
	{
		FEChartsDataTableTestRow Row;
		Row.X = I;
		Table->AddRow(FName(*FString::FromInt(I)), Row);
	}
	FEChartsDataTableMapping Mapping;
	Mapping.X = TEXT("X");
	Mapping.Y = TEXT("Y");
	Widget->SetDataTableMapping(Table, Mapping);
	Widget->LoadDataTable(7);
	TestEqual(TEXT("Startup does not read rows"), Widget->RowsProcessed, 0);
	TestEqual(TEXT("Table row count"), Widget->TotalRows, 1000);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("One frame has hard row budget"), Widget->RowsProcessed, 7);
	TestEqual(TEXT("One progress event per batch"), Sink->TableProgressCount, 1);
	TestEqual(TEXT("Progress delta is budget"), Sink->MaxTableBatch, 7);
	TestTrue(TEXT("Events on GameThread"), Sink->bTableEventsOnGameThread);
	TestEqual(TEXT("Large table stays Reading"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Reading);
	Widget->CancelDataTableLoad();
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("Cancelled ticker no longer reads"), Widget->RowsProcessed, 7);
	TestEqual(TEXT("Cancelled state"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Cancelled);
	TestEqual(TEXT("Cancelled event exactly once"), Sink->TableCancelledCount, 1);
	Widget->CancelDataTableLoad();
	TestEqual(TEXT("Repeated cancel does not broadcast"), Sink->TableCancelledCount, 1);
	Widget->LoadDataTable(0);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("Lower budget clamps to one"), Widget->RowsProcessed, 1);
	Widget->ReleaseSlateResources(false);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("Release cancels"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Cancelled);
	Widget->RemoveFromRoot();
	return true;
}

class FEChartsDataTableSnapshotCommand : public IAutomationLatentCommand
{
	FAutomationTestBase* Test;
	int32 Mode;
	TStrongObjectPtr<UEChartsWidget> Widget;
	double Start = 0;

  public:
	FEChartsDataTableSnapshotCommand(FAutomationTestBase* InTest, int32 InMode) : Test(InTest), Mode(InMode)
	{
	}
	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds();
			Widget.Reset(NewObject<UEChartsWidget>());
			Widget->InitializeECharts(
			    Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
			UDataTable* Table = NewObject<UDataTable>();
			Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			FEChartsDataTableTestRow A;
			A.X = 3;
			A.Y = 30;
			A.Category = TEXT("Beta");
			FEChartsDataTableTestRow B;
			B.X = 1;
			B.Y = 10;
			B.Category = TEXT("Alpha");
			FEChartsDataTableTestRow Invalid;
			Invalid.X = 2;
			Invalid.Y = std::numeric_limits<float>::infinity();
			Table->AddRow(TEXT("A"), A);
			Table->AddRow(TEXT("B"), B);
			Table->AddRow(TEXT("Bad"), Invalid);
			FEChartsDataTableMapping M;
			M.X = Mode == 1 ? TEXT("Category") : TEXT("X");
			M.Y = TEXT("Y");
			M.Z = TEXT("Z");
			if (Mode == 3)
				M.Order = EEChartsDataTableOrder::RowName;
			Widget->SetDataTableMapping(Table, M);
			Widget->LoadDataTable(1);
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 10)
		{
			Test->AddError(TEXT("DataTable snapshot timed out"));
			Widget->ReleaseSlateResources(false);
			return true;
		}
		if (Widget->DataTableLoadState == EEChartsDataTableLoadState::Error)
		{
			Test->AddError(Widget->LastDataTableError);
			return true;
		}
		if (Widget->DataTableLoadState != EEChartsDataTableLoadState::Applying)
			return false;
		Test->TestEqual(TEXT("All source rows processed"), Widget->RowsProcessed, 3);
		Test->TestEqual(TEXT("Invalid row skipped"), Widget->RowsSkipped, 1);
		Test->TestEqual(TEXT("Two valid rows"), Widget->RowsSucceeded, 2);
		Test->TestEqual(TEXT("Before Ready no completion"), Widget->LastAppliedRevision, int64(0));
		if (Mode == 1)
		{
			auto Data = Widget->GetCategorySeriesData(0);
			Test->TestEqual(TEXT("Category count"), Data.Num(), 2);
			if (Data.Num() == 2)
				Test->TestEqual(TEXT("Category lexical sort"), Data[0].X, FString(TEXT("Alpha")));
			Test->TestEqual(TEXT("Category axis"), Widget->XAxisMode, EEChartsXAxisMode::Category);
		}
		else if (Mode == 2)
		{
			auto Data = Widget->Get3DData(0);
			Test->TestEqual(TEXT("3D count"), Data.Num(), 2);
			if (Data.Num() == 2)
			{
				Test->TestEqual(TEXT("3D sorted"), Data[0].X, 1.0);
				Test->TestEqual(TEXT("Default color=Z"), Data[0].ColorValue, 7.0);
				Test->TestEqual(TEXT("Default symbol=12"), Data[0].SymbolSizeValue, 12.0);
			}
		}
		else
		{
			auto Data = Widget->GetSeriesData(0);
			Test->TestEqual(TEXT("Numeric count"), Data.Num(), 2);
			if (Data.Num() == 2)
				Test->TestEqual(TEXT("Numeric/rowname ordering"), Data[0].X, Mode == 3 ? 3.0 : 1.0);
		}
		Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
		Test->TestEqual(
		    TEXT("Ready still waits for ACK"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Applying);
		// SetSeries + SetXAxisMode issue two revisions on a pristine widget.
		Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:2:2"), FString(), 0);
		Test->TestEqual(
		    TEXT("ACK completes snapshot"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Completed);
		Widget->ReleaseSlateResources(false);
		return true;
	}
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableSnapshotTest, "EChartsWidget.DataTable.SnapshotAndAcknowledgement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableSnapshotTest::RunTest(const FString& Parameters)
{
	for (int32 Mode = 0; Mode < 4; ++Mode)
		ADD_LATENT_AUTOMATION_COMMAND(FEChartsDataTableSnapshotCommand(this, Mode));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableExtractionTest,
    "EChartsWidget.DataTable.ExtractionTypesAndStableSort",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableExtractionTest::RunTest(const FString& Parameters)
{
	UDataTable* Table = NewObject<UDataTable>();
	Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
	FEChartsDataTableTestRow Row;
	Row.X = 4;
	Row.Y = 2;
	Row.Z = 7;
	Table->AddRow(TEXT("A"), Row);
	FEChartsDataTableSnapshot S;
	S.Mapping.Y = TEXT("Y");
	S.bCategory = true;
	for (const TCHAR* X : {TEXT("Category"), TEXT("Name"), TEXT("Text"), TEXT("Enum")})
	{
		S.Mapping.X = X;
		FEChartsDataTableRow Out;
		TestTrue(TEXT("String/name/text/enum extraction"), EChartsDataTableLoader::ReadRow(Table, TEXT("A"), S, Out));
		TestTrue(TEXT("Category is nonempty"), !Out.Category.IsEmpty());
	}
	S.bCategory = false;
	S.b3D = true;
	S.Mapping.X = TEXT("Integer");
	S.Mapping.Z = TEXT("Z");
	S.Mapping.Color = TEXT("X");
	S.Mapping.SymbolSize = TEXT("Y");
	FEChartsDataTableRow Out;
	TestTrue(TEXT("Integer 3D numeric fields"), EChartsDataTableLoader::ReadRow(Table, TEXT("A"), S, Out));
	TestEqual(TEXT("Integer extracted"), Out.Point.X, 9.0);
	TestEqual(TEXT("Mapped color"), Out.Point.ColorValue, 4.0);
	TestEqual(TEXT("Mapped symbol size"), Out.Point.SymbolSizeValue, 2.0);
	TestFalse(TEXT("Deleted row skipped"), EChartsDataTableLoader::ReadRow(Table, TEXT("Missing"), S, Out));
	Row.X = std::numeric_limits<double>::quiet_NaN();
	Table->AddRow(TEXT("A"), Row);
	TestFalse(TEXT("NaN mapped color skipped"), EChartsDataTableLoader::ReadRow(Table, TEXT("A"), S, Out));
	TArray<FEChartsDataTableRow> Rows;
	for (int32 I = 0; I < 3; ++I)
	{
		FEChartsDataTableRow R;
		R.Point.X = 1;
		R.Point.Y = I;
		Rows.Add(R);
	}
	auto Series = EChartsDataTableLoader::Convert(MoveTemp(Rows), false, false, EEChartsDataTableOrder::XAscending);
	for (int32 I = 0; I < 3; ++I)
		TestEqual(TEXT("Equal X stable order"), Series.Numeric2D[I].Y, static_cast<double>(I));
	return true;
}

class FEChartsDataTableRestartCommand : public IAutomationLatentCommand
{
	FAutomationTestBase* Test;
	TStrongObjectPtr<UEChartsWidget> Widget;
	double Start = 0;

  public:
	explicit FEChartsDataTableRestartCommand(FAutomationTestBase* T) : Test(T)
	{
	}
	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Widget.Reset(NewObject<UEChartsWidget>());
			Start = FPlatformTime::Seconds();
			Widget->InitializeECharts();
			UDataTable* Table = NewObject<UDataTable>();
			Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			FEChartsDataTableTestRow R;
			R.X = 999;
			Table->AddRow(TEXT("A"), R);
			FEChartsDataTableMapping M;
			M.X = TEXT("X");
			M.Y = TEXT("Y");
			Widget->SetDataTableMapping(Table, M);
			Widget->LoadDataTable(4096);
			FTSTicker::GetCoreTicker().Tick(0.01f);
			Test->TestEqual(
			    TEXT("First worker dispatched"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Processing);
			Widget->CancelDataTableLoad();
			R.X = 42;
			Table->AddRow(TEXT("A"), R);
			Widget->LoadDataTable(4096);
			return false;
		}
		if (FPlatformTime::Seconds() - Start > 10)
		{
			Test->AddError(TEXT("Restart timeout"));
			Widget->ReleaseSlateResources(false);
			return true;
		}
		if (Widget->DataTableLoadState != EEChartsDataTableLoadState::Applying)
			return false;
		auto Data = Widget->GetSeriesData(0);
		Test->TestEqual(TEXT("Restart data count"), Data.Num(), 1);
		if (Data.Num())
			Test->TestEqual(TEXT("Stale worker never overwrites restart"), Data[0].X, 42.0);
		Widget->CancelDataTableLoad();
		Test->TestEqual(TEXT("Pending cancellation restores prior cache"), Widget->GetSeriesData(0).Num(), 0);
		Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
		Widget->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_APPLIED__:1:2:1"), FString(), 0);
		Test->TestEqual(TEXT("Cancelled snapshot cannot complete"), Widget->DataTableLoadState,
		    EEChartsDataTableLoadState::Cancelled);
		Test->TestEqual(TEXT("Cancelled snapshot not submitted on Ready"), Widget->LastAppliedRevision, int64(0));
		Widget->ReleaseSlateResources(false);
		return true;
	}
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableRestartTest, "EChartsWidget.DataTable.RestartAndPendingCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableRestartTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FEChartsDataTableRestartCommand(this));
	return true;
}

class FEChartsDataTableCEFCommand : public IAutomationLatentCommand
{
	FAutomationTestBase* Test;
	int32 Mode;
	bool bProbe = false;
	TStrongObjectPtr<UEChartsWidget> Widget;
	TStrongObjectPtr<UEChartsWidgetTestSink> Sink;
	TSharedPtr<SWidget> Slate;
	double Start = 0;
	void Cleanup()
	{
		Slate.Reset();
		if (Widget.IsValid())
			Widget->ReleaseSlateResources(false);
	}

  public:
	FEChartsDataTableCEFCommand(FAutomationTestBase* T, int32 M) : Test(T), Mode(M)
	{
	}
	virtual bool Update() override
	{
		if (!Widget.IsValid())
		{
			Start = FPlatformTime::Seconds();
			Widget.Reset(NewObject<UEChartsWidget>());
			Sink.Reset(NewObject<UEChartsWidgetTestSink>());
			Widget->OnConsoleMessage.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleConsoleMessage);
			Widget->OnDataTableLoaded.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleTableLoaded);
			Slate = Widget->TakeWidget();
			Widget->InitializeECharts(
			    Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
			UDataTable* Table = NewObject<UDataTable>();
			Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
			FEChartsDataTableTestRow R;
			R.X = 2;
			R.Y = 20;
			R.Category = TEXT("Beta");
			Table->AddRow(TEXT("B"), R);
			R.X = 1;
			R.Y = 10;
			R.Category = TEXT("Alpha");
			Table->AddRow(TEXT("A"), R);
			FEChartsDataTableMapping M;
			M.X = Mode == 1 ? TEXT("Category") : TEXT("X");
			M.Y = TEXT("Y");
			M.Z = TEXT("Z");
			Widget->SetDataTableMapping(Table, M);
			Widget->LoadDataTable(1);
			return false;
		}
		if (Widget->DataTableLoadState == EEChartsDataTableLoadState::Error || FPlatformTime::Seconds() - Start > 30)
		{
			Test->AddError(FString::Printf(TEXT("CEF DataTable failed/timeout: %s"), *Widget->LastDataTableError));
			Cleanup();
			return true;
		}
		if (!bProbe && Widget->DataTableLoadState == EEChartsDataTableLoadState::Completed)
		{
			Test->TestEqual(TEXT("Real APPLIED loaded once"), Sink->TableLoadedCount, 1);
			Test->TestFalse(TEXT("DataTable applies with AutoApply disabled"), Widget->bAutoApplyEnabled);
			Test->TestTrue(TEXT("Loaded event on GameThread"), Sink->bTableEventsOnGameThread);
			FString Condition =
			    Mode == 0   ? TEXT("o.series[0].type==='line'&&JSON.stringify(o.series[0].data)==='[[1,10],[2,20]]'")
			    : Mode == 1 ? TEXT("o.xAxis[0].type==='category'&&JSON.stringify(o.xAxis[0].data)==='[\"Alpha\","
			                       "\"Beta\"]'&&JSON.stringify(o.series[0].data)==='[10,20]'")
			                : TEXT("o.series[0].type==='scatter3D'&&JSON.stringify(o.series[0].data)==='[[1,10,7,7,12],"
			                       "[2,20,7,7,12]]'");
			Widget->ExecuteJavascript(
			    FString::Printf(TEXT("(function(){var o=window.UEEChartsHost.getOptionForTesting();var "
			                         "ok=%s;console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+(ok?'OK':'BAD'));}());"),
			        *Condition));
			bProbe = true;
			return false;
		}
		if (bProbe && Sink->DataOptionReportCount > 0)
		{
			Test->TestTrue(TEXT("Real getOption matches DataTable snapshot"), Sink->bLastDataOptionSucceeded);
			Cleanup();
			return true;
		}
		return false;
	}
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableCEFTest, "EChartsWidget.Integration.CEFDataTable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableCEFTest::RunTest(const FString& Parameters)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI")))
	{
		AddInfo(TEXT("Real CEF DataTable requires D3D12."));
		return true;
	}
	for (int32 Mode = 0; Mode < 3; ++Mode)
		ADD_LATENT_AUTOMATION_COMMAND(FEChartsDataTableCEFCommand(this, Mode));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsDataTableRowNameTest, "EChartsWidget.DataTable.FNameOrderingAndTemplateChange",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsDataTableRowNameTest::RunTest(const FString& Parameters)
{
	UDataTable* Table = NewObject<UDataTable>();
	Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
	FEChartsDataTableTestRow Row;
	Row.X = 10;
	Table->AddRow(TEXT("Row_10"), Row);
	Row.X = 2;
	Table->AddRow(TEXT("Row_2"), Row);
	FEChartsDataTableSnapshot S;
	S.Mapping.X = TEXT("X");
	S.Mapping.Y = TEXT("Y");
	TArray<FEChartsDataTableRow> Rows;
	for (FName Name : Table->GetRowNames())
	{
		FEChartsDataTableRow R;
		EChartsDataTableLoader::ReadRow(Table, Name, S, R);
		Rows.Add(R);
	}
	auto Series = EChartsDataTableLoader::Convert(MoveTemp(Rows), false, false, EEChartsDataTableOrder::RowName);
	TestEqual(TEXT("RowName sorting follows FName numeric suffix ordering"), Series.Numeric2D[0].X, 2.0);
	TStrongObjectPtr<UEChartsWidget> Widget(NewObject<UEChartsWidget>());
	Widget->SetDataTableMapping(Table, S.Mapping);
	Widget->LoadDataTable(1);
	Widget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("Changed template fails before reading incompatible rows"), Widget->DataTableLoadState,
	    EEChartsDataTableLoadState::Error);
	TestEqual(TEXT("Changed template did not scan"), Widget->RowsProcessed, 0);
	Widget->InitializeECharts();
	Widget->SetDataTableMapping(Table, S.Mapping);
	TStrongObjectPtr<UEChartsWidgetTestSink> Sink(NewObject<UEChartsWidgetTestSink>());
	Sink->TableTemplateChangeWidget = Widget.Get();
	Widget->OnDataTableLoadProgress.AddDynamic(Sink.Get(), &UEChartsWidgetTestSink::HandleTableProgress);
	Widget->LoadDataTable(4096);
	FTSTicker::GetCoreTicker().Tick(0.01f);
	TestEqual(TEXT("Final progress callback changing template cannot dispatch incompatible worker"), Widget->DataTableLoadState, EEChartsDataTableLoadState::Error);
	Widget->ReleaseSlateResources(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsConcurrentPayloadTest, "EChartsWidget.DataTable.ConcurrentPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsConcurrentPayloadTest::RunTest(const FString& Parameters)
{
	FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting();
	ParallelFor(4096, [](int32 I)
	{
		TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> Series;
		Series[0].Type = EEChartsSeriesDataType::Numeric2D;
		Series[0].Numeric2D.Add({1, 2});
		FString Payload, Error;
		int32 Count;
		FEChartsPayloadBuilder::BuildBase64Payload(
		    EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, Series, I + 1, Payload, Count, Error);
	});
	TestEqual(TEXT("Concurrent serializers retain every instrumentation increment"),
	    FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting(), 4096);
	return true;
}
#endif
