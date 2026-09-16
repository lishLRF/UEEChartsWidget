#pragma once

#include "CoreMinimal.h"
#include "EChartsWidget.h"
#include "UObject/Object.h"
#include "EChartsWidgetTestSink.generated.h"

UCLASS()
class UEChartsWidgetTestSink : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void HandleReady()
	{
		++ReadyCount;
	}

	UFUNCTION()
	void HandleRendered(EEChartsTemplate RequestedTemplate, const FString& InEffectiveTemplate)
	{
		++RenderedCount;
		LastRequestedTemplate = RequestedTemplate;
		LastEffectiveTemplate = InEffectiveTemplate;
	}

	UFUNCTION()
	void HandleConsoleMessage(const FString& Message, const FString& Source, int32 Line)
	{
		const FString SeriesMarker = TEXT("__UE_ECHARTS_TEST_SERIES_COUNT__:");
		if (Message.StartsWith(SeriesMarker))
		{
			FString Generation;
			FString Count;
			if (Message.RightChop(SeriesMarker.Len()).Split(TEXT(":"), &Generation, &Count) &&
				LexTryParseString(LastSeriesCount, *Count))
			{
				++SeriesCountReportCount;
			}
			return;
		}

		const FString ResizeMarker = TEXT("__UE_ECHARTS_TEST_RESIZED__:");
		if (Message.StartsWith(ResizeMarker))
		{
			TArray<FString> Parts;
			Message.RightChop(ResizeMarker.Len()).ParseIntoArray(Parts, TEXT(":"), false);
			if (Parts.Num() == 3 &&
				LexTryParseString(LastResizeWidth, *Parts[1]) &&
				LexTryParseString(LastResizeHeight, *Parts[2]))
			{
				++ResizeReportCount;
			}
		}
	}

	UFUNCTION()
	void HandleWarning(const FString& Message)
	{
		++WarningCount;
		LastWarning = Message;
	}

	UFUNCTION()
	void HandleError(const FString& Message)
	{
		++ErrorCount;
		LastError = Message;
	}

	int32 ReadyCount = 0;
	int32 RenderedCount = 0;
	int32 WarningCount = 0;
	int32 ErrorCount = 0;
	int32 SeriesCountReportCount = 0;
	int32 LastSeriesCount = INDEX_NONE;
	int32 ResizeReportCount = 0;
	int32 LastResizeWidth = 0;
	int32 LastResizeHeight = 0;
	FString LastWarning;
	FString LastError;
	EEChartsTemplate LastRequestedTemplate = EEChartsTemplate::SegmentedAreaLine;
	FString LastEffectiveTemplate;
};
