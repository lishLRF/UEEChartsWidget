#pragma once

#include "CoreMinimal.h"
#include "EChartsDataTypes.generated.h"

UENUM(BlueprintType)
enum class EEChartsTemplate : uint8
{
	SegmentedAreaLine,
	Bar3DHeightMap,
	DataTableScatter3D,
	CustomOption
};

USTRUCT(BlueprintType)
struct ECHARTSWIDGET_API FEChartsDataPoint2D
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double Y = 0.0;
};

USTRUCT(BlueprintType)
struct ECHARTSWIDGET_API FEChartsCategoryDataPoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	FString X;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double Y = 0.0;
};

USTRUCT(BlueprintType)
struct ECHARTSWIDGET_API FEChartsDataPoint3D
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double X = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double Y = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double Z = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double ColorValue = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ECharts")
	double SymbolSizeValue = 0.0;
};

UENUM(BlueprintType)
enum class EEChartsXAxisMode : uint8
{
	FollowLatestWindow,
	ShowAll,
	Category
};

enum class EEChartsSeriesDataType : uint8
{
	Unset,
	Numeric2D,
	Category,
	Data3D
};

struct ECHARTSWIDGET_API FEChartsSeriesData
{
	FString Name;
	EEChartsSeriesDataType Type = EEChartsSeriesDataType::Unset;
	TArray<FEChartsDataPoint2D> Numeric2D;
	TArray<FEChartsCategoryDataPoint> Category;
	TArray<FEChartsDataPoint3D> Data3D;
	int32 LogicalStart = 0;

	int32 PhysicalNum() const
	{
		switch (Type)
		{
		case EEChartsSeriesDataType::Unset: return 0;
		case EEChartsSeriesDataType::Numeric2D: return Numeric2D.Num();
		case EEChartsSeriesDataType::Category: return Category.Num();
		case EEChartsSeriesDataType::Data3D: return Data3D.Num();
		default: return 0;
		}
	}

	int32 Num() const
	{
		return FMath::Max(0, PhysicalNum() - LogicalStart);
	}

	void Compact()
	{
		if (LogicalStart <= 0) return;
		switch (Type)
		{
		case EEChartsSeriesDataType::Numeric2D: Numeric2D.RemoveAt(0, LogicalStart, EAllowShrinking::No); break;
		case EEChartsSeriesDataType::Category: Category.RemoveAt(0, LogicalStart, EAllowShrinking::No); break;
		case EEChartsSeriesDataType::Data3D: Data3D.RemoveAt(0, LogicalStart, EAllowShrinking::No); break;
		default: break;
		}
		LogicalStart = 0;
	}

	void TrimFront(const int32 Count)
	{
		LogicalStart += FMath::Clamp(Count, 0, Num());
		const int32 Physical = PhysicalNum();
		if (LogicalStart > 0 && LogicalStart * 2 >= Physical) Compact();
	}

	void ResetData()
	{
		Numeric2D.Reset();
		Category.Reset();
		Data3D.Reset();
		LogicalStart = 0;
	}
};
