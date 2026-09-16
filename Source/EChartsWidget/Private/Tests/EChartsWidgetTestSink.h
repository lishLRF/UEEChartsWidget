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
		const FString Marker = TEXT("__UE_ECHARTS_TEST_SERIES_COUNT__:");
		if (!Message.StartsWith(Marker))
		{
			return;
		}

		FString Generation;
		FString Count;
		if (Message.RightChop(Marker.Len()).Split(TEXT(":"), &Generation, &Count) &&
			LexTryParseString(LastSeriesCount, *Count))
		{
			++SeriesCountReportCount;
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
	FString LastWarning;
	FString LastError;
	EEChartsTemplate LastRequestedTemplate = EEChartsTemplate::SegmentedAreaLine;
	FString LastEffectiveTemplate;
};
