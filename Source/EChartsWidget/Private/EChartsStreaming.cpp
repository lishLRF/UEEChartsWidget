#include "EChartsWidget.h"
#include "EChartsDataTableLoader.h"
#include "Async/Async.h"

bool UEChartsWidget::IsStreamingActive() const
{
	return StreamState == EEChartsDataTableStreamState::Preparing ||
		StreamState == EEChartsDataTableStreamState::Playing || StreamState == EEChartsDataTableStreamState::Paused;
}

void UEChartsWidget::SetTimeSeriesEnabled(bool bEnabled)
{
	if (!IsInGameThread()) return;
	bTimeSeriesEnabled = bEnabled;
	if (!bEnabled) StopDataTableStreaming();
}

void UEChartsWidget::SetTimeSeriesWindow(int32 MaxVisiblePoints)
{
	if (!IsInGameThread()) return;
	TimeSeriesWindow = FMath::Clamp(MaxVisiblePoints, 1, FEChartsPayloadBuilder::MaxPointCount);
	if (IsStreamingActive() && SeriesData[0].Num() > TimeSeriesWindow)
	{
		TrimStreamWindow();
		MarkDataChanged();
		if (StreamFinalRevision) StreamFinalRevision = DataRevision;
		ApplyEChartsChanges();
	}
}

void UEChartsWidget::TrimStreamWindow()
{
	if (!bTimeSeriesEnabled || !IsStreamingActive()) return;
	FEChartsSeriesData& S = SeriesData[0];
	const int32 Excess = S.Num() - TimeSeriesWindow;
	if (Excess <= 0) return;
	switch (S.Type)
	{
	case EEChartsSeriesDataType::Numeric2D: S.Numeric2D.RemoveAt(0, Excess, EAllowShrinking::No); break;
	case EEChartsSeriesDataType::Category: S.Category.RemoveAt(0, Excess, EAllowShrinking::No); break;
	case EEChartsSeriesDataType::Data3D: S.Data3D.RemoveAt(0, Excess, EAllowShrinking::No); break;
	default: break;
	}
}

bool UEChartsWidget::StartDataTableStreaming(float IntervalSeconds, int32 RowsPerStep, bool Loop, int32 PreparationRowsPerFrame)
{
	if (!IsInGameThread()) return false;
	if (!bTimeSeriesEnabled)
	{
		ReportDataError(TEXT("Enable Time Series before starting DataTable streaming."));
		return false;
	}
	StopStreaming(false);
	StreamInterval = FMath::Clamp(FMath::IsFinite(IntervalSeconds) ? IntervalSeconds : 0.1f, 0.01f, 60.0f);
	StreamRowsPerStep = FMath::Clamp(RowsPerStep, 1, 256);
	bStreamLoop = Loop;
	CurrentRow = 0; LoopCount = 0; StreamedRows = 0;
	LoadDataTable(PreparationRowsPerFrame);
	if (DataTableLoadState != EEChartsDataTableLoadState::Reading)
	{
		StreamState = EEChartsDataTableStreamState::Error;
		return false;
	}
	// The preparation shares the snapshot reader, but never owns a cache rollback or a bulk Apply.
	bHasDataTableCacheSnapshot = false;
	DataTablePreviousSeries = {};
	StreamState = EEChartsDataTableStreamState::Preparing;
	return true;
}

void UEChartsWidget::PrepareStreamRows(uint64 Request)
{
	DataTableLoadState = EEChartsDataTableLoadState::Processing;
	auto Snapshot = MoveTemp(DataTableSnapshot);
	const TWeakObjectPtr<UEChartsWidget> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Request, Snapshot = MoveTemp(Snapshot)]() mutable
	{
		auto Result = EChartsDataTableLoader::Convert(MoveTemp(Snapshot->Rows), Snapshot->bCategory,
			Snapshot->b3D, Snapshot->Mapping.Order);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, Result = MoveTemp(Result)]() mutable
		{
			UEChartsWidget* W = WeakThis.Get();
			if (!W || W->DataTableRequest != Request || W->StreamState != EEChartsDataTableStreamState::Preparing) return;
			if (W->RuntimeState == EEChartsRuntimeState::Error) { W->FailStreaming(W->LastError); return; }
			if (Result.Num() == 0) { W->FailStreaming(TEXT("DataTable contains no valid rows to stream.")); return; }
			if (W->SeriesData[0].Type != EEChartsSeriesDataType::Unset && W->SeriesData[0].Type != Result.Type)
			{
				W->FailStreaming(TEXT("Series 0 data type is incompatible with the DataTable stream. Clear it before starting."));
				return;
			}
			W->PreparedStreamRows = MoveTemp(Result);
			W->bPreserveStreamCategoryOrder = W->PreparedStreamRows.Type == EEChartsSeriesDataType::Category;
			W->XAxisMode = W->PreparedStreamRows.Type == EEChartsSeriesDataType::Category ? EEChartsXAxisMode::Category : EEChartsXAxisMode::ShowAll;
			W->DataTableLoadState = EEChartsDataTableLoadState::Idle;
			W->StreamState = EEChartsDataTableStreamState::Playing;
			W->TrimStreamWindow();
			const uint64 StreamRequest = W->StreamRequest;
			W->OnDataTableStreamingStarted.Broadcast();
			if (StreamRequest == W->StreamRequest) W->ScheduleStreamTicker();
		});
	});
}

