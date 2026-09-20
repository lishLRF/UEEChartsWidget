#include "Async/Async.h"
#include "EChartsDataTableLoader.h"
#include "EChartsWidget.h"

bool UEChartsWidget::GetEChartsDataTableColumns(
    UDataTable* Table, TArray<FEChartsDataTableColumn>& Columns, FString& Error) const
{
	if (!IsInGameThread())
		return false;
	return EChartsDataTableLoader::Columns(Table, Columns, Error);
}
bool UEChartsWidget::SetDataTableMapping(UDataTable* Table, const FEChartsDataTableMapping& Mapping)
{
	if (!IsInGameThread())
		return false;
	StopDataTableStreaming();
	StopDataTableLoad(false);
	bool bCategory = false;
	FString Error;
	MappedDataTable = nullptr;
	if (!EChartsDataTableLoader::Validate(Table, Mapping, CurrentTemplate, bCategory, Error))
	{
		FailDataTableLoad(Error);
		return false;
	}
	MappedDataTable = Table;
	DataTableMapping = Mapping;
	DataTableLoadState = EEChartsDataTableLoadState::Idle;
	LastDataTableError.Reset();
	return true;
}
void UEChartsWidget::LoadDataTable(int32 RowsPerFrame)
{
	if (!IsInGameThread())
		return;
	StopDataTableStreaming();
	StopDataTableLoad(false);
	RowsProcessed = RowsSucceeded = RowsSkipped = TotalRows = 0;
	LastDataTableError.Reset();
	bool bCategory = false;
	FString Error;
	if (!EChartsDataTableLoader::Validate(MappedDataTable, DataTableMapping, CurrentTemplate, bCategory, Error))
	{
		FailDataTableLoad(Error);
		return;
	}
	DataTableSnapshot = MakeShared<FEChartsDataTableSnapshot>();
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	++DataTableSnapshotCreationCountForTesting;
#endif
	DataTableRequestTemplate = CurrentTemplate;
	DataTablePreviousSeries = SeriesData[0];
	DataTablePreviousAxis = XAxisMode;
	bHasDataTableCacheSnapshot = true;
	auto& S = *DataTableSnapshot;
	S.RowNames = MappedDataTable->GetRowNames();
	S.Mapping = DataTableMapping;
	S.Template = CurrentTemplate;
	S.bCategory = bCategory;
	S.b3D =
	    CurrentTemplate == EEChartsTemplate::Bar3DHeightMap || CurrentTemplate == EEChartsTemplate::DataTableScatter3D;
	S.Budget = FMath::Clamp(RowsPerFrame, 1, 4096);
	TotalRows = S.RowNames.Num();
	DataTableLoadState = EEChartsDataTableLoadState::Reading;
	const uint64 Request = DataTableRequest;
	// A positive delay keeps a progress callback's replacement ticker out of the current ticker pass.
	DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
	    FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return ReadDataTableBatch(Request); }), 0.0001f);
}
bool UEChartsWidget::ReadDataTableBatch(uint64 Request)
{
	check(IsInGameThread());
	if (Request != DataTableRequest || DataTableLoadState != EEChartsDataTableLoadState::Reading || !DataTableSnapshot)
		return false;
	if (DataTableSnapshot->bStreaming && DataTableSnapshot->bSortKeysReady &&
		StreamState != EEChartsDataTableStreamState::Preparing &&
		PreparedStreamRows.Num() - CurrentRow >= DataTableSnapshot->Budget)
	{
		return true;
	}
	// FTSTicker pumps new delegates within the same Tick. This guard belongs to the widget, not a request.
	if (LastDataTableReadFrame == GFrameCounter)
		return true;
	LastDataTableReadFrame = GFrameCounter;
	auto S = DataTableSnapshot;
	if (CurrentTemplate != S->Template)
	{
		CancelDataTableLoad();
		return false;
	}
	FString Error;
	bool bCategory = false;
	if (!EChartsDataTableLoader::Validate(MappedDataTable, S->Mapping, S->Template, bCategory, Error) ||
	    bCategory != S->bCategory)
	{
		FailDataTableLoad(Error.IsEmpty() ? TEXT("DataTable schema changed during snapshot.") : Error);
		return false;
	}
	if (S->bStreaming && !S->bSortKeysReady)
	{
		const int32 End = S->SortKeysProcessed + FMath::Min(S->Budget, TotalRows - S->SortKeysProcessed);
		for (; S->SortKeysProcessed < End; ++S->SortKeysProcessed)
		{
			FEChartsDataTableRow SortKey;
			if (!EChartsDataTableLoader::ReadSortKey(MappedDataTable, S->RowNames[S->SortKeysProcessed], *S,
				S->EstimatedSortKeyJsonBytes, SortKey, Error))
			{
				FailDataTableLoad(Error);
				return false;
			}
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
			if (S->bCategory && !SortKey.Category.IsEmpty()) ++StreamCategorySortKeyCopyCountForTesting;
#endif
			S->Rows.Add(MoveTemp(SortKey));
		}
		if (S->SortKeysProcessed < TotalRows) return true;
		DataTableTickerHandle.Reset();
		SortStreamRowNames(Request);
		return false;
	}
	const int32 End = RowsProcessed + FMath::Min(S->Budget, TotalRows - RowsProcessed);
	for (; RowsProcessed < End; ++RowsProcessed)
	{
		FEChartsDataTableRow Row;
		if (EChartsDataTableLoader::ReadRow(MappedDataTable, S->RowNames[RowsProcessed], *S, Row))
		{
			if (S->bStreaming)
			{
				if (!AppendPreparedStreamRow(MoveTemp(Row))) return false;
			}
			else S->Rows.Add(MoveTemp(Row));
			++RowsSucceeded;
		}
		else
			++RowsSkipped;
	}
	OnDataTableLoadProgress.Broadcast(RowsProcessed, TotalRows);
	if (Request != DataTableRequest)
		return false;
	if (CurrentTemplate != S->Template)
	{
		CancelDataTableLoad();
		return false;
	}
	if (!S->bStreaming && RowsSucceeded > FEChartsPayloadBuilder::MaxPointCount)
	{
		FailDataTableLoad(TEXT("DataTable exceeds the 100000 point limit."));
		return false;
	}
	if (S->bStreaming)
	{
		StartPreparedStream(Request);
		if (Request != DataTableRequest || !IsStreamingActive()) return false;
		if (RowsProcessed < TotalRows) return true;
		DataTableTickerHandle.Reset();
		DataTableSnapshot.Reset();
		DataTableLoadState = EEChartsDataTableLoadState::Idle;
		bStreamProductionComplete = true;
		if (PreparedStreamRows.Num() == 0)
		{
			FailStreaming(TEXT("DataTable contains no valid rows to stream."));
		}
		return false;
	}
	if (RowsProcessed < TotalRows)
		return true;
	DataTableTickerHandle.Reset();
	ProcessDataTableSnapshot(Request);
	return false;
}
void UEChartsWidget::ProcessDataTableSnapshot(uint64 Request)
{
	check(IsInGameThread());
	DataTableLoadState = EEChartsDataTableLoadState::Processing;
	auto S = MoveTemp(DataTableSnapshot);
	TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> Series;
	Series[0].Name = SeriesData[0].Name;
	for (int32 Index = 1; Index < FEChartsPayloadBuilder::MaxSeriesCount; ++Index)
		Series[Index] = SeriesData[Index];
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	DataTableWorkerSnapshotPointCountForTesting = 0;
	for (const FEChartsSeriesData& Copied : Series) DataTableWorkerSnapshotPointCountForTesting += Copied.Num();
#endif
	const int64 BaseRevision = DataRevision;
	const auto Template = CurrentTemplate;
	auto Next = [](int64 R) { return R >= 9007199254740991LL ? int64(1) : R + 1; };
	const int64 Revision = Next(Next(BaseRevision));
	const auto Axis = S->bCategory ? EEChartsXAxisMode::Category : EEChartsXAxisMode::ShowAll;
	const bool bCategory = S->bCategory;
	const bool b3D = S->b3D;
	const auto Order = S->Mapping.Order;
	const TWeakObjectPtr<UEChartsWidget> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, Request, Rows = MoveTemp(S->Rows), bCategory, b3D, Order,
	                                       Series = MoveTemp(Series), BaseRevision, Revision, Template, Axis]() mutable
	{
		const FString Name = Series[0].Name;
		Series[0] = EChartsDataTableLoader::Convert(MoveTemp(Rows), bCategory, b3D, Order);
		Series[0].Name = Name;
		FString Payload, Error;
		int32 Count = 0;
		FEChartsPayloadBuilder::BuildBase64Payload(Template, Axis, Series, Revision, Payload, Count, Error);
		AsyncTask(ENamedThreads::GameThread,
		    [WeakThis, Request, BaseRevision, Revision, Template, Axis, Result = MoveTemp(Series[0]),
		        Payload = MoveTemp(Payload), Error = MoveTemp(Error)]() mutable
		{
			UEChartsWidget* W = WeakThis.Get();
			if (!W || W->DataTableRequest != Request || W->DataTableLoadState != EEChartsDataTableLoadState::Processing)
				return;
			if (!Error.IsEmpty())
			{
				W->FailDataTableLoad(Error);
				return;
			}
			if (W->CurrentTemplate != Template)
			{
				W->CancelDataTableLoad();
				return;
			}
			if (W->DataRevision != BaseRevision)
			{
				W->FailDataTableLoad(
				    TEXT("Chart data changed while processing the DataTable; load again."));
				return;
			}
			if (W->RuntimeState == EEChartsRuntimeState::Error)
			{
				W->FailDataTableLoad(W->LastError);
				return;
			}
			W->bInstallingDataTable = true;
			bool bSet = false;
			switch (Result.Type)
			{
			case EEChartsSeriesDataType::Numeric2D:
				bSet = W->SetSeriesData(0, Result.Numeric2D);
				break;
			case EEChartsSeriesDataType::Category:
				bSet = W->SetCategorySeriesData(0, Result.Category);
				break;
			case EEChartsSeriesDataType::Data3D:
				bSet = W->Set3DData(0, Result.Data3D);
				break;
			default:
				break;
			}
			W->SetXAxisMode(Axis);
			W->bInstallingDataTable = false;
			if (!bSet)
			{
				W->FailDataTableLoad(TEXT("Could not install DataTable snapshot."));
				return;
			}
			W->DataTableApplyRevision = Revision;
			W->DataTablePayloadBase64 = MoveTemp(Payload);
			W->DataTableLoadState = EEChartsDataTableLoadState::Applying;
			W->ApplyEChartsChanges();
			if (W->bOptionBarrierActive) W->SendPendingOrCachedOption();
		});
	});
}
void UEChartsWidget::StopDataTableLoad(bool bNotify)
{
	++DataTableRequest;
	if (DataTableTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DataTableTickerHandle);
		DataTableTickerHandle.Reset();
	}
	DataTableSnapshot.Reset();
	const bool bActive = DataTableLoadState == EEChartsDataTableLoadState::Reading ||
	                     DataTableLoadState == EEChartsDataTableLoadState::Processing ||
	                     DataTableLoadState == EEChartsDataTableLoadState::Applying;
	if (bActive && InFlightRevision == DataTableApplyRevision)
		InFlightRevision = 0;
	if (bActive && bHasDataTableCacheSnapshot &&
		(bNotify || (DataTableLoadState == EEChartsDataTableLoadState::Applying && DataTableApplyRevision == DataRevision)))
	{
		SeriesData[0] = MoveTemp(DataTablePreviousSeries);
		XAxisMode = DataTablePreviousAxis;
		bInstallingDataTable = true;
		MarkDataChanged();
		bInstallingDataTable = false;
	}
	if (bActive)
	{
		bApplyRequested = false;
		CancelAutoApply();
	}
	bHasDataTableCacheSnapshot = false;
	DataTableApplyRevision = 0;
	DataTablePayloadBase64.Reset();
	DataTablePreviousSeries = {};
	if (bActive)
	{
		DataTableLoadState = EEChartsDataTableLoadState::Cancelled;
		if (bNotify)
			OnDataTableLoadCancelled.Broadcast();
	}
}
void UEChartsWidget::CancelDataTableLoad()
{
	if (IsInGameThread())
	{
		if (DataTableSnapshot && DataTableSnapshot->bStreaming) StopDataTableStreaming();
		StopDataTableLoad(true);
	}
}
void UEChartsWidget::FailDataTableLoad(const FString& Error)
{
	if (DataTableSnapshot && DataTableSnapshot->bStreaming)
	{
		FailStreaming(Error);
		LastDataTableError = Error;
		DataTableLoadState = EEChartsDataTableLoadState::Error;
		return;
	}
	StopDataTableLoad(false);
	LastDataTableError = Error;
	DataTableLoadState = EEChartsDataTableLoadState::Error;
	ReportDataError(Error);
	if (bOptionBarrierActive) SendPendingOrCachedOption();
}
