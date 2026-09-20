#include "EChartsWidget.h"
#include "EChartsDataTableLoader.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "EChartsWidget"

namespace
{
	const FString ReadyMarker = TEXT("__UE_ECHARTS_READY__:");
	const FString RenderedMarker = TEXT("__UE_ECHARTS_RENDERED__:");
	const FString AppliedMarker = TEXT("__UE_ECHARTS_APPLIED__:");
	const FString WarningMarker = TEXT("__UE_ECHARTS_WARNING__:");
	const FString ErrorMarker = TEXT("__UE_ECHARTS_ERROR__:");

	bool TryParseGeneration(const FString& Text, uint64& OutGeneration)
	{
		return !Text.IsEmpty() && LexTryParseString(OutGeneration, *Text);
	}

	bool TryParseGenerationAndPayload(
		const FString& Text,
		uint64& OutGeneration,
		FString& OutPayload)
	{
		FString GenerationText;
		return Text.Split(TEXT(":"), &GenerationText, &OutPayload) &&
			TryParseGeneration(GenerationText, OutGeneration);
	}

	FString PercentEncodeFilePath(const FString& Path)
	{
		const FTCHARToUTF8 Utf8(*Path);
		const ANSICHAR* Bytes = Utf8.Get();
		const TCHAR HexDigits[] = TEXT("0123456789ABCDEF");
		FString Encoded;
		Encoded.Reserve(Utf8.Length() * 3);

		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Byte = static_cast<uint8>(Bytes[Index]);
			const bool bUnreserved =
				(Byte >= 'A' && Byte <= 'Z') ||
				(Byte >= 'a' && Byte <= 'z') ||
				(Byte >= '0' && Byte <= '9') ||
				Byte == '-' || Byte == '_' || Byte == '.' || Byte == '~';

			if (bUnreserved || Byte == '/' || Byte == ':')
			{
				Encoded.AppendChar(static_cast<TCHAR>(Byte));
			}
			else
			{
				Encoded.AppendChar(TEXT('%'));
				Encoded.AppendChar(HexDigits[Byte >> 4]);
				Encoded.AppendChar(HexDigits[Byte & 0x0F]);
			}
		}

		return Encoded;
	}
}

UEChartsWidget::UEChartsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSupportsTransparency = true;
	for (int32 SeriesIndex = 0; SeriesIndex < FEChartsPayloadBuilder::MaxSeriesCount; ++SeriesIndex)
	{
		SeriesData[SeriesIndex].Name = FString::Printf(TEXT("Series %d"), SeriesIndex + 1);
	}
	BindConsoleMessage();
}

bool UEChartsWidget::IsValidSeriesIndex(const int32 SeriesIndex) const
{
	return SeriesIndex >= 0 && SeriesIndex < FEChartsPayloadBuilder::MaxSeriesCount;
}

bool UEChartsWidget::IsGameThreadMutation() const
{
	return IsInGameThread();
}

int32 UEChartsWidget::GetTotalPointCount() const
{
	int32 Total = 0;
	for (const FEChartsSeriesData& Series : SeriesData)
	{
		Total += Series.Num();
	}
	return Total;
}

bool UEChartsWidget::CanReplacePointCount(const int32 SeriesIndex, const int32 NewSeriesPointCount) const
{
	const int32 BoundedCount = SeriesIndex == 0 && IsStreamingActive() && bTimeSeriesEnabled
		? FMath::Min(NewSeriesPointCount, TimeSeriesWindow) : NewSeriesPointCount;
	return IsValidSeriesIndex(SeriesIndex) && NewSeriesPointCount >= 0 &&
		GetTotalPointCount() - SeriesData[SeriesIndex].Num() + BoundedCount <= FEChartsPayloadBuilder::MaxPointCount;
}

void UEChartsWidget::ReportDataError(const FString& Message)
{
	LastError = Message;
	OnEChartsError.Broadcast(Message);
}