void UEChartsWidget::CancelStreamTicker()
{
	if (StreamTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StreamTickerHandle);
		StreamTickerHandle.Reset();
	}
}

void UEChartsWidget::ScheduleStreamTicker()
{
	if (bStreamingSuspended || StreamState != EEChartsDataTableStreamState::Playing || StreamFinalRevision || StreamTickerHandle.IsValid()) return;
	const uint64 Request = StreamRequest;
	StreamTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return StreamStep(Request); }), StreamInterval);
}

bool UEChartsWidget::StreamStep(uint64 Request)
{
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	++StreamTickCallsForTesting;
#endif
	if (Request != StreamRequest || StreamState != EEChartsDataTableStreamState::Playing || bStreamingSuspended) return false;
	if (LastStreamFrame == GFrameCounter || LastDataTableReadFrame == GFrameCounter) return true;
	LastStreamFrame = GFrameCounter;
	if (CurrentTemplate != DataTableRequestTemplate) { FailStreaming(TEXT("Template changed during streaming.")); return false; }
	bool bCategory = false; FString Error;
	if (!EChartsDataTableLoader::Validate(MappedDataTable, DataTableMapping, CurrentTemplate, bCategory, Error)) { FailStreaming(Error); return false; }
	FEChartsSeriesData& Target = SeriesData[0];
	Target.Type = PreparedStreamRows.Type;
	const int32 End = FMath::Min(CurrentRow + StreamRowsPerStep, PreparedStreamRows.Num());
	const int32 Added = End - CurrentRow;
	const int32 Capacity = FMath::Min(TimeSeriesWindow, Target.Num() + Added);
	if (!CanReplacePointCount(0, Capacity)) { FailStreaming(TEXT("Stream exceeds the chart point limit across all series.")); return false; }
	for (; CurrentRow < End; ++CurrentRow)
	{
		switch (Target.Type)
		{
		case EEChartsSeriesDataType::Numeric2D: Target.Numeric2D.Add(PreparedStreamRows.Numeric2D[CurrentRow]); break;
		case EEChartsSeriesDataType::Category: Target.Category.Add(PreparedStreamRows.Category[CurrentRow]); break;
		case EEChartsSeriesDataType::Data3D: Target.Data3D.Add(PreparedStreamRows.Data3D[CurrentRow]); break;
		default: break;
		}
	}
	StreamedRows += Added;
	MarkDataChanged();
	const bool bAtEnd = CurrentRow == PreparedStreamRows.Num();
	if (bAtEnd && !bStreamLoop)
	{
		StreamFinalRevision = DataRevision;
		StreamTickerHandle.Reset();
	}
	ApplyEChartsChanges();
	if (Request != StreamRequest || !IsStreamingActive()) return false;
	OnDataTableStreamProgress.Broadcast(CurrentRow, PreparedStreamRows.Num(), LoopCount);
	if (Request != StreamRequest || !IsStreamingActive()) return false;
	if (bAtEnd && bStreamLoop)
	{
		CurrentRow = 0;
		++LoopCount;
		OnDataTableStreamLooped.Broadcast(LoopCount);
	}
	return Request == StreamRequest && StreamState == EEChartsDataTableStreamState::Playing && !StreamFinalRevision && !bStreamingSuspended;
}

void UEChartsWidget::PauseDataTableStreaming()
{
	if (!IsInGameThread() || StreamState != EEChartsDataTableStreamState::Playing) return;
	CancelStreamTicker();
	StreamState = EEChartsDataTableStreamState::Paused;
}

void UEChartsWidget::ResumeDataTableStreaming()
{
	if (!IsInGameThread() || StreamState != EEChartsDataTableStreamState::Paused) return;
	StreamState = EEChartsDataTableStreamState::Playing;
	ScheduleStreamTicker();
}

void UEChartsWidget::StopStreaming(bool bNotify)
{
	const bool bActive = IsStreamingActive();
	const bool bPreparing = StreamState == EEChartsDataTableStreamState::Preparing;
	++StreamRequest;
	CancelStreamTicker();
	StreamState = EEChartsDataTableStreamState::Stopped;
	StreamFinalRevision = 0;
	PreparedStreamRows = {};
	if (bPreparing) StopDataTableLoad(false);
	if (bActive && bNotify) OnDataTableStreamingStopped.Broadcast();
}

void UEChartsWidget::StopDataTableStreaming()
{
	if (IsInGameThread()) StopStreaming(true);
}

void UEChartsWidget::FailStreaming(const FString& Error)
{
	StopStreaming(false);
	StreamState = EEChartsDataTableStreamState::Error;
	ReportDataError(Error);
}

void UEChartsWidget::CompleteStreamIfAcknowledged(int64 Revision)
{
	if (!IsStreamingActive() || !StreamFinalRevision || Revision < StreamFinalRevision) return;
	CancelStreamTicker();
	StreamFinalRevision = 0;
	PreparedStreamRows = {};
	StreamState = EEChartsDataTableStreamState::Completed;
	OnDataTableStreamingCompleted.Broadcast();
}

void UEChartsWidget::ResumeStreamingAfterRebuild()
{
	if (!bStreamingSuspended) return;
	bStreamingSuspended = false;
	if (StreamState == EEChartsDataTableStreamState::Preparing && DataTableSnapshot && !DataTableTickerHandle.IsValid())
	{
		const uint64 Request = DataTableRequest;
		DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return ReadDataTableBatch(Request); }), 0.0001f);
	}
	ScheduleStreamTicker();
}
