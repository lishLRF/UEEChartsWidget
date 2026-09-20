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
	int32 RingCapacity = 0;
	int32 RingCount = 0;
#if WITH_DEV_AUTOMATION_TESTS
	int64 FrontMoveCountForTesting = 0;
#endif

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
		return RingCapacity > 0 ? RingCount : PhysicalNum();
	}

	int32 PhysicalIndex(const int32 LogicalIndex) const
	{
		return RingCapacity > 0 ? (LogicalStart + LogicalIndex) % FMath::Max(1, PhysicalNum()) : LogicalIndex;
	}

	const FEChartsDataPoint2D& NumericAt(const int32 Index) const { return Numeric2D[PhysicalIndex(Index)]; }
	const FEChartsCategoryDataPoint& CategoryAt(const int32 Index) const { return Category[PhysicalIndex(Index)]; }
	const FEChartsDataPoint3D& Data3DAt(const int32 Index) const { return Data3D[PhysicalIndex(Index)]; }

	void ConfigureRing(const int32 Capacity)
	{
		const int32 NewCapacity = FMath::Max(1, Capacity);
		const int32 Keep = FMath::Min(Num(), NewCapacity);
		const int32 First = Num() - Keep;
		switch (Type)
		{
		case EEChartsSeriesDataType::Numeric2D:
		{
			TArray<FEChartsDataPoint2D> Ordered; Ordered.Reserve(NewCapacity);
			for (int32 I = 0; I < Keep; ++I) Ordered.Add(NumericAt(First + I));
			Numeric2D = MoveTemp(Ordered); break;
		}
		case EEChartsSeriesDataType::Category:
		{
			TArray<FEChartsCategoryDataPoint> Ordered; Ordered.Reserve(NewCapacity);
			for (int32 I = 0; I < Keep; ++I) Ordered.Add(CategoryAt(First + I));
			Category = MoveTemp(Ordered); break;
		}
		case EEChartsSeriesDataType::Data3D:
		{
			TArray<FEChartsDataPoint3D> Ordered; Ordered.Reserve(NewCapacity);
			for (int32 I = 0; I < Keep; ++I) Ordered.Add(Data3DAt(First + I));
			Data3D = MoveTemp(Ordered); break;
		}
		default: break;
		}
		LogicalStart = 0;
		RingCapacity = NewCapacity;
		RingCount = Keep;
	}

	void Linearize()
	{
		if (RingCapacity <= 0) return;
		const int32 Count = RingCount;
		ConfigureRing(FMath::Max(1, Count));
		RingCapacity = 0;
		RingCount = 0;
		LogicalStart = 0;
	}

	void AddNumericRing(const FEChartsDataPoint2D& Point)
	{
		if (RingCapacity <= 0) { Numeric2D.Add(Point); return; }
		if (RingCount < RingCapacity)
		{
			if (Numeric2D.Num() < RingCapacity) Numeric2D.Add(Point);
			else Numeric2D[(LogicalStart + RingCount) % RingCapacity] = Point;
			++RingCount;
		}
		else { Numeric2D[LogicalStart] = Point; LogicalStart = (LogicalStart + 1) % RingCapacity; }
	}

	void AddCategoryRing(const FEChartsCategoryDataPoint& Point)
	{
		if (RingCapacity <= 0) { Category.Add(Point); return; }
		if (RingCount < RingCapacity)
		{
			if (Category.Num() < RingCapacity) Category.Add(Point);
			else Category[(LogicalStart + RingCount) % RingCapacity] = Point;
			++RingCount;
		}
		else { Category[LogicalStart] = Point; LogicalStart = (LogicalStart + 1) % RingCapacity; }
	}

	void AddData3DRing(const FEChartsDataPoint3D& Point)
	{
		if (RingCapacity <= 0) { Data3D.Add(Point); return; }
		if (RingCount < RingCapacity)
		{
			if (Data3D.Num() < RingCapacity) Data3D.Add(Point);
			else Data3D[(LogicalStart + RingCount) % RingCapacity] = Point;
			++RingCount;
		}
		else { Data3D[LogicalStart] = Point; LogicalStart = (LogicalStart + 1) % RingCapacity; }
	}

	void TrimFront(const int32 Count)
	{
		const int32 Removed = FMath::Clamp(Count, 0, Num());
		if (RingCapacity > 0)
		{
			RingCount -= Removed;
			LogicalStart = RingCount > 0 ? (LogicalStart + Removed) % FMath::Max(1, PhysicalNum()) : 0;
		}
	}

	void ResetData()
	{
		Numeric2D.Reset();
		Category.Reset();
		Data3D.Reset();
		LogicalStart = 0;
		RingCapacity = 0;
		RingCount = 0;
#if WITH_DEV_AUTOMATION_TESTS
		FrontMoveCountForTesting = 0;
#endif
	}
};
