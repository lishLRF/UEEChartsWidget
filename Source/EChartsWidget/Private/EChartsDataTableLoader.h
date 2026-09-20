#pragma once
#include "EChartsDataTableTypes.h"
#include "EChartsDataTypes.h"
#include "Engine/DataTable.h"

// All UObject/reflection access is confined to these GameThread helpers. Worker input contains values only.
struct FEChartsDataTableRow
{
	FName RowKey;
	FString RowName;
	int32 RowNameNumber = 0;
	FString Category;
	FEChartsDataPoint3D Point;
	bool bValidSortKey = false;
};
struct FEChartsDataTableSnapshot
{
	TArray<FName> RowNames;
	TArray<FEChartsDataTableRow> Rows;
	FEChartsDataTableMapping Mapping;
	EEChartsTemplate Template;
	bool bCategory = false;
	bool b3D = false;
	bool bStreaming = false;
	bool bSortKeysReady = false;
	int32 SortKeysProcessed = 0;
	int64 EstimatedSortKeyJsonBytes = 1024;
	int32 Budget = 256;
};
namespace EChartsDataTableLoader
{
bool Columns(UDataTable* Table, TArray<FEChartsDataTableColumn>& Out, FString& Error);
bool Validate(UDataTable* Table, const FEChartsDataTableMapping& Mapping, EEChartsTemplate Template, bool& bCategory,
              FString& Error);
bool ReadSortKey(UDataTable* Table, FName RowName, const FEChartsDataTableSnapshot& Snapshot,
	int64& InOutEstimatedJsonBytes, FEChartsDataTableRow& Out, FString& Error);
bool ReadRow(UDataTable* Table, FName RowName, const FEChartsDataTableSnapshot& Snapshot, FEChartsDataTableRow& Out);
TArray<FName> SortRowNames(TArray<FEChartsDataTableRow> Rows, bool bCategory, EEChartsDataTableOrder Order);
FEChartsSeriesData Convert(TArray<FEChartsDataTableRow> Rows, bool bCategory, bool b3D, EEChartsDataTableOrder Order);
} // namespace EChartsDataTableLoader
