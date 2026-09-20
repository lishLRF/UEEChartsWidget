#include "EChartsWidget.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "EChartsDataTableTestRow.h"
#include "EChartsWidgetTestSink.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingContractTest, "EChartsWidget.Streaming.Contract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingContractTest::RunTest(const FString& Parameters)
{
    UClass* Class = UEChartsWidget::StaticClass();
    for (const TCHAR* Name : {TEXT("SetTimeSeriesEnabled"), TEXT("SetTimeSeriesWindow"),
        TEXT("StartDataTableStreaming"), TEXT("PauseDataTableStreaming"),
        TEXT("ResumeDataTableStreaming"), TEXT("StopDataTableStreaming")})
        TestNotNull(Name, Class->FindFunctionByName(Name));
    for (const TCHAR* Name : {TEXT("StreamState"), TEXT("CurrentRow"), TEXT("LoopCount"), TEXT("StreamedRows"),
        TEXT("bTimeSeriesEnabled"), TEXT("TimeSeriesWindow"), TEXT("OnDataTableStreamingStarted"),
        TEXT("OnDataTableStreamProgress"), TEXT("OnDataTableStreamLooped"),
        TEXT("OnDataTableStreamingCompleted"), TEXT("OnDataTableStreamingStopped")})
        TestNotNull(Name, FindFProperty<FProperty>(Class, Name));
    TestNotNull(TEXT("Reflected stream state enum"), FindObject<UEnum>(nullptr, TEXT("/Script/EChartsWidget.EEChartsDataTableStreamState")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingDefaultsTest, "EChartsWidget.Streaming.DefaultsAndDisabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingDefaultsTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UEChartsWidget> W(NewObject<UEChartsWidget>());
    TStrongObjectPtr<UEChartsWidgetTestSink> S(NewObject<UEChartsWidgetTestSink>());
    W->OnEChartsError.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleError);
    TestFalse(TEXT("Opt in"), W->bTimeSeriesEnabled);
    TestEqual(TEXT("Default window"), W->TimeSeriesWindow, 1000);
    TestEqual(TEXT("Default state"), W->StreamState, EEChartsDataTableStreamState::Stopped);
    W->AddDataPoint(0, 9, 9);
    TestFalse(TEXT("Disabled start rejected"), W->StartDataTableStreaming());
    TestEqual(TEXT("Disabled emits error"), S->ErrorCount, 1);
    TestEqual(TEXT("Disabled preserves snapshot"), W->GetSeriesData(0).Num(), 1);
    W->SetTimeSeriesWindow(-1); TestEqual(TEXT("Lower window clamp"), W->TimeSeriesWindow, 1);
    W->SetTimeSeriesWindow(100001); TestEqual(TEXT("Upper window clamp"), W->TimeSeriesWindow, 100000);
    W->SetTimeSeriesEnabled(true); TestTrue(TEXT("Enable"), W->bTimeSeriesEnabled);
    W->ReleaseSlateResources(false);
    return true;
}

class FEChartsStreamCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    int32 Mode, Stage = 0;
    bool bLoop;
    double Start = 0, PhaseStart = 0;
    int64 SavedRows = 0;
    TStrongObjectPtr<UEChartsWidget> W;
    TStrongObjectPtr<UEChartsWidgetTestSink> S;
    int32 Count() const { return Mode == 1 ? W->GetCategorySeriesData(0).Num() : Mode == 2 ? W->Get3DData(0).Num() : W->GetSeriesData(0).Num(); }
    void Ack() { for (int32 I = 0; I < 100 && W->IsApplyInFlightForTesting(); ++I) W->AcknowledgeCurrentApplyForTesting(); }
    void Cleanup() { W->StopDataTableStreaming(); W->ReleaseSlateResources(false); }
public:
    FEChartsStreamCommand(FAutomationTestBase* T, int32 M, bool L) : Test(T), Mode(M), bLoop(L) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); S.Reset(NewObject<UEChartsWidgetTestSink>());
            W->OnDataTableStreamingStarted.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamStarted);
            W->OnDataTableStreamingStopped.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamStopped);
            W->OnDataTableStreamingCompleted.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamCompleted);
            W->OnDataTableStreamLooped.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamLooped);
            W->OnDataTableStreamProgress.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamProgress);
            W->OnDataTableLoadProgress.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleTableProgress);
            W->InitializeECharts(Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
            W->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 5; I > 0; --I) { FEChartsDataTableTestRow R; R.X = I; R.Y = I * 10; R.Category = FString::FromInt(I % 2); Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = Mode == 1 ? TEXT("Category") : TEXT("X"); M.Y = TEXT("Y"); M.Z = TEXT("Z");
            W->SetDataTableMapping(Table, M); W->SetTimeSeriesEnabled(true); W->SetTimeSeriesWindow(3);
            if (!Test->TestTrue(TEXT("Enabled start accepted"), W->StartDataTableStreaming(0.05f, 1, bLoop, 1))) { Cleanup(); return true; }
            Test->TestEqual(TEXT("Preparation is asynchronous"), W->StreamState, EEChartsDataTableStreamState::Preparing);
            Test->TestEqual(TEXT("No eager display"), Count(), 0);
            return false;
        }
        if (FPlatformTime::Seconds() - Start > 10) { Test->AddError(TEXT("Stream state transition timeout")); Cleanup(); return true; }
        if (Stage == 0 && W->StreamState == EEChartsDataTableStreamState::Playing)
        {
            Test->TestEqual(TEXT("Started once"), S->StreamStartedCount, 1);
            Test->TestTrue(TEXT("Preparation frame budget"), S->MaxTableBatch <= 1);
            Test->TestTrue(TEXT("No complete snapshot installation"), Count() <= 1);
            PhaseStart = FPlatformTime::Seconds(); Stage = 1;
        }
        if (Stage == 1 && W->StreamedRows > 0)
        {
            Test->TestEqual(TEXT("First timer appends fixed batch"), W->StreamedRows, int64(1));
            Test->TestTrue(TEXT("First streamed batch is submitted before full row preparation"), W->RowsProcessed < W->TotalRows);
            Test->TestTrue(TEXT("First streamed batch has an in-flight Apply"), W->IsApplyInFlightForTesting());
            Test->TestEqual(TEXT("No ACK required before next step"), W->LastAppliedRevision, int64(0));
            SavedRows = W->StreamedRows; W->PauseDataTableStreaming(); PhaseStart = FPlatformTime::Seconds(); Stage = 2;
        }
        if (Stage == 2 && FPlatformTime::Seconds() - PhaseStart > 0.15)
        {
            Test->TestEqual(TEXT("Pause holds cursor"), W->StreamedRows, SavedRows);
            Test->TestEqual(TEXT("Paused state"), W->StreamState, EEChartsDataTableStreamState::Paused);
            Ack(); Test->TestEqual(TEXT("ACK does not advance paused stream"), W->StreamedRows, SavedRows);
            W->ResumeDataTableStreaming(); Stage = 3;
        }
        if (Stage == 3 && W->StreamedRows < (bLoop ? 12 : 5)) W->AcknowledgeCurrentApplyForTesting();
        if (Stage == 3 && W->StreamedRows >= (bLoop ? 12 : 5))
        {
            Test->TestEqual(TEXT("Window bounded"), Count(), 3);
            Test->TestEqual(TEXT("Per-frame budget"), S->StreamSameFrameCount, 0);
            if (bLoop)
            {
                Test->TestTrue(TEXT("Loop counter advances"), W->LoopCount >= 2);
                Test->TestTrue(TEXT("Loop event"), S->StreamLoopedCount >= 2);
                Test->TestEqual(TEXT("Loop never completes"), S->StreamCompletedCount, 0);
                W->PauseDataTableStreaming();
                if (Mode == 0) { W->AddDataPoint(0, 99, 99); Test->TestEqual(TEXT("External add trims"), Count(), 3); }
                W->ClearSeries(0);
                Test->TestEqual(TEXT("Clear stops stream"), W->StreamState, EEChartsDataTableStreamState::Stopped);
                Test->TestEqual(TEXT("Stopped event once"), S->StreamStoppedCount, 1);
                Cleanup(); return true;
            }
            Test->TestEqual(TEXT("Final completion waits for ACK"), S->StreamCompletedCount, 0);
            Test->TestEqual(TEXT("All rows appended"), W->StreamedRows, int64(5));
            W->AcknowledgeRevisionForTesting(W->GetInFlightRevisionForTesting() + 1);
            Test->TestEqual(TEXT("Future revision cannot complete the stream"), S->StreamCompletedCount, 0);
            Ack(); Ack();
            Test->TestEqual(TEXT("Final ACK completes once"), S->StreamCompletedCount, 1);
            Test->TestEqual(TEXT("Completed state"), W->StreamState, EEChartsDataTableStreamState::Completed);
            if (Mode == 0)
            {
                Test->TestEqual(TEXT("Stable sorted suffix"), W->GetSeriesData(0)[0].X, 3.0);
                W->SetTimeSeriesWindow(5);
                Test->TestTrue(TEXT("Completed stream accepts ordinary Add"), W->AddDataPoint(0, 6.0, 60.0));
                Test->TestTrue(TEXT("Completed stream accepts ordinary Append"), W->AppendSeriesData(0, {{7.0, 70.0}}));
                const auto Grown = W->GetSeriesData(0);
                Test->TestEqual(TEXT("Completed cache grows beyond the old ring capacity"), Grown.Num(), 5);
                for (int32 I = 0; I < Grown.Num(); ++I)
                    Test->TestEqual(TEXT("Completed cache preserves original suffix then new points"), Grown[I].X, double(I + 3));
            }
            Cleanup(); return true;
        }
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingLifecycleTest, "EChartsWidget.Streaming.LifecycleAndWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingLifecycleTest::RunTest(const FString& Parameters)
{
    for (int32 Mode = 0; Mode < 3; ++Mode) for (bool Loop : {false, true})
        ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamCommand(this, Mode, Loop));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingBoundariesTest, "EChartsWidget.Streaming.PreparationAndMutationBoundaries",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingBoundariesTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UEChartsWidget> W(NewObject<UEChartsWidget>());
    TStrongObjectPtr<UEChartsWidgetTestSink> S(NewObject<UEChartsWidgetTestSink>());
    UDataTable* T = NewObject<UDataTable>(); T->RowStruct = FEChartsDataTableTestRow::StaticStruct();
    for (int32 I = 0; I < 5000; ++I) T->AddRow(FName(*FString::FromInt(I)), FEChartsDataTableTestRow());
    FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y");
    W->SetDataTableMapping(T, M); W->SetTimeSeriesEnabled(true); W->SetTimeSeriesWindow(2);
    W->OnDataTableStreamingStopped.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamStopped);
    W->InitializeECharts();
    W->StartDataTableStreaming(-1, -1, true, -1);
    TestEqual(TEXT("Interval lower clamp"), W->GetStreamIntervalForTesting(), 0.01f);
    TestEqual(TEXT("Batch lower clamp"), W->GetStreamRowsPerStepForTesting(), 1);
    FTSTicker::GetCoreTicker().Tick(0.01f);
    TestEqual(TEXT("Sort-key preparation lower budget"), W->GetStreamSortKeysProcessedForTesting(), 1);
    W->ReleaseSlateResources(false);
    TestFalse(TEXT("Release removes preparing handle"), W->IsDataTablePrepareScheduledForTesting());
    TestEqual(TEXT("Release preserves preparation intent"), W->StreamState, EEChartsDataTableStreamState::Preparing);
    W->PrepareRebuildForTesting();
    TestTrue(TEXT("Rebuild restores preparation handle"), W->IsDataTablePrepareScheduledForTesting());
    W->StopDataTableStreaming();
    TestFalse(TEXT("Stop removes preparing handle"), W->IsDataTablePrepareScheduledForTesting());
    W->StartDataTableStreaming(100, 1000, true, 10000);
    TestEqual(TEXT("Interval upper clamp"), W->GetStreamIntervalForTesting(), 60.0f);
    TestEqual(TEXT("Batch upper clamp"), W->GetStreamRowsPerStepForTesting(), 256);
    W->SetSeriesData(0, {{1,1},{2,2},{3,3},{4,4}});
    TestEqual(TEXT("Set stops stream"), W->StreamState, EEChartsDataTableStreamState::Stopped);
    TestEqual(TEXT("Set replacement is intact after stop"), W->GetSeriesData(0).Num(), 4);
    W->StartDataTableStreaming(); W->SetDataTableMapping(T, M);
    TestEqual(TEXT("Mapping change stops"), W->StreamState, EEChartsDataTableStreamState::Stopped);
    W->StartDataTableStreaming(); W->InitializeECharts(EEChartsTemplate::DataTableScatter3D);
    TestEqual(TEXT("Template change stops"), W->StreamState, EEChartsDataTableStreamState::Stopped);
    W->InitializeECharts(); W->StartDataTableStreaming();
    W->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_ERROR__:4:browser failed"), FString(), 0);
    TestEqual(TEXT("Runtime failure stops preparing stream"), W->StreamState, EEChartsDataTableStreamState::Error);
    TestFalse(TEXT("Runtime failure removes preparation ticker"), W->IsDataTablePrepareScheduledForTesting());
    W->StopDataTableStreaming(); W->ReleaseSlateResources(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingSourceLimitTest, "EChartsWidget.Streaming.SourceLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingSourceLimitTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UEChartsWidget> W(NewObject<UEChartsWidget>());
    TStrongObjectPtr<UDataTable> T(NewObject<UDataTable>());
    T->RowStruct = FEChartsDataTableTestRow::StaticStruct();
    FEChartsDataTableTestRow R;
    for (int32 I = 0; I <= FEChartsPayloadBuilder::MaxPointCount; ++I)
        T->AddRow(FName(*FString::FromInt(I)), R);
    FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y");
    TestTrue(TEXT("Oversized table maps before stream-specific validation"), W->SetDataTableMapping(T.Get(), M));
    W->SetTimeSeriesEnabled(true);
    TestFalse(TEXT("Stream rejects more than 100000 source rows before preparation"), W->StartDataTableStreaming());
    TestEqual(TEXT("Oversized source enters Error state"), W->StreamState, EEChartsDataTableStreamState::Error);
    TestFalse(TEXT("Oversized source never schedules preparation"), W->IsDataTablePrepareScheduledForTesting());
    TestEqual(TEXT("Oversized source is rejected before DataTable snapshot/GetRowNames"), W->GetDataTableSnapshotCreationCountForTesting(), 0);
    TStrongObjectPtr<UDataTable> LongCategoryTable(NewObject<UDataTable>());
    LongCategoryTable->RowStruct = FEChartsDataTableTestRow::StaticStruct();
    FEChartsDataTableTestRow LongRow;
    LongRow.Category = FString::ChrN(FEChartsPayloadBuilder::MaxJsonBytes + 1, TEXT('A'));
    LongCategoryTable->AddRow(TEXT("Long"), LongRow);
    M.X = TEXT("Category");
    TestTrue(TEXT("Long category table maps"), W->SetDataTableMapping(LongCategoryTable.Get(), M));
    TestTrue(TEXT("Long category begins incremental validation"), W->StartDataTableStreaming(0.1f, 1, false, 1));
    FTSTicker::GetCoreTicker().Tick(0.1f);
    TestEqual(TEXT("Long category source fails the byte budget before prepared caching"),
        W->StreamState, EEChartsDataTableStreamState::Error);
    TestEqual(TEXT("Long category is never retained in prepared source"), W->GetPreparedStreamCountForTesting(), 0);
    TestEqual(TEXT("Long FString is rejected before copying a category sort key"), W->GetStreamCategorySortKeyCopyCountForTesting(), 0);
    W->ReleaseSlateResources(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingPreparingMutationLimitTest, "EChartsWidget.Streaming.PreparingMutationLimit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingPreparingMutationLimitTest::RunTest(const FString& Parameters)
{
    TStrongObjectPtr<UEChartsWidget> W(NewObject<UEChartsWidget>());
    TStrongObjectPtr<UDataTable> T(NewObject<UDataTable>()); T->RowStruct = FEChartsDataTableTestRow::StaticStruct();
    FEChartsDataTableTestRow R; T->AddRow(TEXT("Only"), R);
    FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y"); W->SetDataTableMapping(T.Get(), M);
    W->SetTimeSeriesEnabled(true); W->SetTimeSeriesWindow(3);
    TestTrue(TEXT("Stream enters Preparing"), W->StartDataTableStreaming(1.0f, 1, false, 1));
    TestEqual(TEXT("Mutation is tested before ring configuration"), W->StreamState, EEChartsDataTableStreamState::Preparing);
    TArray<FEChartsDataPoint2D> Oversized; Oversized.SetNum(FEChartsPayloadBuilder::MaxPointCount + 1);
    const int32 Before = W->GetSeriesData(0).Num();
    TestFalse(TEXT("Preparing stream rejects an append beyond the global point limit"), W->AppendSeriesData(0, Oversized));
    TestEqual(TEXT("Rejected Preparing append is atomic"), W->GetSeriesData(0).Num(), Before);
    W->StopDataTableStreaming(); W->ReleaseSlateResources(false);
    return true;
}

class FEChartsStreamRebuildCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    int32 Stage = 0, Action;
    int64 SavedRows = 0;
    double Start = 0, PhaseStart = 0;
    TStrongObjectPtr<UEChartsWidget> W;
    TStrongObjectPtr<UEChartsWidgetTestSink> S;
public:
    FEChartsStreamRebuildCommand(FAutomationTestBase* T, int32 A) : Test(T), Action(A) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); S.Reset(NewObject<UEChartsWidgetTestSink>());
            W->InitializeECharts(); W->SetTimeSeriesEnabled(true);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 0; I < 10; ++I) { FEChartsDataTableTestRow R; R.X = I; Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y"); W->SetDataTableMapping(Table, M);
            S->StreamCallbackWidget = W.Get(); S->StreamCallbackAction = Action; S->StreamRestartsRemaining = 8;
            W->OnDataTableStreamProgress.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamProgress);
            W->StartDataTableStreaming(0.01f, 1, true, 1); return false;
        }
        if (FPlatformTime::Seconds() - Start > 10) { Test->AddError(TEXT("Rebuild/reentrant stream timeout")); W->StopDataTableStreaming(); W->ReleaseSlateResources(false); return true; }
        if (Stage == 0 && S->StreamProgressCount >= (Action == 3 ? 10 : 1))
        {
            Test->TestEqual(TEXT("No same-frame callback recursion"), S->StreamSameFrameCount, 0);
            if (Action == 2)
            {
                Test->TestEqual(TEXT("Callback stop holds stopped"), W->StreamState, EEChartsDataTableStreamState::Stopped);
                Test->TestFalse(TEXT("Callback stop removes playing ticker"), W->IsDataTableStreamScheduledForTesting());
                W->ReleaseSlateResources(false); return true;
            }
            if (Action == 1) Test->TestEqual(TEXT("Callback pause holds paused"), W->StreamState, EEChartsDataTableStreamState::Paused);
            SavedRows = W->StreamedRows;
            W->ReleaseSlateResources(false);
            Test->TestFalse(TEXT("Release removes stream ticker"), W->IsDataTableStreamScheduledForTesting());
            PhaseStart = FPlatformTime::Seconds(); Stage = 1;
        }
        if (Stage == 1 && FPlatformTime::Seconds() - PhaseStart > 0.08)
        {
            Test->TestEqual(TEXT("Released widget does not advance"), W->StreamedRows, SavedRows);
            S->StreamCallbackAction = 0;
            W->PrepareRebuildForTesting(); W->PrepareRebuildForTesting();
            if (Action == 1)
            {
                Test->TestFalse(TEXT("Paused rebuild never installs playing ticker"), W->IsDataTableStreamScheduledForTesting());
                W->ResumeDataTableStreaming();
            }
            Test->TestTrue(TEXT("Playing rebuild restores ticker"), W->IsDataTableStreamScheduledForTesting());
            W->InitializeECharts(); Test->TestEqual(TEXT("Same-template Initialize keeps cursor"), W->StreamedRows, SavedRows);
            Stage = 2;
        }
        if (Stage == 2 && W->StreamedRows >= SavedRows + 10)
        {
            Test->TestEqual(TEXT("Rebuild no same-frame duplicate advancement"), S->StreamSameFrameCount, 0);
            if (Action == 4) Test->TestEqual(TEXT("Pause/resume callback never leaves duplicate timer delegates"), W->GetSameFrameStreamTickCallsForTesting(), 0);
            W->StopDataTableStreaming(); Test->TestFalse(TEXT("Stop releases timer"), W->IsDataTableStreamScheduledForTesting());
            Test->TestEqual(TEXT("Stop releases prepared cache"), W->GetPreparedStreamCountForTesting(), 0);
            Test->TestTrue(TEXT("Stop keeps display cache"), W->GetSeriesData(0).Num() > 0);
            W->ReleaseSlateResources(false); return true;
        }
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingRebuildTest, "EChartsWidget.Streaming.RebuildAndCallbacks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingRebuildTest::RunTest(const FString& Parameters)
{
    for (int32 Action : {0, 1, 2, 3, 4}) ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamRebuildCommand(this, Action));
    return true;
}

class FEChartsStreamBackpressureReentryCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    TStrongObjectPtr<UEChartsWidget> W;
    TStrongObjectPtr<UEChartsWidgetTestSink> S;
    double Start = 0, BackpressureStart = 0;
    int32 SavedTicks = 0;
    bool bObserved = false;
public:
    explicit FEChartsStreamBackpressureReentryCommand(FAutomationTestBase* T) : Test(T) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); S.Reset(NewObject<UEChartsWidgetTestSink>());
            W->InitializeECharts(); W->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
            W->SetTimeSeriesEnabled(true);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 0; I < 10; ++I) { FEChartsDataTableTestRow R; R.X = I; Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y"); W->SetDataTableMapping(Table, M);
            S->StreamCallbackWidget = W.Get(); S->StreamCallbackAction = 4; S->StreamCallbackAfterProgressCount = 3;
            W->OnDataTableStreamProgress.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamProgress);
            W->StartDataTableStreaming(0.01f, 1, true, 10);
            return false;
        }
        if (FPlatformTime::Seconds() - Start > 10) { Test->AddError(TEXT("Backpressure reentry timeout")); W->StopDataTableStreaming(); return true; }
        if (!bObserved && S->StreamProgressCount >= 3)
        {
            bObserved = true; BackpressureStart = FPlatformTime::Seconds(); SavedTicks = W->GetStreamTickCallsForTesting();
            Test->TestTrue(TEXT("Two unacknowledged delta batches reach browser backpressure"), W->GetPendingStreamDeltaCountForTesting() >= 2);
        }
        if (bObserved && FPlatformTime::Seconds() - BackpressureStart > 0.1)
        {
            Test->TestTrue(TEXT("Reentrant pause/resume never grows the bounded delta queue"), W->GetPendingStreamDeltaCountForTesting() <= 2);
            Test->TestEqual(TEXT("No orphan ticker runs after backpressure"), W->GetStreamTickCallsForTesting(), SavedTicks);
            W->StopDataTableStreaming();
            Test->TestFalse(TEXT("Stop leaves no tracked ticker"), W->IsDataTableStreamScheduledForTesting());
            W->ReleaseSlateResources(false); return true;
        }
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingBackpressureReentryTest, "EChartsWidget.Streaming.BackpressureReentry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingBackpressureReentryTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamBackpressureReentryCommand(this));
    return true;
}

class FEChartsStreamRevisionWrapCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    TStrongObjectPtr<UEChartsWidget> W;
    TStrongObjectPtr<UEChartsWidgetTestSink> S;
    int32 Stage = 0, ProgressAtWrap = 0;
    double Start = 0;
public:
    explicit FEChartsStreamRevisionWrapCommand(FAutomationTestBase* T) : Test(T) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); S.Reset(NewObject<UEChartsWidgetTestSink>());
            W->InitializeECharts(); W->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
            W->SetTimeSeriesEnabled(true);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 0; I < 4; ++I) { FEChartsDataTableTestRow R; R.X = I; Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y"); W->SetDataTableMapping(Table, M);
            W->OnDataTableStreamProgress.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamProgress);
            W->StartDataTableStreaming(0.01f, 1, true, 4); return false;
        }
        if (FPlatformTime::Seconds() - Start > 10) { Test->AddError(TEXT("Revision wrap timeout")); W->StopDataTableStreaming(); return true; }
        if (W->RuntimeState == EEChartsRuntimeState::Error || W->StreamState == EEChartsDataTableStreamState::Error)
        {
            Test->AddError(TEXT("Revision wrap must not enter Error before forcing a full payload"));
            W->StopDataTableStreaming(); W->ReleaseSlateResources(false); return true;
        }
        if (Stage == 0 && S->StreamProgressCount >= 1 && W->IsApplyInFlightForTesting())
        {
            W->AcknowledgeCurrentApplyForTesting();
            W->SetDataRevisionForTesting(9007199254740991LL);
            ProgressAtWrap = S->StreamProgressCount; Stage = 1;
        }
        if (Stage == 1 && S->StreamProgressCount > ProgressAtWrap && W->GetInFlightRevisionForTesting() == 1)
        {
            Test->TestFalse(TEXT("Revision wrap forces a full payload"), W->WasLastSubmitDeltaForTesting());
            W->AcknowledgeCurrentApplyForTesting();
            ProgressAtWrap = S->StreamProgressCount; Stage = 2;
        }
        if (Stage == 2 && S->StreamProgressCount > ProgressAtWrap && W->IsApplyInFlightForTesting())
        {
            Test->TestTrue(TEXT("Delta resumes after exact ACK of wrapped full revision"), W->WasLastSubmitDeltaForTesting());
            Test->TestTrue(TEXT("Revision wrap leaves runtime out of Error"), W->RuntimeState != EEChartsRuntimeState::Error);
            W->StopDataTableStreaming(); W->ReleaseSlateResources(false); return true;
        }
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingRevisionWrapTest, "EChartsWidget.Streaming.RevisionWrap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingRevisionWrapTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamRevisionWrapCommand(this));
    return true;
}

class FEChartsStreamMemoryCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    TStrongObjectPtr<UEChartsWidget> W;
    double Start = 0;
    uint32 WarmBytes = 0;
public:
    explicit FEChartsStreamMemoryCommand(FAutomationTestBase* T) : Test(T) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); W->SetTimeSeriesEnabled(true);
            W->InitializeECharts(); W->OnConsoleMessage.Broadcast(TEXT("__UE_ECHARTS_READY__:1"), FString(), 0);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 0; I < 333; ++I) { FEChartsDataTableTestRow R; R.X = I; R.Y = I; Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = TEXT("X"); M.Y = TEXT("Y"); W->SetDataTableMapping(Table, M);
            W->StartDataTableStreaming(0.01f, 256, true, 4096); return false;
        }
        W->AcknowledgeCurrentApplyForTesting();
        if (FPlatformTime::Seconds() - Start > 15) { Test->AddError(TEXT("Loop memory test timeout")); W->StopDataTableStreaming(); return true; }
        Test->TestTrue(TEXT("Every loop callback remains within default 1000 window"), W->GetSeriesData(0).Num() <= 1000);
        if (W->LoopCount >= 10 && WarmBytes == 0) WarmBytes = W->GetNumericStreamAllocatedBytesForTesting();
        if (W->LoopCount < 100) return false;
        Test->TestEqual(TEXT("One hundred loops use same memory as warm window"), W->GetNumericStreamAllocatedBytesForTesting(), WarmBytes);
        Test->TestEqual(TEXT("Prepared source never grows across loops"), W->GetPreparedStreamCountForTesting(), 333);
        Test->TestEqual(TEXT("Default window retains exact 1000 points"), W->GetSeriesData(0).Num(), 1000);
        Test->TestTrue(TEXT("Physical streaming cache stays within two windows plus one batch"), W->GetStreamPhysicalPointCountForTesting() <= 2256);
        Test->TestTrue(TEXT("Steady-state stream submission uses delta protocol"), W->WasLastSubmitDeltaForTesting());
        Test->TestTrue(TEXT("One-batch delta command remains small"), W->GetLastSubmitCommandLengthForTesting() < 65536);
        W->StopDataTableStreaming(); W->ReleaseSlateResources(false); return true;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingMemoryTest, "EChartsWidget.Streaming.BoundedLoopMemory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingMemoryTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamMemoryCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingRingBufferTest, "EChartsWidget.Streaming.RingBuffer100K",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingRingBufferTest::RunTest(const FString& Parameters)
{
    FEChartsSeriesData Series;
    Series.Type = EEChartsSeriesDataType::Numeric2D;
    Series.Numeric2D.Reserve(FEChartsPayloadBuilder::MaxPointCount * 2 + 1);
    for (int32 I = 0; I < FEChartsPayloadBuilder::MaxPointCount; ++I) Series.Numeric2D.Add({double(I), double(I)});
    Series.ConfigureRing(FEChartsPayloadBuilder::MaxPointCount);
    const FEChartsDataPoint2D* StableData = Series.Numeric2D.GetData();
    const SIZE_T StableAllocatedBytes = Series.Numeric2D.GetAllocatedSize();
    for (int32 I = 0; I < FEChartsPayloadBuilder::MaxPointCount; ++I)
    {
        Series.AddNumericRing({double(I + FEChartsPayloadBuilder::MaxPointCount), double(I)});
    }
    TestEqual(TEXT("Logical 100k window stays exact"), Series.Num(), FEChartsPayloadBuilder::MaxPointCount);
    TestEqual(TEXT("True ring physical count equals capacity"), Series.PhysicalNum(), FEChartsPayloadBuilder::MaxPointCount);
    TestTrue(TEXT("True ring keeps the underlying allocation pointer stable"), Series.Numeric2D.GetData() == StableData);
    TestEqual(TEXT("True ring keeps allocated bytes stable"), Series.Numeric2D.GetAllocatedSize(), StableAllocatedBytes);
    TestEqual(TEXT("Logical head hides the stale prefix"), Series.NumericAt(0).X,
        double(FEChartsPayloadBuilder::MaxPointCount));
    FEChartsSeriesData Delta; Delta.Type = EEChartsSeriesDataType::Numeric2D; Delta.Numeric2D.Add({200000.0, 1.0});
    FString Base64, Error;
    TestTrue(TEXT("Single-point delta serializes"), FEChartsPayloadBuilder::BuildStreamDeltaBase64(
        Delta, 1, 1, 2, Base64, Error));
    TestTrue(TEXT("Single-point delta encoding is batch-sized"), Base64.Len() < 2048);
    TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> FullSeries;
    FullSeries[0] = Series;
    FString FullBase64; int32 FullCount = 0;
    TestTrue(TEXT("Logical 100k window full payload serializes"), FEChartsPayloadBuilder::BuildBase64Payload(
        EEChartsTemplate::SegmentedAreaLine, EEChartsXAxisMode::ShowAll, FullSeries, 3,
        FullBase64, FullCount, Error));
    TestEqual(TEXT("Full payload sees only the logical 100k window"), FullCount, FEChartsPayloadBuilder::MaxPointCount);
    TestTrue(TEXT("Single-step delta is independent of the 100k full payload size"), Base64.Len() * 100 < FullBase64.Len());
    return true;
}

