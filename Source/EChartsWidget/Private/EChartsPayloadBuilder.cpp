#include "EChartsPayloadBuilder.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <atomic>

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	std::atomic<int32> GSerializationAttemptCount{0};
	std::atomic<int32> GDecodeAttemptCount{0};
#endif

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

	bool TryAddBytes(const int64 Bytes, const int64 Limit, int64& InOutBytes)
	{
		if (Bytes < 0 || InOutBytes < 0 || Limit < 0 || InOutBytes > Limit || Bytes > Limit - InOutBytes)
		{
			return false;
		}
		InOutBytes += Bytes;
		return true;
	}

	bool TryAccumulateJsonStringBytes(const FStringView Value, const int64 Limit, int64& InOutBytes)
	{
		if (InOutBytes < 0 || InOutBytes > Limit || Value.Len() > Limit - InOutBytes) return false;
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			uint32 Codepoint = static_cast<uint32>(Value[Index]);
#if !PLATFORM_TCHAR_IS_4_BYTES
			if (StringConv::IsHighSurrogate(Codepoint))
			{
				if (Index + 1 < Value.Len() && StringConv::IsLowSurrogate(static_cast<uint32>(Value[Index + 1])))
				{
					Codepoint = StringConv::EncodeSurrogate(
						static_cast<uint16>(Codepoint),
						static_cast<uint16>(Value[++Index]));
				}
				else
				{
					Codepoint = UNICODE_BOGUS_CHAR_CODEPOINT;
				}
			}
			else if (StringConv::IsLowSurrogate(Codepoint))
			{
				Codepoint = UNICODE_BOGUS_CHAR_CODEPOINT;
			}
#else
			if (!StringConv::IsValidCodepoint(Codepoint) || StringConv::IsHighSurrogate(Codepoint) || StringConv::IsLowSurrogate(Codepoint))
			{
				Codepoint = UNICODE_BOGUS_CHAR_CODEPOINT;
			}
#endif

			int64 EncodedBytes = 0;
			switch (Codepoint)
			{
			case TEXT('"'):
			case TEXT('\\'):
			case TEXT('\b'):
			case TEXT('\f'):
			case TEXT('\n'):
			case TEXT('\r'):
			case TEXT('\t'):
				EncodedBytes = 2;
				break;
			default:
				if (Codepoint < 0x20) EncodedBytes = 6;
				else if (Codepoint < 0x80) EncodedBytes = 1;
				else if (Codepoint < 0x800) EncodedBytes = 2;
				else if (Codepoint < 0x10000) EncodedBytes = 3;
				else EncodedBytes = 4;
				break;
			}
			if (!TryAddBytes(EncodedBytes, Limit, InOutBytes))
			{
				return false;
			}
		}
		return true;
	}

	bool TryAccumulateFiniteDoubleBytes(const double Value, const int64 Limit, int64& InOutBytes)
	{
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
		TCHAR Buffer[64];
		const int32 Length = FCString::Snprintf(Buffer, UE_ARRAY_COUNT(Buffer), TEXT("%.17g"), Value);
		return Length > 0 && Length < UE_ARRAY_COUNT(Buffer) && TryAddBytes(Length, Limit, InOutBytes);
	}

	bool PreflightPayload(
		const TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount>& Series,
		int32& OutPointCount,
		FString& OutError)
	{
		int64 TotalPoints = 0;
		for (const FEChartsSeriesData& Item : Series)
		{
			if (!TryAddBytes(Item.Num(), FEChartsPayloadBuilder::MaxPointCount, TotalPoints))
			{
				OutError = FString::Printf(TEXT("ECharts data batch exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount);
				return false;
			}
		}

		int64 EstimatedJsonBytes = 1024;
		for (const FEChartsSeriesData& Item : Series)
		{
			if (Item.Num() <= 0) continue;
			if (!TryAccumulateJsonStringBytes(Item.Name, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
				!TryAddBytes(128, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes))
			{
				OutError = FString::Printf(TEXT("ECharts data batch exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes);
				return false;
			}
			switch (Item.Type)
			{
			case EEChartsSeriesDataType::Unset:
				break;
			case EEChartsSeriesDataType::Numeric2D:
				for (int32 Index = 0; Index < Item.Num(); ++Index)
				{
					const FEChartsDataPoint2D& Point = Item.NumericAt(Index);
					if (!TryAddBytes(4, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.X, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.Y, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes))
					{
						OutError = FString::Printf(TEXT("ECharts data contains non-finite values or exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes);
						return false;
					}
				}
				break;
			case EEChartsSeriesDataType::Category:
				for (int32 Index = 0; Index < Item.Num(); ++Index)
				{
					const FEChartsCategoryDataPoint& Point = Item.CategoryAt(Index);
					if (!TryAccumulateJsonStringBytes(Point.X, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAddBytes(6, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.Y, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes))
					{
						OutError = FString::Printf(TEXT("ECharts data contains non-finite values or exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes);
						return false;
					}
				}
				break;
			case EEChartsSeriesDataType::Data3D:
				for (int32 Index = 0; Index < Item.Num(); ++Index)
				{
					const FEChartsDataPoint3D& Point = Item.Data3DAt(Index);
					if (!TryAddBytes(7, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.X, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.Y, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.Z, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.ColorValue, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes) ||
						!TryAccumulateFiniteDoubleBytes(Point.SymbolSizeValue, FEChartsPayloadBuilder::MaxJsonBytes, EstimatedJsonBytes))
					{
						OutError = FString::Printf(TEXT("ECharts data contains non-finite values or exceeds the %d byte JSON safety limit."), FEChartsPayloadBuilder::MaxJsonBytes);
						return false;
					}
				}
				break;
			}
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
	FString& OutError,
	const bool bPreserveCategoryOrder)
{
	OutBase64.Reset();
	OutPointCount = 0;
	OutError.Reset();
	if (!PreflightPayload(Series, OutPointCount, OutError))
	{
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS
	++GSerializationAttemptCount;
#endif

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("revision"), static_cast<double>(Revision));
	Root->SetStringField(TEXT("template"), TemplateName(Template));
	Root->SetStringField(TEXT("xAxisMode"), XAxisModeName(XAxisMode));
	if (bPreserveCategoryOrder) Root->SetBoolField(TEXT("preserveCategoryOrder"), true);

	TArray<TSharedPtr<FJsonValue>> JsonSeries;
	JsonSeries.Reserve(MaxSeriesCount);
	int32 CompactSeriesIndex = 0;
	for (int32 SeriesIndex = 0; SeriesIndex < MaxSeriesCount; ++SeriesIndex)
	{
		const FEChartsSeriesData& Item = Series[SeriesIndex];
		if (Item.Num() <= 0) continue;
		TSharedRef<FJsonObject> JsonItem = MakeShared<FJsonObject>();
		JsonItem->SetNumberField(TEXT("index"), CompactSeriesIndex++);
		JsonItem->SetStringField(TEXT("name"), Item.Name);
		JsonItem->SetStringField(TEXT("type"), SeriesTypeName(Item.Type));

		TArray<TSharedPtr<FJsonValue>> Data;
		Data.Reserve(Item.Num());
		switch (Item.Type)
		{
		case EEChartsSeriesDataType::Unset:
			break;
		case EEChartsSeriesDataType::Numeric2D:
			for (int32 Index = 0; Index < Item.Num(); ++Index)
			{
				const FEChartsDataPoint2D& Point = Item.NumericAt(Index);
				Data.Add(NumberArray({Point.X, Point.Y}));
			}
			break;
		case EEChartsSeriesDataType::Category:
			for (int32 Index = 0; Index < Item.Num(); ++Index)
			{
				const FEChartsCategoryDataPoint& Point = Item.CategoryAt(Index);
				TArray<TSharedPtr<FJsonValue>> Pair;
				Pair.Reserve(2);
				Pair.Add(MakeShared<FJsonValueString>(Point.X));
				Pair.Add(MakeShared<FJsonValueNumber>(Point.Y));
				Data.Add(MakeShared<FJsonValueArray>(MoveTemp(Pair)));
			}
			break;
		case EEChartsSeriesDataType::Data3D:
			for (int32 Index = 0; Index < Item.Num(); ++Index)
			{
				const FEChartsDataPoint3D& Point = Item.Data3DAt(Index);
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
	const int64 EncodedLength = Base64.Len();
	if (EncodedLength == 0 || EncodedLength % 4 != 0)
	{
		OutError = TEXT("Invalid Base64 ECharts payload.");
		return false;
	}
	int32 Padding = 0;
	if (Base64.EndsWith(TEXT("=="))) Padding = 2;
	else if (Base64.EndsWith(TEXT("="))) Padding = 1;
	const int64 MaximumDecodedBytes = (EncodedLength / 4) * 3 - Padding;
	if (MaximumDecodedBytes > MaxJsonBytes)
	{
		OutError = FString::Printf(TEXT("Decoded ECharts payload exceeds the %d byte JSON safety limit."), MaxJsonBytes);
		return false;
	}
	TArray<uint8> Bytes;
#if WITH_DEV_AUTOMATION_TESTS
	++GDecodeAttemptCount;
#endif
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

bool FEChartsPayloadBuilder::BuildStreamDeltaBase64(
	const FEChartsSeriesData& Delta,
	const int32 DropCount,
	const int64 BaseRevision,
	const int64 Revision,
	FString& OutBase64,
	FString& OutError)
{
	OutBase64.Reset();
	OutError.Reset();
	if (DropCount < 0 || BaseRevision <= 0 || Revision <= 0 || Delta.Num() > 256)
	{
		OutError = TEXT("Invalid ECharts stream delta metadata.");
		return false;
	}
	TStaticArray<FEChartsSeriesData, MaxSeriesCount> PreflightSeries;
	PreflightSeries[0] = Delta;
	int32 PointCount = 0;
	if (!PreflightPayload(PreflightSeries, PointCount, OutError)) return false;

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("revision"), static_cast<double>(Revision));
	Root->SetNumberField(TEXT("baseRevision"), static_cast<double>(BaseRevision));
	Root->SetNumberField(TEXT("drop"), DropCount);
	TSharedRef<FJsonObject> JsonSeries = MakeShared<FJsonObject>();
	JsonSeries->SetNumberField(TEXT("index"), 0);
	JsonSeries->SetStringField(TEXT("type"), SeriesTypeName(Delta.Type));
	TArray<TSharedPtr<FJsonValue>> Data;
	Data.Reserve(Delta.Num());
	switch (Delta.Type)
	{
	case EEChartsSeriesDataType::Numeric2D:
		for (int32 Index = 0; Index < Delta.Num(); ++Index)
		{
			const auto& Point = Delta.NumericAt(Index);
			Data.Add(NumberArray({Point.X, Point.Y}));
		}
		break;
	case EEChartsSeriesDataType::Category:
		for (int32 Index = 0; Index < Delta.Num(); ++Index)
		{
			const auto& Point = Delta.CategoryAt(Index);
			TArray<TSharedPtr<FJsonValue>> Pair;
			Pair.Add(MakeShared<FJsonValueString>(Point.X));
			Pair.Add(MakeShared<FJsonValueNumber>(Point.Y));
			Data.Add(MakeShared<FJsonValueArray>(MoveTemp(Pair)));
		}
		break;
	case EEChartsSeriesDataType::Data3D:
		for (int32 Index = 0; Index < Delta.Num(); ++Index)
		{
			const auto& Point = Delta.Data3DAt(Index);
			Data.Add(NumberArray({Point.X, Point.Y, Point.Z, Point.ColorValue, Point.SymbolSizeValue}));
		}
		break;
	default: break;
	}
	JsonSeries->SetArrayField(TEXT("data"), MoveTemp(Data));
	Root->SetObjectField(TEXT("series"), JsonSeries);
	FString Json;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("Failed to serialize ECharts stream delta.");
		return false;
	}
	const FTCHARToUTF8 Utf8(*Json);
	if (Utf8.Length() > MaxJsonBytes)
	{
		OutError = FString::Printf(TEXT("ECharts stream delta exceeds the %d byte JSON safety limit."), MaxJsonBytes);
		return false;
	}
	OutBase64 = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return true;
}

bool FEChartsPayloadBuilder::AccumulateJsonStringBytes(const FString& Value, const int64 Limit, int64& InOutBytes)
{
	return TryAccumulateJsonStringBytes(Value, Limit, InOutBytes);
}

bool FEChartsPayloadBuilder::AccumulateFiniteDoubleBytes(const double Value, const int64 Limit, int64& InOutBytes)
{
	return TryAccumulateFiniteDoubleBytes(Value, Limit, InOutBytes);
}

#if WITH_DEV_AUTOMATION_TESTS
void FEChartsPayloadBuilder::ResetSafetyInstrumentationForTesting()
{
	GSerializationAttemptCount = 0;
	GDecodeAttemptCount = 0;
}

int32 FEChartsPayloadBuilder::GetSerializationAttemptCountForTesting()
{
	return GSerializationAttemptCount;
}

int32 FEChartsPayloadBuilder::GetDecodeAttemptCountForTesting()
{
	return GDecodeAttemptCount;
}

bool FEChartsPayloadBuilder::AccumulateJsonStringBytesForTesting(
	const FString& Value,
	const int64 Limit,
	int64& InOutBytes)
{
	return TryAccumulateJsonStringBytes(Value, Limit, InOutBytes);
}
#endif