void UEChartsWidget::MarkDataChanged()
{
	if (IsStreamingActive() && !bRecordingStreamStep) InvalidateStreamDelta();
	TrimStreamWindow();
	if (!bInstallingDataTable && DataTableLoadState == EEChartsDataTableLoadState::Applying)
	{
		DataTableApplyRevision = 0;
		bHasDataTableCacheSnapshot = false;
		StopDataTableLoad(false);
	}
	static constexpr int64 MaxJavascriptSafeInteger = 9007199254740991LL;
	if (DataRevision >= MaxJavascriptSafeInteger)
	{
		DataRevision = 1;
		LastAppliedRevision = 0;
		if (IsStreamingActive()) InvalidateStreamDelta();
	}
	else
	{
		++DataRevision;
	}
	if (StreamFinalRevision != 0 && IsStreamingActive()) StreamFinalRevision = DataRevision;
	bIsDirty = true;
	bHasPresentationState = true;
	ScheduleAutoApply();
}

bool UEChartsWidget::AddDataPoint(const int32 SeriesIndex, const double X, const double Y)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex) || !FMath::IsFinite(X) || !FMath::IsFinite(Y))
	{
		return false;
	}
	FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	if (Series.Type != EEChartsSeriesDataType::Unset && Series.Type != EEChartsSeriesDataType::Numeric2D)
	{
		return false;
	}
	if (!CanReplacePointCount(SeriesIndex, Series.Num() + 1))
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	Series.Type = EEChartsSeriesDataType::Numeric2D;
	Series.AddNumericRing({X, Y});
	MarkDataChanged();
	return true;
}

bool UEChartsWidget::AddCategoryDataPoint(const int32 SeriesIndex, const FString& X, const double Y)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex) || X.TrimStartAndEnd().IsEmpty() || !FMath::IsFinite(Y))
	{
		return false;
	}
	FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	if (Series.Type != EEChartsSeriesDataType::Unset && Series.Type != EEChartsSeriesDataType::Category)
	{
		return false;
	}
	if (!CanReplacePointCount(SeriesIndex, Series.Num() + 1))
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	Series.Type = EEChartsSeriesDataType::Category;
	Series.AddCategoryRing({X, Y});
	MarkDataChanged();
	return true;
}

bool UEChartsWidget::SetSeriesData(const int32 SeriesIndex, const TArray<FEChartsDataPoint2D>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex)) return false;
	for (const FEChartsDataPoint2D& Point : Data)
	{
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y)) return false;
	}
	if (GetTotalPointCount() - SeriesData[SeriesIndex].Num() + Data.Num() > FEChartsPayloadBuilder::MaxPointCount)
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	const bool bStopped = !bInstallingDataTable && IsStreamingActive();
	if (bStopped) StopStreaming(false);
	FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	Series.ResetData();
	Series.Type = EEChartsSeriesDataType::Numeric2D;
	Series.Numeric2D = Data;
	if (SeriesIndex == 0) bPreserveStreamCategoryOrder = false;
	MarkDataChanged();
	if (bStopped) OnDataTableStreamingStopped.Broadcast();
	return true;
}

bool UEChartsWidget::AppendSeriesData(const int32 SeriesIndex, const TArray<FEChartsDataPoint2D>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex) ||
		(SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Unset && SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Numeric2D)) return false;
	for (const FEChartsDataPoint2D& Point : Data)
	{
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y)) return false;
	}
	if (!CanReplacePointCount(SeriesIndex, SeriesData[SeriesIndex].Num() + Data.Num()))
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	if (!Data.IsEmpty())
	{
		SeriesData[SeriesIndex].Type = EEChartsSeriesDataType::Numeric2D;
		for (const FEChartsDataPoint2D& Point : Data) SeriesData[SeriesIndex].AddNumericRing(Point);
		MarkDataChanged();
	}
	return true;
}

TArray<FEChartsDataPoint2D> UEChartsWidget::GetSeriesData(const int32 SeriesIndex) const
{
	if (!IsValidSeriesIndex(SeriesIndex) || SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Numeric2D) return {};
	const FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	TArray<FEChartsDataPoint2D> Result; Result.Reserve(Series.Num());
	for (int32 I = 0; I < Series.Num(); ++I) Result.Add(Series.NumericAt(I));
	return Result;
}

