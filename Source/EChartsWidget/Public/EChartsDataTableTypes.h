#pragma once
#include "CoreMinimal.h"
#include "EChartsDataTableTypes.generated.h"

UENUM(BlueprintType)
enum class EEChartsDataTableColumnType : uint8
{
	Numeric,
	Category,
	Unsupported
};
UENUM(BlueprintType)
enum class EEChartsDataTableOrder : uint8
{
	XAscending,
	RowName
};
UENUM(BlueprintType)
enum class EEChartsDataTableStreamState : uint8
{
	Stopped,
	Preparing,
	Playing,
	Paused,
	Completed,
	Error
};
UENUM(BlueprintType)
enum class EEChartsDataTableLoadState : uint8
{
	Idle,
	Reading,
	Processing,
	Applying,
	Completed,
	Error,
	Cancelled
};

USTRUCT(BlueprintType)
struct ECHARTSWIDGET_API FEChartsDataTableColumn
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "ECharts|DataTable") FName Name;
	UPROPERTY(BlueprintReadOnly, Category = "ECharts|DataTable")
	EEChartsDataTableColumnType ColumnType = EEChartsDataTableColumnType::Unsupported;
	UPROPERTY(BlueprintReadOnly, Category = "ECharts|DataTable") bool bCanUseAsX = false;
	UPROPERTY(BlueprintReadOnly, Category = "ECharts|DataTable") bool bCanUseAsNumeric = false;
};

USTRUCT(BlueprintType)
struct ECHARTSWIDGET_API FEChartsDataTableMapping
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable") FName X;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable") FName Y;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable") FName Z;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable") FName Color;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable") FName SymbolSize;
	/** Stable, case-sensitive lexical category ordering; numeric X is ascending. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts|DataTable")
	EEChartsDataTableOrder Order = EEChartsDataTableOrder::XAscending;
};
