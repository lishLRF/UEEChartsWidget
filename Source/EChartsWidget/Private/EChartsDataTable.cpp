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
	DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
	    FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return ReadDataTableBatch(Request); }));
}
bool UEChartsWidget::ReadDataTableBatch(uint64 Request)
{
	check(IsInGameThread());
	if (Request != DataTableRequest || DataTableLoadState != EEChartsDataTableLoadState::Reading || !DataTableSnapshot)
		return false;
	auto S = DataTableSnapshot;
	if (CurrentTemplate != S->Template)
	{
		FailDataTableLoad(TEXT("Chart template changed during the DataTable snapshot; load again."));
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
	const int32 End = RowsProcessed + FMath::Min(S->Budget, TotalRows - RowsProcessed);
	for (; RowsProcessed < End; ++RowsProcessed)
	{
		FEChartsDataTableRow Row;
		if (EChartsDataTableLoader::ReadRow(MappedDataTable, S->RowNames[RowsProcessed], *S, Row))
		{
			S->Rows.Add(MoveTemp(Row));
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
		FailDataTableLoad(TEXT("Chart template changed in the DataTable progress callback; load again."));
		return false;
	}
	if (RowsSucceeded > FEChartsPayloadBuilder::MaxPointCount)
	{
		FailDataTableLoad(TEXT("DataTable exceeds the 100000 point limit."));
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
	auto Series = SeriesData;
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
			if (W->DataRevision != BaseRevision || W->CurrentTemplate != Template)
			{
				W->FailDataTableLoad(
				    TEXT("Chart data or template changed while processing the DataTable; load again."));
				return;
			}
			if (W->RuntimeState == EEChartsRuntimeState::Error)
			{
				W->FailDataTableLoad(W->LastError);
				return;
			}
			W->DataTablePreviousSeries = W->SeriesData[0];
			W->DataTablePreviousAxis = W->XAxisMode;
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
	if (DataTableLoadState == EEChartsDataTableLoadState::Applying && DataTableApplyRevision == DataRevision)
	{
		SeriesData[0] = MoveTemp(DataTablePreviousSeries);
		XAxisMode = DataTablePreviousAxis;
		bInstallingDataTable = true;
		MarkDataChanged();
		bInstallingDataTable = false;
		bApplyRequested = false;
		CancelAutoApply();
	}
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
		StopDataTableLoad(true);
}
void UEChartsWidget::FailDataTableLoad(const FString& Error)
{
	StopDataTableLoad(false);
	LastDataTableError = Error;
	DataTableLoadState = EEChartsDataTableLoadState::Error;
	ReportDataError(Error);
}