bool UEChartsWidget::SetCategorySeriesData(const int32 SeriesIndex, const TArray<FEChartsCategoryDataPoint>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex)) return false;
	for (const FEChartsCategoryDataPoint& Point : Data)
	{
		if (Point.X.TrimStartAndEnd().IsEmpty() || !FMath::IsFinite(Point.Y)) return false;
	}
	if (GetTotalPointCount() - SeriesData[SeriesIndex].Num() + Data.Num() > FEChartsPayloadBuilder::MaxPointCount)
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	const bool bStopped = !bInstallingDataTable && IsStreamingActive();
	if (bStopped) StopStreaming(false);
	FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	Series.ResetData();
	Series.Type = EEChartsSeriesDataType::Category;
	Series.Category = Data;
	if (SeriesIndex == 0) bPreserveStreamCategoryOrder = false;
	MarkDataChanged();
	if (bStopped) OnDataTableStreamingStopped.Broadcast();
	return true;
}

bool UEChartsWidget::AppendCategorySeriesData(const int32 SeriesIndex, const TArray<FEChartsCategoryDataPoint>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex) ||
		(SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Unset && SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Category)) return false;
	for (const FEChartsCategoryDataPoint& Point : Data)
	{
		if (Point.X.TrimStartAndEnd().IsEmpty() || !FMath::IsFinite(Point.Y)) return false;
	}
	if (!CanReplacePointCount(SeriesIndex, SeriesData[SeriesIndex].Num() + Data.Num()))
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	if (!Data.IsEmpty())
	{
		SeriesData[SeriesIndex].Type = EEChartsSeriesDataType::Category;
		for (const FEChartsCategoryDataPoint& Point : Data) SeriesData[SeriesIndex].AddCategoryRing(Point);
		MarkDataChanged();
	}
	return true;
}

TArray<FEChartsCategoryDataPoint> UEChartsWidget::GetCategorySeriesData(const int32 SeriesIndex) const
{
	if (!IsValidSeriesIndex(SeriesIndex) || SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Category) return {};
	const FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	TArray<FEChartsCategoryDataPoint> Result; Result.Reserve(Series.Num());
	for (int32 I = 0; I < Series.Num(); ++I) Result.Add(Series.CategoryAt(I));
	return Result;
}

bool UEChartsWidget::Set3DData(const int32 SeriesIndex, const TArray<FEChartsDataPoint3D>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex)) return false;
	for (const FEChartsDataPoint3D& Point : Data)
	{
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) || !FMath::IsFinite(Point.Z) ||
			!FMath::IsFinite(Point.ColorValue) || !FMath::IsFinite(Point.SymbolSizeValue)) return false;
	}
	if (GetTotalPointCount() - SeriesData[SeriesIndex].Num() + Data.Num() > FEChartsPayloadBuilder::MaxPointCount)
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	const bool bStopped = !bInstallingDataTable && IsStreamingActive();
	if (bStopped) StopStreaming(false);
	FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	Series.ResetData();
	Series.Type = EEChartsSeriesDataType::Data3D;
	Series.Data3D = Data;
	if (SeriesIndex == 0) bPreserveStreamCategoryOrder = false;
	MarkDataChanged();
	if (bStopped) OnDataTableStreamingStopped.Broadcast();
	return true;
}

bool UEChartsWidget::Append3DData(const int32 SeriesIndex, const TArray<FEChartsDataPoint3D>& Data)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex) ||
		(SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Unset && SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Data3D)) return false;
	for (const FEChartsDataPoint3D& Point : Data)
	{
		if (!FMath::IsFinite(Point.X) || !FMath::IsFinite(Point.Y) || !FMath::IsFinite(Point.Z) ||
			!FMath::IsFinite(Point.ColorValue) || !FMath::IsFinite(Point.SymbolSizeValue)) return false;
	}
	if (!CanReplacePointCount(SeriesIndex, SeriesData[SeriesIndex].Num() + Data.Num()))
	{
		ReportDataError(FString::Printf(TEXT("ECharts data cache exceeds the %d point limit."), FEChartsPayloadBuilder::MaxPointCount));
		return false;
	}
	if (!Data.IsEmpty())
	{
		SeriesData[SeriesIndex].Type = EEChartsSeriesDataType::Data3D;
		for (const FEChartsDataPoint3D& Point : Data) SeriesData[SeriesIndex].AddData3DRing(Point);
		MarkDataChanged();
	}
	return true;
}

