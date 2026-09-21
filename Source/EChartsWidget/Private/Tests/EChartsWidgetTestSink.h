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
	UFUNCTION() void HandleStreamStarted() { ++StreamStartedCount; }
	UFUNCTION() void HandleStreamStopped() { ++StreamStoppedCount; }
	UFUNCTION() void HandleStreamCompleted() { ++StreamCompletedCount; }
	UFUNCTION() void HandleStreamLooped(int64 Loop) { ++StreamLoopedCount; }
	UFUNCTION() void HandleStreamProgress(int32 Current, int32 Total, int64 Loop)
	{
		++StreamProgressCount;
		if (StreamProgressFrame == GFrameCounter) ++StreamSameFrameCount;
		StreamProgressFrame = GFrameCounter;
		if (StreamCallbackWidget.IsValid() && StreamProgressCount >= StreamCallbackAfterProgressCount)
		{
			UEChartsWidget* W = StreamCallbackWidget.Get();
			if (StreamCallbackAction == 1) W->PauseDataTableStreaming();
			if (StreamCallbackAction == 2) W->StopDataTableStreaming();
			if (StreamCallbackAction == 3 && StreamRestartsRemaining-- > 0) W->StartDataTableStreaming(0.01f, 1, true, 1);
			if (StreamCallbackAction == 4) { W->PauseDataTableStreaming(); W->ResumeDataTableStreaming(); }
		}
	}
	int32 StreamStartedCount = 0, StreamStoppedCount = 0, StreamCompletedCount = 0, StreamLoopedCount = 0;
	int32 StreamProgressCount = 0, StreamSameFrameCount = 0, StreamCallbackAction = 0, StreamRestartsRemaining = 0;
	int32 StreamCallbackAfterProgressCount = 0;
	uint64 StreamProgressFrame = MAX_uint64;
	TWeakObjectPtr<UEChartsWidget> StreamCallbackWidget;
	UFUNCTION()
	void HandleTableProgress(int32 Processed, int32 Total)
	{
		if (TableProgressFrame != GFrameCounter) { TableProgressFrame = GFrameCounter; TableRowsThisFrame = 0; }
		TableRowsThisFrame += Processed - LastTableProcessed;
		MaxTableRowsPerFrame = FMath::Max(MaxTableRowsPerFrame, TableRowsThisFrame);
		++TableProgressCount;
		MaxTableBatch = FMath::Max(MaxTableBatch, Processed - LastTableProcessed);
		LastTableProcessed = Processed;
		bTableEventsOnGameThread &= IsInGameThread();
		if (TableTemplateChangeWidget.IsValid()) TableTemplateChangeWidget->InitializeECharts(EEChartsTemplate::Bar3DHeightMap);
		if (TableRestartWidget.IsValid() && TableRestartsRemaining > 0)
		{
			--TableRestartsRemaining;
			LastTableProcessed = 0;
			TableRestartWidget->LoadDataTable(7);
		}
	}
	UFUNCTION()
	void HandleTableLoaded(int32 Succeeded, int32 Skipped)
	{
		++TableLoadedCount;
		bTableEventsOnGameThread &= IsInGameThread();
		if (LoadedMutationWidget.IsValid())
		{
			UEChartsWidget* Widget = LoadedMutationWidget.Get();
			LoadedMutationWidget.Reset();
			if (bRestartOnApplied) Widget->LoadDataTable(1);
			else Widget->AddDataPoint(0, 99, 99);
		}
	}
	UFUNCTION()
	void HandleTableCancelled() { ++TableCancelledCount; bTableEventsOnGameThread &= IsInGameThread(); }
	int32 TableProgressCount = 0;
	int32 MaxTableBatch = 0;
	int32 LastTableProcessed = 0;
	int32 TableLoadedCount = 0;
	int32 TableCancelledCount = 0;
	bool bTableEventsOnGameThread = true;
	TWeakObjectPtr<UEChartsWidget> TableTemplateChangeWidget;
	TWeakObjectPtr<UEChartsWidget> TableRestartWidget;
	int32 TableRestartsRemaining = 0;
	uint64 TableProgressFrame = MAX_uint64;
	int32 TableRowsThisFrame = 0;
	int32 MaxTableRowsPerFrame = 0;
	TWeakObjectPtr<UEChartsWidget> AppliedMutationWidget;
	TWeakObjectPtr<UEChartsWidget> LoadedMutationWidget;
	bool bRestartOnApplied = false;
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
		const FString AdvancedMarker = TEXT("__UE_ECHARTS_TEST_ADVANCED__:");
		if (Message.StartsWith(AdvancedMarker))
		{
			++AdvancedProbeCount;
			LastAdvancedProbe = Message.RightChop(AdvancedMarker.Len());
			return;
		}
		const FString DataOptionMarker = TEXT("__UE_ECHARTS_TEST_DATA_OPTION__:1:");
		if (Message.StartsWith(DataOptionMarker))
		{
			++DataOptionReportCount;
			bLastDataOptionSucceeded = Message.RightChop(DataOptionMarker.Len()) == TEXT("OK");
			return;
		}

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
		if (ErrorInitializeWidget.IsValid())
		{
			UEChartsWidget* Widget = ErrorInitializeWidget.Get();
			ErrorInitializeWidget.Reset();
			Widget->InitializeECharts(EEChartsTemplate::SegmentedAreaLine, EEChartsInteractionMode::ClickOnly);
		}
	}

	UFUNCTION()
	void HandleApplied(int64 Revision, int32 PointCount)
	{
		++AppliedCount;
		LastAppliedRevision = Revision;
		LastAppliedPointCount = PointCount;
		if (AppliedMutationWidget.IsValid())
		{
			UEChartsWidget* Widget = AppliedMutationWidget.Get();
			AppliedMutationWidget.Reset();
			if (bRestartOnApplied) Widget->LoadDataTable(1);
			else Widget->AddDataPoint(0, 99, 99);
		}
	}

	UFUNCTION()
	void HandleOptionApplied(bool bSuccess, const FString& Message)
	{
		++OptionResultCount;
		bLastOptionSuccess = bSuccess;
		LastAdvancedMessage = Message;
		if (!bSuccess && OptionFailureInitializeWidget.IsValid())
		{
			UEChartsWidget* Widget = OptionFailureInitializeWidget.Get();
			OptionFailureInitializeWidget.Reset();
			Widget->InitializeECharts(OptionFailureInitializeTemplate, EEChartsInteractionMode::ClickOnly);
		}
	}

	UFUNCTION()
	void HandleInteractionModeApplied(EEChartsInteractionMode Mode, bool bSuccess, const FString& Message)
	{
		++InteractionResultCount;
		LastInteractionMode = Mode;
		bLastInteractionSuccess = bSuccess;
		LastAdvancedMessage = Message;
	}

	UFUNCTION()
	void HandleLegendSettingsApplied(bool bSuccess, const FString& Message)
	{
		++LegendResultCount;
		bLastLegendSuccess = bSuccess;
		LastAdvancedMessage = Message;
	}

	UFUNCTION()
	void HandleJavaScriptResult(int64 RequestId, bool bSuccess, const FString& Message)
	{
		++JavaScriptResultCount;
		LastJavaScriptRequestId = RequestId;
		bLastJavaScriptSuccess = bSuccess;
		LastAdvancedMessage = Message;
	}

	int32 ReadyCount = 0;
	int32 RenderedCount = 0;
	int32 WarningCount = 0;
	int32 ErrorCount = 0;
	int32 AppliedCount = 0;
	int32 OptionResultCount = 0;
	int32 InteractionResultCount = 0;
	int32 LegendResultCount = 0;
	int32 JavaScriptResultCount = 0;
	int64 LastAppliedRevision = 0;
	int32 LastAppliedPointCount = 0;
	int64 LastJavaScriptRequestId = 0;
	bool bLastOptionSuccess = false;
	bool bLastInteractionSuccess = false;
	bool bLastLegendSuccess = false;
	bool bLastJavaScriptSuccess = false;
	EEChartsInteractionMode LastInteractionMode = EEChartsInteractionMode::ClickOnly;
	FString LastAdvancedMessage;
	TWeakObjectPtr<UEChartsWidget> OptionFailureInitializeWidget;
	EEChartsTemplate OptionFailureInitializeTemplate = EEChartsTemplate::Bar3DHeightMap;
	TWeakObjectPtr<UEChartsWidget> ErrorInitializeWidget;
	int32 DataOptionReportCount = 0;
	bool bLastDataOptionSucceeded = false;
	int32 SeriesCountReportCount = 0;
	int32 LastSeriesCount = INDEX_NONE;
	int32 ResizeReportCount = 0;
	int32 AdvancedProbeCount = 0;
	int32 LastResizeWidth = 0;
	int32 LastResizeHeight = 0;
	FString LastWarning;
	FString LastError;
	FString LastAdvancedProbe;
	EEChartsTemplate LastRequestedTemplate = EEChartsTemplate::SegmentedAreaLine;
	FString LastEffectiveTemplate;
};
