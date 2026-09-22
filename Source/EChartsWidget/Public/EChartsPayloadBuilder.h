#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "EChartsDataTypes.h"

class ECHARTSWIDGET_API FEChartsPayloadBuilder
{
public:
	static constexpr int32 MaxSeriesCount = 4;
	static constexpr int32 MaxPointCount = 100000;
	static constexpr int32 MaxJsonBytes = 16 * 1024 * 1024;

	static bool BuildBase64Payload(
		EEChartsTemplate Template,
		EEChartsXAxisMode XAxisMode,
		const TStaticArray<FEChartsSeriesData, MaxSeriesCount>& Series,
		int64 Revision,
		FString& OutBase64,
		int32& OutPointCount,
		FString& OutError,
		bool bPreserveCategoryOrder = false,
		bool bPointWindow2D = false);

	static bool DecodeBase64Payload(const FString& Base64, FString& OutJson, FString& OutError);
	static bool BuildStreamDeltaBase64(
		const FEChartsSeriesData& Delta,
		int32 DropCount,
		int64 BaseRevision,
		int64 Revision,
		FString& OutBase64,
		FString& OutError);
	static bool AccumulateJsonStringBytes(const FString& Value, int64 Limit, int64& InOutBytes);
	static bool AccumulateFiniteDoubleBytes(double Value, int64 Limit, int64& InOutBytes);

#if WITH_DEV_AUTOMATION_TESTS
	static void ResetSafetyInstrumentationForTesting();
	static int32 GetSerializationAttemptCountForTesting();
	static int32 GetDecodeAttemptCountForTesting();
	static bool AccumulateJsonStringBytesForTesting(const FString& Value, int64 Limit, int64& InOutBytes);
#endif
};
