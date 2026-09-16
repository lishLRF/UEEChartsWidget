#include "EChartsPayloadBuilder.h"

#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString TemplateName(const EEChartsTemplate Template)
	{
		switch (Template)
		{
		case EEChartsTemplate::SegmentedAreaLine: return TEXT("SegmentedAreaLine");
		case EEChartsTemplate::Bar3DHeightMap: return TEXT("Bar3DHeightMap");
		case EEChartsTemplate::DataTableScatter3D: return TEXT("DataTableScatter3D");
		case EEChartsTemplate::CustomOption: return TEXT("CustomOption");
		default: return TEXT("SegmentedAreaLine");
		}
	}

	FString XAxisModeName(const EEChartsXAxisMode Mode)
	{
		switch (Mode)
		{
		case EEChartsXAxisMode::FollowLatestWindow: return TEXT("FollowLatestWindow");
		case EEChartsXAxisMode::ShowAll: return TEXT("ShowAll");
		case EEChartsXAxisMode::Category: return TEXT("Category");
		default: return TEXT("ShowAll");
		}
	}

	FString SeriesTypeName(const EEChartsSeriesDataType Type)
	{
		switch (Type)
		{
		case EEChartsSeriesDataType::Unset: return TEXT("numeric2D");
		case EEChartsSeriesDataType::Numeric2D: return TEXT("numeric2D");
		case EEChartsSeriesDataType::Category: return TEXT("category");
		case EEChartsSeriesDataType::Data3D: return TEXT("data3D");
		default: return TEXT("numeric2D");
		}
	}

	int64 Utf8Bytes(const FString& Value)
	{
		return FTCHARToUTF8(*Value).Length();
	}

	bool PreflightPayload(
		const TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount>& Series,
		int32& OutPointCount,
		FString& OutError)
	{
		int64 EstimatedJsonBytes = 1024;
		int64 TotalPoints = 0;
		for (const FEChartsSeriesData& Item : Series)
		{
			TotalPoints += Item.Num();
			EstimatedJsonBytes += Utf8Bytes(Item.Name) * 6 + 128;
			switch (Item.Type)
			{
			case EEChartsSeriesDataType::Unset:
				break;
			case EEChartsSeriesDataType::Numeric2D:
				EstimatedJsonBytes += static_cast<int64>(Item.Numeric2D.Num()) * 72;
				break;
			case EEChartsSeriesDataType::Category:
				for (const FEChartsCategoryDataPoint& Point : Item.Category)
				{
					EstimatedJsonBytes += Utf8Bytes(Point.X) * 6 + 48;
				}
				break;
			case EEChartsSeriesDataType::Data3D:
				EstimatedJsonBytes += static_cast<int64>(Item.Data3D.Num()) * 184;
				break;
			}
		}

		if (TotalPoints > FEChartsPayloadBuilder::MaxPointCount)
		{
			OutError = FString::Printf(TEXT("ECharts data batch exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount);
			return false;
		}
		if (EstimatedJsonBytes > FEChartsPayloadBuilder::MaxJsonBytes)
		{
			OutError = FString::Printf(TEXT("ECharts data batch exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes);
			return false;
		}

		OutPointCount = static_cast<int32>(TotalPoints);
		return true;
	}

	TSharedPtr<FJsonValue> NumberArray(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(static_cast<int32>(Values.size()));
		for (const double Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(JsonValues));
	}
}

bool FEChartsPayloadBuilder::BuildBase64Payload(
	const EEChartsTemplate Template,
	const EEChartsXAxisMode XAxisMode,
	const TStaticArray<FEChartsSeriesData, MaxSeriesCount>& Series,
	const int64 Revision,
	FString& OutBase64,
	int32& OutPointCount,
	FString& OutError)
{
	OutBase64.Reset();
	OutPointCount = 0;
	OutError.Reset();
	if (!PreflightPayload(Series, OutPointCount, OutError))
	{
		return false;
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("revision"), static_cast<double>(Revision));
	Root->SetStringField(TEXT("template"), TemplateName(Template));
	Root->SetStringField(TEXT("xAxisMode"), XAxisModeName(XAxisMode));

	TArray<TSharedPtr<FJsonValue>> JsonSeries;
	JsonSeries.Reserve(MaxSeriesCount);
	for (int32 SeriesIndex = 0; SeriesIndex < MaxSeriesCount; ++SeriesIndex)
	{
		const FEChartsSeriesData& Item = Series[SeriesIndex];
		TSharedRef<FJsonObject> JsonItem = MakeShared<FJsonObject>();
		JsonItem->SetNumberField(TEXT("index"), SeriesIndex);
		JsonItem->SetStringField(TEXT("name"), Item.Name);
		JsonItem->SetStringField(TEXT("type"), SeriesTypeName(Item.Type));

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Item.Num());
		switch (Item.Type)
		{
		case EEChartsSeriesDataType::Unset:
			break;
		case EEChartsSeriesDataType::Numeric2D:
			for (const FEChartsDataPoint2D& Point : Item.Numeric2D)
			{
				Data.Add(NumberArray({Point.X, Point.Y}));
			}
			break;
		case EEChartsSeriesDataType::Category:
			for (const FEChartsCategoryDataPoint& Point : Item.Category)
			{
				TArray<TSharedPtr<FJsonValue>> Pair;
				Pair.Reserve(2);
				Pair.Add(MakeShared<FJsonValueString>(Point.X));
				Pair.Add(MakeShared<FJsonValueNumber>(Point.Y));
				Data.Add(MakeShared<FJsonValueArray>(MoveTemp(Pair)));
			}
			break;
		case EEChartsSeriesDataType::Data3D:
			for (const FEChartsDataPoint3D& Point : Item.Data3D)
			{
				Data.Add(NumberArray({Point.X, Point.Y, Point.Z, Point.ColorValue, Point.SymbolSizeValue}));
			}
			break;
		}
		JsonItem->SetArrayField(TEXT("data"), MoveTemp(Data));
		JsonSeries.Add(MakeShared<FJsonValueObject>(JsonItem));
	}
	Root->SetArrayField(TEXT("series"), MoveTemp(JsonSeries));

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Failed to serialize ECharts data payload.");
		return false;
	}

	const FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() > MaxJsonBytes)
	{
		OutError = FString::Printf(TEXT("ECharts data payload is %d bytes and exceeds the %d byte JSON safety limit."), Utf8.Length(), MaxJsonBytes);
		return false;
	}
	OutBase64 = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return true;
}

bool FEChartsPayloadBuilder::DecodeBase64Payload(const FString& Base64, FString& OutJson, FString& OutError)
{
	OutJson.Reset();
	OutError.Reset();
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64, Bytes))
	{
		OutError = TEXT("Invalid Base64 ECharts payload.");
		return false;
	}
	if (Bytes.Num() > MaxJsonBytes)
	{
		OutError = FString::Printf(TEXT("Decoded ECharts payload exceeds the %d byte JSON safety limit."), MaxJsonBytes);
		return false;
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	OutJson = FString(Converted.Length(), Converted.Get());
	return true;
}