class FEChartsStreamCEFCommand : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;
    int32 Mode, Stage = 0;
    bool bLoop;
    double Start = 0;
    int64 PausedRows = 0;
    TStrongObjectPtr<UEChartsWidget> W;
    TStrongObjectPtr<UEChartsWidgetTestSink> S;
    TSharedPtr<SWidget> Slate;
    void Cleanup() { W->StopDataTableStreaming(); Slate.Reset(); W->ReleaseSlateResources(false); }
    void Probe(bool bFinal)
    {
        FString Expected = TEXT("[");
        FString Categories = TEXT("[");
        int32 Count = 0;
        if (Mode == 1)
        {
            const auto Points = W->GetCategorySeriesData(0); Count = Points.Num();
            for (int32 I = 0; I < Count; ++I) { if (I) { Expected += TEXT(","); Categories += TEXT(","); } Expected += FString::Printf(TEXT("%.0f"), Points[I].Y); Categories += TEXT("\"") + Points[I].X + TEXT("\""); }
            FString Reference = TEXT("[");
            const TArray<FEChartsCategoryDataPoint> Other = {{TEXT("0"), 900.0}, {TEXT("C"), 4.0}};
            TSet<FString> StreamLabels;
            for (const auto& Point : Points) StreamLabels.Add(Point.X);
            for (const auto& Point : Other)
            {
                if (!StreamLabels.Contains(Point.X))
                {
                    if (Count++) { Expected += TEXT(","); Categories += TEXT(","); }
                    Expected += TEXT("null"); Categories += TEXT("\"") + Point.X + TEXT("\"");
                }
            }
            for (int32 I = 0; I < Count; ++I)
            {
                if (I) Reference += TEXT(",");
                const FString Label = I < Points.Num() ? Points[I].X : (I - Points.Num() == 0 && !StreamLabels.Contains(TEXT("0")) ? TEXT("0") : TEXT("C"));
                Reference += Label == TEXT("0") ? TEXT("900") : Label == TEXT("C") ? TEXT("4") : TEXT("null");
            }
            Reference += TEXT("]");
            Expected += TEXT("]"); Categories += TEXT("]");
            Test->TestEqual(bFinal ? TEXT("Final window length") : TEXT("First visible row length"), Points.Num(), bFinal ? 3 : 1);
            FString Condition = FString::Printf(TEXT("o.series[0].data.length===o.xAxis[0].data.length&&JSON.stringify(o.series[0].data)==='%s'&&JSON.stringify(o.xAxis[0].data)==='%s'&&JSON.stringify(o.series[1].data)==='%s'"), *Expected, *Categories, *Reference);
            W->ExecuteJavascript(FString::Printf(TEXT("(function(){var o=window.UEEChartsHost.getOptionForTesting();console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+((%s)?'OK':'BAD'));}());"), *Condition));
            return;
        }
        else if (Mode == 2)
        {
            const auto Points = W->Get3DData(0); Count = Points.Num();
            for (int32 I = 0; I < Count; ++I) { if (I) Expected += TEXT(","); const auto& P = Points[I]; Expected += FString::Printf(TEXT("[%.0f,%.0f,%.0f,%.0f,%.0f]"), P.X, P.Y, P.Z, P.ColorValue, P.SymbolSizeValue); }
        }
        else
        {
            const auto Points = W->GetSeriesData(0); Count = Points.Num();
            for (int32 I = 0; I < Count; ++I) { if (I) Expected += TEXT(","); Expected += FString::Printf(TEXT("[%.0f,%.0f]"), Points[I].X, Points[I].Y); }
        }
        Expected += TEXT("]"); Categories += TEXT("]");
        Test->TestEqual(bFinal ? TEXT("Final window length") : TEXT("First visible row length"), Count, bFinal ? 3 : 1);
        FString Condition = FString::Printf(TEXT("o.series[0].data.length<=3&&JSON.stringify(o.series[0].data)==='%s'"), *Expected);
        if (Mode == 1) Condition += FString::Printf(TEXT("&&JSON.stringify(o.xAxis[0].data)==='%s'"), *Categories);
        W->ExecuteJavascript(FString::Printf(TEXT("(function(){var o=window.UEEChartsHost.getOptionForTesting();console.log('__UE_ECHARTS_TEST_DATA_OPTION__:1:'+((%s)?'OK':'BAD'));}());"), *Condition));
    }