TArray<FEChartsDataPoint3D> UEChartsWidget::Get3DData(const int32 SeriesIndex) const
{
	if (!IsValidSeriesIndex(SeriesIndex) || SeriesData[SeriesIndex].Type != EEChartsSeriesDataType::Data3D) return {};
	const FEChartsSeriesData& Series = SeriesData[SeriesIndex];
	TArray<FEChartsDataPoint3D> Result; Result.Reserve(Series.Num());
	for (int32 I = 0; I < Series.Num(); ++I) Result.Add(Series.Data3DAt(I));
	return Result;
}

bool UEChartsWidget::ClearSeries(const int32 SeriesIndex)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex)) return false;
	SeriesData[SeriesIndex].ResetData();
	SeriesData[SeriesIndex].Type = EEChartsSeriesDataType::Unset;
	if (SeriesIndex == 0) bPreserveStreamCategoryOrder = false;
	MarkDataChanged();
	StopDataTableStreaming();
	return true;
}

void UEChartsWidget::ClearAll()
{
	if (!IsGameThreadMutation()) return;
	bPreserveStreamCategoryOrder = false;
	for (FEChartsSeriesData& Series : SeriesData)
	{
		Series.ResetData();
		Series.Type = EEChartsSeriesDataType::Unset;
	}
	MarkDataChanged();
	StopDataTableStreaming();
}

bool UEChartsWidget::SetSeriesName(const int32 SeriesIndex, const FString& Name)
{
	if (!IsGameThreadMutation() || !IsValidSeriesIndex(SeriesIndex)) return false;
	SeriesData[SeriesIndex].Name = Name;
	MarkDataChanged();
	return true;
}

void UEChartsWidget::SetXAxisMode(const EEChartsXAxisMode Mode)
{
	if (!IsGameThreadMutation()) return;
	XAxisMode = Mode;
	MarkDataChanged();
}

void UEChartsWidget::ApplyEChartsChanges()
{
	if (!IsGameThreadMutation()) return;
	CancelAutoApply();
	if (!bIsDirty)
	{
		bApplyRequested = false;
		return;
	}
	bApplyRequested = true;
	if (RuntimeState == EEChartsRuntimeState::Ready && InFlightRevision == 0)
	{
		SubmitLatestData();
	}
}

void UEChartsWidget::SetAutoApplyEnabled(const bool bEnabled, const float InMaxUpdatesPerSecond)
{
	if (!IsGameThreadMutation()) return;
	CancelAutoApply();
	bAutoApplyEnabled = bEnabled;
	MaxUpdatesPerSecond = FMath::Clamp(FMath::IsFinite(InMaxUpdatesPerSecond) ? InMaxUpdatesPerSecond : 10.0f, 1.0f, 30.0f);
	if (bAutoApplyEnabled)
	{
		ScheduleAutoApply();
	}
	else
	{
		CancelAutoApply();
	}
}

