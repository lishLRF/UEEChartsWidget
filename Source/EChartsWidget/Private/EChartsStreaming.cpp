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
	S.TrimFront(Excess);
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
	InvalidateStreamDelta();
	StreamInterval = FMath::Clamp(FMath::IsFinite(IntervalSeconds) ? IntervalSeconds : 0.1f, 0.01f, 60.0f);
	StreamRowsPerStep = FMath::Clamp(RowsPerStep, 1, 256);
	bStreamLoop = Loop;
	bStreamProductionComplete = false;
	StreamSourceJsonBytes = 1024;
	CurrentRow = 0; LoopCount = 0; StreamedRows = 0;
	LoadDataTable(PreparationRowsPerFrame);
	if (DataTableLoadState != EEChartsDataTableLoadState::Reading)
	{
		StreamState = EEChartsDataTableStreamState::Error;
		return false;
	}
	if (TotalRows > FEChartsPayloadBuilder::MaxPointCount)
	{
		StopDataTableLoad(false);
		StreamState = EEChartsDataTableStreamState::Error;
		ReportDataError(FString::Printf(TEXT("DataTable stream source exceeds the %d row limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	// The preparation shares the snapshot reader, but never owns a cache rollback or a bulk Apply.
	bHasDataTableCacheSnapshot = false;
	DataTablePreviousSeries = {};
	DataTableSnapshot->bStreaming = true;
	PreparedStreamRows.Type = DataTableSnapshot->b3D ? EEChartsSeriesDataType::Data3D
		: DataTableSnapshot->bCategory ? EEChartsSeriesDataType::Category : EEChartsSeriesDataType::Numeric2D;
	StreamState = EEChartsDataTableStreamState::Preparing;
	return true;
}

void UEChartsWidget::SortStreamRowNames(const uint64 Request)
{
	DataTableLoadState = EEChartsDataTableLoadState::Processing;
	auto Snapshot = DataTableSnapshot;
	auto SortKeys = MoveTemp(Snapshot->Rows);
	const bool bCategory = Snapshot->bCategory;
	const EEChartsDataTableOrder Order = Snapshot->Mapping.Order;
	const TWeakObjectPtr<UEChartsWidget> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Request, SortKeys = MoveTemp(SortKeys), bCategory, Order]() mutable
	{
		auto RowNames = EChartsDataTableLoader::SortRowNames(MoveTemp(SortKeys), bCategory, Order);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, RowNames = MoveTemp(RowNames)]() mutable
		{
			UEChartsWidget* W = WeakThis.Get();
			if (!W || W->DataTableRequest != Request || !W->DataTableSnapshot ||
				!W->DataTableSnapshot->bStreaming || !W->IsStreamingActive()) return;
			if (W->RuntimeState == EEChartsRuntimeState::Error) { W->FailStreaming(W->LastError); return; }
			W->DataTableSnapshot->RowNames = MoveTemp(RowNames);
			W->DataTableSnapshot->Rows.Reset();
			W->DataTableSnapshot->bSortKeysReady = true;
			W->DataTableLoadState = EEChartsDataTableLoadState::Reading;
			if (!W->bStreamingSuspended && !W->DataTableTickerHandle.IsValid())
			{
				W->DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateWeakLambda(W, [W, Request](float) { return W->ReadDataTableBatch(Request); }),
					0.0001f);
			}
		});
	});
}