public:
    FEChartsStreamCEFCommand(FAutomationTestBase* T, int32 M, bool L) : Test(T), Mode(M), bLoop(L) {}
    virtual bool Update() override
    {
        if (!W.IsValid())
        {
            Start = FPlatformTime::Seconds(); W.Reset(NewObject<UEChartsWidget>()); S.Reset(NewObject<UEChartsWidgetTestSink>());
            W->OnConsoleMessage.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleConsoleMessage);
            W->OnDataTableStreamingCompleted.AddDynamic(S.Get(), &UEChartsWidgetTestSink::HandleStreamCompleted);
            Slate = W->TakeWidget(); W->InitializeECharts(Mode == 2 ? EEChartsTemplate::DataTableScatter3D : EEChartsTemplate::SegmentedAreaLine);
            UDataTable* Table = NewObject<UDataTable>(); Table->RowStruct = FEChartsDataTableTestRow::StaticStruct();
            for (int32 I = 5; I > 0; --I) { FEChartsDataTableTestRow R; R.X = I; R.Y = I * 10; R.Category = FString::FromInt(I % 2); Table->AddRow(FName(*FString::FromInt(I)), R); }
            FEChartsDataTableMapping M; M.X = Mode == 1 ? TEXT("Category") : TEXT("X"); M.Y = TEXT("Y"); M.Z = TEXT("Z"); W->SetDataTableMapping(Table, M);
            if (Mode == 1) W->SetCategorySeriesData(1, {{TEXT("0"), 900.0}, {TEXT("C"), 4.0}});
            W->SetTimeSeriesEnabled(true); W->SetTimeSeriesWindow(3); return false;
        }
        if (FPlatformTime::Seconds() - Start > 40 || W->StreamState == EEChartsDataTableStreamState::Error)
        { Test->AddError(FString::Printf(TEXT("CEF stream failed/timeout at stage %d: %s"), Stage, *W->LastError)); Cleanup(); return true; }
        if (Stage == 0 && W->RuntimeState == EEChartsRuntimeState::Ready)
        { W->StartDataTableStreaming(0.3f, 1, bLoop, 1); Stage = 1; }
        if (Stage == 1 && W->LastAppliedPointCount == (Mode == 1 ? 3 : 1))
        {
            Test->TestTrue(TEXT("Real CEF applies the first row before full row preparation"), W->RowsProcessed < W->TotalRows);
            W->PauseDataTableStreaming(); Probe(false); Stage = 2;
        }
        if (Stage == 2 && S->DataOptionReportCount == 1)
        {
            Test->TestTrue(TEXT("Real CEF displays first streamed row before full table"), S->bLastDataOptionSucceeded);
            PausedRows = W->StreamedRows;
            Slate.Reset(); W->ReleaseSlateResources(false); Slate = W->TakeWidget(); Stage = 3;
        }
        if (Stage == 3 && W->RuntimeState == EEChartsRuntimeState::Ready && !W->bIsDirty)
        {
            Test->TestEqual(TEXT("Real rebuild retains paused row index"), W->StreamedRows, PausedRows);
            Test->TestEqual(TEXT("Real rebuild remains paused"), W->StreamState, EEChartsDataTableStreamState::Paused);
            W->ResumeDataTableStreaming(); Stage = 4;
        }
        if (Stage == 4 && ((!bLoop && W->StreamState == EEChartsDataTableStreamState::Completed) || (bLoop && W->StreamedRows >= 8)))
        { W->PauseDataTableStreaming(); Stage = 5; }
        if (Stage == 5 && !W->bIsDirty)
        {
            Test->TestEqual(TEXT("Only nonloop receives final-ACK Completed"), S->StreamCompletedCount, bLoop ? 0 : 1);
            Test->TestFalse(TEXT("Streaming applies without AutoApply"), W->bAutoApplyEnabled);
            Test->TestTrue(TEXT("Real CEF uses bounded stream-delta submissions after the first full payload"), W->WasLastSubmitDeltaForTesting());
            if (bLoop) Test->TestTrue(TEXT("Real loop counter"), W->LoopCount >= 1);
            Probe(true); Stage = 6;
        }
        if (Stage == 6 && S->DataOptionReportCount == 2)
        { Test->TestTrue(TEXT("Real CEF getOption equals bounded C++ stream window"), S->bLastDataOptionSucceeded); Cleanup(); return true; }
        return false;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEChartsStreamingCEFTest, "EChartsWidget.Integration.CEFStreaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FEChartsStreamingCEFTest::RunTest(const FString& Parameters)
{
    if (FParse::Param(FCommandLine::Get(), TEXT("NullRHI"))) { AddInfo(TEXT("CEF stream requires D3D12.")); return true; }
    for (int32 Mode = 0; Mode < 3; ++Mode) for (bool Loop : {false, true}) ADD_LATENT_AUTOMATION_COMMAND(FEChartsStreamCEFCommand(this, Mode, Loop));
    return true;
}
#endif