void UEChartsWidget::SubmitLatestData()
{
	if (!bIsDirty)
	{
		bApplyRequested = false;
		return;
	}
	if (RuntimeState != EEChartsRuntimeState::Ready || InFlightRevision != 0) return;
	FString PayloadBase64;
	FString Error;
	int32 PointCount = 0;
	bool bSubmitDelta = false;
	int64 SubmissionRevision = DataRevision;
	if (IsStreamingActive() && bStreamDeltaReady && !PendingStreamDeltas.IsEmpty())
	{
		const FEChartsPendingStreamDelta& Delta = PendingStreamDeltas[0];
		SubmissionRevision = Delta.Revision;
		if (!FEChartsPayloadBuilder::BuildStreamDeltaBase64(
			Delta.Added, Delta.DropCount, LastAppliedRevision, SubmissionRevision, PayloadBase64, Error))
		{
			bApplyRequested = false;
			FailStreaming(Error);
			return;
		}
		PointCount = GetTotalPointCount();
		PendingStreamDeltas.RemoveAt(0, 1, EAllowShrinking::No);
		bSubmitDelta = true;
	}
	else if (DataTableApplyRevision == DataRevision && !DataTablePayloadBase64.IsEmpty())
	{
		PayloadBase64 = DataTablePayloadBase64;
		PointCount = GetTotalPointCount();
	}
	else if (!FEChartsPayloadBuilder::BuildBase64Payload(
		CurrentTemplate, XAxisMode, SeriesData, DataRevision, PayloadBase64, PointCount, Error, bPreserveStreamCategoryOrder))
	{
		bApplyRequested = false;
		if (IsStreamingActive()) { FailStreaming(Error); return; }
		ReportDataError(Error);
		return;
	}

	InFlightRevision = SubmissionRevision;
	bApplyRequested = bSubmitDelta && !PendingStreamDeltas.IsEmpty();
	LastSubmitSeconds = FPlatformTime::Seconds();
	if (!bSubmitDelta && IsStreamingActive())
	{
		bStreamDeltaReady = false;
		StreamFullPayloadRevision = SubmissionRevision;
		PendingStreamDeltas.Reset();
	}
	const FString Command = bSubmitDelta
		? FEChartsWidgetJavascript::BuildApplyStreamDeltaCommand(PayloadBase64)
		: FEChartsWidgetJavascript::BuildApplyDataCommand(PayloadBase64);
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	LastSubmitCommandLengthForTesting = Command.Len();
	bLastSubmitWasDeltaForTesting = bSubmitDelta;
#endif
	ExecuteJavascript(Command);
}

void UEChartsWidget::ScheduleAutoApply()
{
	if (!bAutoApplyEnabled || !bIsDirty || RuntimeState != EEChartsRuntimeState::Ready ||
		InFlightRevision != 0 || AutoApplyTickerHandle.IsValid())
	{
		return;
	}
	const double Interval = 1.0 / static_cast<double>(MaxUpdatesPerSecond);
	const float Delay = static_cast<float>(FMath::Max(0.0, Interval - (FPlatformTime::Seconds() - LastSubmitSeconds)));
	AutoApplyTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateWeakLambda(this, [this](float)
		{
			AutoApplyTickerHandle.Reset();
			if (bAutoApplyEnabled && bIsDirty && RuntimeState == EEChartsRuntimeState::Ready && InFlightRevision == 0)
			{
				SubmitLatestData();
			}
			return false;
		}),
		Delay);
}

void UEChartsWidget::CancelAutoApply()
{
	if (AutoApplyTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(AutoApplyTickerHandle);
		AutoApplyTickerHandle.Reset();
	}
}

void UEChartsWidget::InitializeECharts(
	const EEChartsTemplate Template,
	const EEChartsInteractionMode InInteractionMode)
{
	if (Template != CurrentTemplate) StopDataTableStreaming();
	if (Template != DataTableRequestTemplate &&
		(DataTableLoadState == EEChartsDataTableLoadState::Reading ||
		 DataTableLoadState == EEChartsDataTableLoadState::Processing ||
		 DataTableLoadState == EEChartsDataTableLoadState::Applying))
	{
		CancelDataTableLoad();
	}
	BindConsoleMessage();
	CurrentTemplate = Template;
	InteractionMode = InInteractionMode;
	bHasInitialized = true;
	bReloadOnRebuild = false;
	BeginLoadGeneration();
	LoadURL(InitialURL);
}

void UEChartsWidget::BeginLoadGeneration()
{
	CancelAutoApply();
	if (IsStreamingActive()) InvalidateStreamDelta();
	if (InFlightRevision != 0)
	{
		bApplyRequested = true;
		InFlightRevision = 0;
	}
	if (bHasPresentationState)
	{
		bIsDirty = true;
		bApplyRequested = true;
	}
	RuntimeState = EEChartsRuntimeState::Loading;
	LastError.Reset();
	LastWarning.Reset();
	EffectiveTemplate.Reset();
	bReadyBroadcast = false;
	bRenderedBroadcast = false;
	if (++LoadGeneration == 0)
	{
		++LoadGeneration;
	}

	InitialURL = FString::Printf(
		TEXT("%s?generation=%llu"),
		*FEChartsWidgetResourceLocator::GetChartHostUrl(),
		LoadGeneration);
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	if (bForceWebGLUnavailableForTesting)
	{
		InitialURL += TEXT("&forceWebGL=0");
	}
	if (bReportSeriesCountForTesting)
	{
		InitialURL += TEXT("&testSeriesProbe=1");
	}
#endif
}