bool UEChartsWidget::AppendPreparedStreamRow(FEChartsDataTableRow&& Row)
{
	int64 EstimatedBytes = StreamSourceJsonBytes;
	bool bFits = false;
	if (PreparedStreamRows.Type == EEChartsSeriesDataType::Data3D)
	{
		bFits = EstimatedBytes <= FEChartsPayloadBuilder::MaxJsonBytes - 7;
		EstimatedBytes += bFits ? 7 : 0;
		for (const double Value : {Row.Point.X, Row.Point.Y, Row.Point.Z, Row.Point.ColorValue, Row.Point.SymbolSizeValue})
			bFits = bFits && FEChartsPayloadBuilder::AccumulateFiniteDoubleBytes(Value, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes);
	}
	else if (PreparedStreamRows.Type == EEChartsSeriesDataType::Category)
	{
		bFits = FEChartsPayloadBuilder::AccumulateJsonStringBytes(Row.Category, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes) &&
			EstimatedBytes <= FEChartsPayloadBuilder::MaxJsonBytes - 6;
		EstimatedBytes += bFits ? 6 : 0;
		bFits = bFits && FEChartsPayloadBuilder::AccumulateFiniteDoubleBytes(Row.Point.Y, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes);
	}
	else
	{
		bFits = EstimatedBytes <= FEChartsPayloadBuilder::MaxJsonBytes - 4;
		EstimatedBytes += bFits ? 4 : 0;
		bFits = bFits && FEChartsPayloadBuilder::AccumulateFiniteDoubleBytes(Row.Point.X, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes) &&
			FEChartsPayloadBuilder::AccumulateFiniteDoubleBytes(Row.Point.Y, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedBytes);
	}
	if (!bFits)
	{
		FailStreaming(FString::Printf(TEXT("DataTable stream source exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes));
		return false;
	}
	StreamSourceJsonBytes = EstimatedBytes;
	if (PreparedStreamRows.Type == EEChartsSeriesDataType::Data3D) PreparedStreamRows.Data3D.Add(Row.Point);
	else if (PreparedStreamRows.Type == EEChartsSeriesDataType::Category)
		PreparedStreamRows.Category.Add({MoveTemp(Row.Category), Row.Point.Y});
	else PreparedStreamRows.Numeric2D.Add({Row.Point.X, Row.Point.Y});
	return true;
}

void UEChartsWidget::StartPreparedStream(const uint64 Request)
{
	if (Request != DataTableRequest || StreamState != EEChartsDataTableStreamState::Preparing || PreparedStreamRows.Num() == 0) return;
	if (SeriesData[0].Type != EEChartsSeriesDataType::Unset && SeriesData[0].Type != PreparedStreamRows.Type)
	{
		FailStreaming(TEXT("Series 0 data type is incompatible with the DataTable stream. Clear it before starting."));
		return;
	}
	bPreserveStreamCategoryOrder = PreparedStreamRows.Type == EEChartsSeriesDataType::Category;
	XAxisMode = bPreserveStreamCategoryOrder ? EEChartsXAxisMode::Category : EEChartsXAxisMode::ShowAll;
	StreamState = EEChartsDataTableStreamState::Playing;
	TrimStreamWindow();
	const uint64 ActiveStreamRequest = StreamRequest;
	OnDataTableStreamingStarted.Broadcast();
	if (Request == DataTableRequest && ActiveStreamRequest == StreamRequest) ScheduleStreamTicker();
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
	if (LastStreamTickFrameForTesting == GFrameCounter) ++SameFrameStreamTickCallsForTesting;
	LastStreamTickFrameForTesting = GFrameCounter;
#endif
	if (Request != StreamRequest || StreamState != EEChartsDataTableStreamState::Playing || bStreamingSuspended) return false;
	if (LastStreamFrame == GFrameCounter || LastDataTableReadFrame == GFrameCounter) return true;
	LastStreamFrame = GFrameCounter;
	if (CurrentTemplate != DataTableRequestTemplate) { FailStreaming(TEXT("Template changed during streaming.")); return false; }
	bool bCategory = false; FString Error;
	if (!EChartsDataTableLoader::Validate(MappedDataTable, DataTableMapping, CurrentTemplate, bCategory, Error)) { FailStreaming(Error); return false; }
	if (CurrentRow >= PreparedStreamRows.Num())
	{
		if (!bStreamProductionComplete) return true;
		if (bStreamLoop)
		{
			CurrentRow = 0;
			++LoopCount;
			OnDataTableStreamLooped.Broadcast(LoopCount);
			return Request == StreamRequest && StreamState == EEChartsDataTableStreamState::Playing && !bStreamingSuspended;
		}
		StreamFinalRevision = DataRevision;
		StreamTickerHandle.Reset();
		CompleteStreamIfAcknowledged(LastAppliedRevision);
		return false;
	}
	FEChartsSeriesData& Target = SeriesData[0];
	Target.Type = PreparedStreamRows.Type;
	FEChartsSeriesData AddedRows;
	AddedRows.Type = Target.Type;
	const int32 End = FMath::Min(CurrentRow + StreamRowsPerStep, PreparedStreamRows.Num());
	const int32 Added = End - CurrentRow;
	const int32 DropCount = FMath::Max(0, Target.Num() + Added - TimeSeriesWindow);
	const int32 Capacity = FMath::Min(TimeSeriesWindow, Target.Num() + Added);
	if (!CanReplacePointCount(0, Capacity)) { FailStreaming(TEXT("Stream exceeds the chart point limit across all series.")); return false; }
	for (; CurrentRow < End; ++CurrentRow)
	{
		switch (Target.Type)
		{
		case EEChartsSeriesDataType::Numeric2D:
			Target.Numeric2D.Add(PreparedStreamRows.Numeric2D[CurrentRow]);
			AddedRows.Numeric2D.Add(PreparedStreamRows.Numeric2D[CurrentRow]);
			break;
		case EEChartsSeriesDataType::Category:
			Target.Category.Add(PreparedStreamRows.Category[CurrentRow]);
			AddedRows.Category.Add(PreparedStreamRows.Category[CurrentRow]);
			break;
		case EEChartsSeriesDataType::Data3D:
			Target.Data3D.Add(PreparedStreamRows.Data3D[CurrentRow]);
			AddedRows.Data3D.Add(PreparedStreamRows.Data3D[CurrentRow]);
			break;
		default: break;
		}
	}
	StreamedRows += Added;
	bRecordingStreamStep = true;
	MarkDataChanged();
	bRecordingStreamStep = false;
	QueueStreamDelta(MoveTemp(AddedRows), DropCount);
	const bool bAtEnd = bStreamProductionComplete && CurrentRow == PreparedStreamRows.Num();
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
	const bool bBackpressuredByBrowser = InFlightRevision != 0 && PendingStreamDeltas.Num() >= 2;
	if (bBackpressuredByBrowser) StreamTickerHandle.Reset();
	return Request == StreamRequest && StreamState == EEChartsDataTableStreamState::Playing && !StreamFinalRevision &&
		!bStreamingSuspended && !bBackpressuredByBrowser;
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
	bStreamProductionComplete = false;
	PreparedStreamRows = {};
	SeriesData[0].Compact();
	InvalidateStreamDelta();
	if (bPreparing || (DataTableSnapshot && DataTableSnapshot->bStreaming)) StopDataTableLoad(false);
	if (bActive && bNotify) OnDataTableStreamingStopped.Broadcast();
}

void UEChartsWidget::QueueStreamDelta(FEChartsSeriesData&& Added, const int32 DropCount)
{
	// Before the first full payload is submitted, the eventual full snapshot already contains these rows.
	if (!bStreamDeltaReady && StreamFullPayloadRevision == 0) return;
	FEChartsPendingStreamDelta& Delta = PendingStreamDeltas.AddDefaulted_GetRef();
	Delta.Added = MoveTemp(Added);
	Delta.DropCount = DropCount;
	Delta.Revision = DataRevision;
}

void UEChartsWidget::InvalidateStreamDelta()
{
	PendingStreamDeltas.Reset();
	bStreamDeltaReady = false;
	StreamFullPayloadRevision = 0;
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
	if (!IsStreamingActive() || !StreamFinalRevision || Revision != StreamFinalRevision) return;
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
	if (DataTableSnapshot && DataTableSnapshot->bStreaming && DataTableLoadState == EEChartsDataTableLoadState::Reading &&
		!DataTableTickerHandle.IsValid())
	{
		const uint64 Request = DataTableRequest;
		DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return ReadDataTableBatch(Request); }), 0.0001f);
	}
	ScheduleStreamTicker();
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
int32 UEChartsWidget::GetStreamSortKeysProcessedForTesting() const
{
	return DataTableSnapshot ? DataTableSnapshot->SortKeysProcessed : 0;
}
#endif
