#pragma once
#include "EChartsDataTableTypes.h"
#include "EChartsDataTypes.h"
#include "Engine/DataTable.h"

// All UObject/reflection access is confined to these GameThread helpers. Worker input contains values only.
struct FEChartsDataTableRow
{
	FString RowName;
	int32 RowNameNumber = 0;
	FString Category;
	FEChartsDataPoint3D Point;
};
struct FEChartsDataTableSnapshot
{
	TArray<FName> RowNames;
	TArray<FEChartsDataTableRow> Rows;
	FEChartsDataTableMapping Mapping;
	EEChartsTemplate Template;
	bool bCategory = false;
	bool b3D = false;
	int32 Budget = 256;
};
namespace EChartsDataTableLoader
{
bool Columns(UDataTable* Table, TArray<FEChartsDataTableColumn>& Out, FString& Error);
bool Validate(UDataTable* Table, const FEChartsDataTableMapping& Mapping, EEChartsTemplate Template, bool& bCategory,
              FString& Error);
bool ReadRow(UDataTable* Table, FName RowName, const FEChartsDataTableSnapshot& Snapshot, FEChartsDataTableRow& Out);
FEChartsSeriesData Convert(TArray<FEChartsDataTableRow> Rows, bool bCategory, bool b3D, EEChartsDataTableOrder Order);
} // namespace EChartsDataTableLoader