void UEChartsWidget::ReleaseSlateResources(const bool bReleaseChildren)
{
	bStreamingSuspended = true;
	CancelStreamTicker();
	if (DataTableSnapshot && DataTableSnapshot->bStreaming)
	{
		if (DataTableTickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(DataTableTickerHandle);
		DataTableTickerHandle.Reset();
	}
	else StopDataTableLoad(false);
	CancelAutoApply();
	if (InFlightRevision != 0)
	{
		bApplyRequested = true;
		InFlightRevision = 0;
	}
	bReloadOnRebuild = bHasInitialized;
	OnConsoleMessage.RemoveDynamic(this, &UEChartsWidget::HandleEChartsConsoleMessage);
	bReadyBroadcast = false;
	bRenderedBroadcast = false;
	RuntimeState = EEChartsRuntimeState::Uninitialized;
	LastError.Reset();
	LastWarning.Reset();
	EffectiveTemplate.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

void UEChartsWidget::BeginDestroy()
{
	StopStreaming(false);
	StopDataTableLoad(false);
	CancelAutoApply();
	Super::BeginDestroy();
}

TSharedRef<SWidget> UEChartsWidget::RebuildWidget()
{
	BindConsoleMessage();
	PrepareAutomaticRebuild();
	return Super::RebuildWidget();
}

void UEChartsWidget::PrepareAutomaticRebuild()
{
	if (bHasInitialized && bReloadOnRebuild)
	{
		bReloadOnRebuild = false;
		BeginLoadGeneration();
	}
	ResumeStreamingAfterRebuild();
}

void UEChartsWidget::HandleEChartsConsoleMessage(
	const FString& Message,
	const FString& Source,
	const int32 Line)
{
	if (Message.StartsWith(ReadyMarker))
	{
		uint64 MessageGeneration = 0;
		if (TryParseGeneration(Message.RightChop(ReadyMarker.Len()), MessageGeneration) &&
			MessageGeneration == LoadGeneration &&
			RuntimeState != EEChartsRuntimeState::Error &&
			!bReadyBroadcast)
		{
			bReadyBroadcast = true;
			RuntimeState = EEChartsRuntimeState::Ready;
			FString PayloadJson = TEXT("{}");
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
			PayloadJson = InitializationPayloadForTesting;
#endif
			ExecuteJavascript(FEChartsWidgetJavascript::BuildRenderCommand(
				CurrentTemplate,
				InteractionMode,
				PayloadJson));
			OnChartReady.Broadcast();
			if (bApplyRequested)
			{
				SubmitLatestData();
			}
			else
			{
				ScheduleAutoApply();
			}
		}
		return;
	}

	if (Message.StartsWith(AppliedMarker))
	{
		TArray<FString> Parts;
		Message.RightChop(AppliedMarker.Len()).ParseIntoArray(Parts, TEXT(":"), false);
		uint64 MessageGeneration = 0;
		int64 MessageRevision = 0;
		int32 MessagePointCount = 0;
		if (Parts.Num() == 3 && TryParseGeneration(Parts[0], MessageGeneration) &&
			LexTryParseString(MessageRevision, *Parts[1]) && LexTryParseString(MessagePointCount, *Parts[2]) &&
			MessageGeneration == LoadGeneration && MessageRevision > 0 && MessagePointCount >= 0 &&
			MessageRevision == InFlightRevision)
		{
			InFlightRevision = 0;
			LastAppliedRevision = MessageRevision;
			if (MessageRevision == StreamFullPayloadRevision)
			{
				StreamFullPayloadRevision = 0;
				bStreamDeltaReady = IsStreamingActive();
			}
			LastAppliedPointCount = MessagePointCount;
			if (MessageRevision == DataRevision)
			{
				bIsDirty = false;
			}
			if (DataTableLoadState == EEChartsDataTableLoadState::Applying && MessageRevision == DataTableApplyRevision)
			{
				const int32 Succeeded = RowsSucceeded;
				const int32 Skipped = RowsSkipped;
				++DataTableRequest;
				DataTableLoadState = EEChartsDataTableLoadState::Completed;
				bApplyRequested = false;
				DataTableApplyRevision = 0;
				DataTablePayloadBase64.Reset();
				DataTablePreviousSeries = {};
				bHasDataTableCacheSnapshot = false;
				OnDataTableLoaded.Broadcast(Succeeded, Skipped);
			}
			CompleteStreamIfAcknowledged(MessageRevision);
			OnEChartsApplied.Broadcast(MessageRevision, MessagePointCount);
			if (!PendingStreamDeltas.IsEmpty()) bApplyRequested = true;
			if (bApplyRequested && bIsDirty)
			{
				SubmitLatestData();
			}
			else
			{
				ScheduleAutoApply();
			}
			ScheduleStreamTicker();
		}
		return;
	}

	if (Message.StartsWith(RenderedMarker))
	{
		TArray<FString> Parts;
		Message.RightChop(RenderedMarker.Len()).ParseIntoArray(Parts, TEXT(":"), false);
		uint64 MessageGeneration = 0;
		if (Parts.Num() == 3 &&
			TryParseGeneration(Parts[0], MessageGeneration) &&
			MessageGeneration == LoadGeneration &&
			Parts[1] == FEChartsWidgetJavascript::TemplateName(CurrentTemplate) &&
			RuntimeState == EEChartsRuntimeState::Ready &&
			!bRenderedBroadcast)
		{
			bRenderedBroadcast = true;
			EffectiveTemplate = Parts[2];
			OnChartRendered.Broadcast(CurrentTemplate, EffectiveTemplate);
		}
		return;
	}

	if (Message.StartsWith(WarningMarker))
	{
		uint64 MessageGeneration = 0;
		FString Warning;
		if (TryParseGenerationAndPayload(
			Message.RightChop(WarningMarker.Len()), MessageGeneration, Warning) &&
			MessageGeneration == LoadGeneration &&
			RuntimeState != EEChartsRuntimeState::Error)
		{
			LastWarning = Warning;
			OnEChartsWarning.Broadcast(Warning);
		}
		return;
	}

	if (Message.StartsWith(ErrorMarker))
	{
		uint64 MessageGeneration = 0;
		FString Error;
		if (TryParseGenerationAndPayload(
			Message.RightChop(ErrorMarker.Len()), MessageGeneration, Error) &&
			MessageGeneration == LoadGeneration &&
			RuntimeState != EEChartsRuntimeState::Error)
		{
			LastError = Error;
			RuntimeState = EEChartsRuntimeState::Error;
			if (IsStreamingActive())
			{
				StopStreaming(false);
				StreamState = EEChartsDataTableStreamState::Error;
			}
			if (DataTableLoadState == EEChartsDataTableLoadState::Reading || DataTableLoadState == EEChartsDataTableLoadState::Processing || DataTableLoadState == EEChartsDataTableLoadState::Applying)
			{
				StopDataTableLoad(false);
				DataTableLoadState = EEChartsDataTableLoadState::Error;
				LastDataTableError = Error;
			}
			OnEChartsError.Broadcast(LastError);
		}
	}
}

FString FEChartsWidgetJavascript::TemplateName(const EEChartsTemplate Template)
{
	switch (Template)
	{
	case EEChartsTemplate::SegmentedAreaLine:
		return TEXT("SegmentedAreaLine");
	case EEChartsTemplate::Bar3DHeightMap:
		return TEXT("Bar3DHeightMap");
	case EEChartsTemplate::DataTableScatter3D:
		return TEXT("DataTableScatter3D");
	case EEChartsTemplate::CustomOption:
		return TEXT("CustomOption");
	default:
		return TEXT("SegmentedAreaLine");
	}
}

FString FEChartsWidgetJavascript::InteractionModeName(const EEChartsInteractionMode InteractionMode)
{
	switch (InteractionMode)
	{
	case EEChartsInteractionMode::Disabled:
		return TEXT("Disabled");
	case EEChartsInteractionMode::ClickOnly:
		return TEXT("ClickOnly");
	case EEChartsInteractionMode::FullHover:
		return TEXT("FullHover");
	default:
		return TEXT("ClickOnly");
	}
}

FString FEChartsWidgetJavascript::BuildRenderCommand(
	const EEChartsTemplate Template,
	const EEChartsInteractionMode InteractionMode,
	const FString& PayloadJson)
{
	return FString::Printf(
		TEXT("window.UEEChartsHost.renderTemplate(\"%s\",%s,\"%s\");"),
		*TemplateName(Template),
		PayloadJson.IsEmpty() ? TEXT("{}") : *PayloadJson,
		*InteractionModeName(InteractionMode));
}

FString FEChartsWidgetJavascript::BuildApplyDataCommand(const FString& PayloadBase64)
{
	if (PayloadBase64.IsEmpty())
	{
		return FString();
	}
	for (const TCHAR Character : PayloadBase64)
	{
		const bool bBase64Character =
			(Character >= TEXT('A') && Character <= TEXT('Z')) ||
			(Character >= TEXT('a') && Character <= TEXT('z')) ||
			(Character >= TEXT('0') && Character <= TEXT('9')) ||
			Character == TEXT('+') || Character == TEXT('/') || Character == TEXT('=');
		if (!bBase64Character)
		{
			return FString();
		}
	}
	return FString::Printf(
		TEXT("window.UEEChartsHost.applyDataBase64(\"%s\");"),
		*PayloadBase64);
}

FString FEChartsWidgetJavascript::BuildApplyStreamDeltaCommand(const FString& PayloadBase64)
{
	const FString FullCommand = BuildApplyDataCommand(PayloadBase64);
	return FullCommand.IsEmpty()
		? FString()
		: FString::Printf(TEXT("window.UEEChartsHost.applyStreamDeltaBase64(\"%s\");"), *PayloadBase64);
}

void UEChartsWidget::BindConsoleMessage()
{
	OnConsoleMessage.AddUniqueDynamic(this, &UEChartsWidget::HandleEChartsConsoleMessage);
}

#if WITH_EDITOR
const FText UEChartsWidget::GetPaletteCategory()
{
	return LOCTEXT("PaletteCategory", "ECharts");
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
void UEChartsWidget::RebindConsoleMessageForTesting()
{
	BindConsoleMessage();
}

void UEChartsWidget::PrepareRebuildForTesting()
{
	BindConsoleMessage();
	PrepareAutomaticRebuild();
}

void UEChartsWidget::SetForceWebGLUnavailableForTesting(const bool bForceUnavailable)
{
	bForceWebGLUnavailableForTesting = bForceUnavailable;
}

void UEChartsWidget::SetInitializationPayloadForTesting(
	const FString& PayloadJson,
	const bool bReportSeriesCount)
{
	InitializationPayloadForTesting = PayloadJson.IsEmpty() ? TEXT("{}") : PayloadJson;
	bReportSeriesCountForTesting = bReportSeriesCount;
}

void UEChartsWidget::AcknowledgeCurrentApplyForTesting()
{
	if (InFlightRevision > 0) AcknowledgeRevisionForTesting(InFlightRevision);
}

void UEChartsWidget::AcknowledgeRevisionForTesting(const int64 Revision)
{
	OnConsoleMessage.Broadcast(
		FString::Printf(TEXT("__UE_ECHARTS_APPLIED__:%llu:%lld:%d"), LoadGeneration, Revision, GetTotalPointCount()),
		FString(), 0);
}

bool UEChartsWidget::IsAutoApplyScheduledForTesting() const
{
	return AutoApplyTickerHandle.IsValid();
}
#endif

FString FEChartsWidgetResourceLocator::GetChartHostPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("EChartsWidget"));
	if (!Plugin.IsValid())
	{
		return FString();
	}

	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Web/chart-host.html")));
}

FString FEChartsWidgetResourceLocator::GetChartHostUrl()
{
	return BuildHostPageUrlForPath(GetChartHostPath());
}

FString FEChartsWidgetResourceLocator::BuildHostPageUrlForPath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString AbsolutePath = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(AbsolutePath);

	return FString(TEXT("file:///")) + PercentEncodeFilePath(AbsolutePath);
}

#undef LOCTEXT_NAMESPACE
