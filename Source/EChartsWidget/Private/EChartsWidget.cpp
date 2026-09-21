#include "EChartsWidget.h"
#include "EChartsDataTableLoader.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "EChartsWidget"

struct FEChartsPayloadBuildTestGate
{
	FEChartsPayloadBuildTestGate() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
	~FEChartsPayloadBuildTestGate() { FPlatformProcess::ReturnSynchEventToPool(Event); }
	FEvent* Event = nullptr;
};

namespace
{
	const FString ReadyMarker = TEXT("__UE_ECHARTS_READY__:");
	const FString RenderedMarker = TEXT("__UE_ECHARTS_RENDERED__:");
	const FString AppliedMarker = TEXT("__UE_ECHARTS_APPLIED__:");
	const FString WarningMarker = TEXT("__UE_ECHARTS_WARNING__:");
	const FString ErrorMarker = TEXT("__UE_ECHARTS_ERROR__:");
	const FString OptionResultMarker = TEXT("__UE_ECHARTS_OPTION_RESULT__:");
	const FString InteractionResultMarker = TEXT("__UE_ECHARTS_INTERACTION_RESULT__:");
	const FString LegendResultMarker = TEXT("__UE_ECHARTS_LEGEND_RESULT__:");
	const FString JavaScriptResultMarker = TEXT("__UE_ECHARTS_JAVASCRIPT_RESULT__:");
	constexpr int32 MaxOptionJsonBytes = 16 * 1024 * 1024;
	constexpr int32 MaxJavaScriptBytes = 1024 * 1024;

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

	bool TryParseAdvancedResult(
		const FString& Text,
		uint64& OutGeneration,
		uint64& OutRequestId,
		bool& bOutSuccess,
		FString& OutMessage)
	{
		FString GenerationText;
		FString Remainder;
		FString RequestText;
		FString SuccessText;
		if (!Text.Split(TEXT(":"), &GenerationText, &Remainder) ||
			!Remainder.Split(TEXT(":"), &RequestText, &Remainder) ||
			!Remainder.Split(TEXT(":"), &SuccessText, &OutMessage) ||
			!TryParseGeneration(GenerationText, OutGeneration) ||
			!LexTryParseString(OutRequestId, *RequestText) || OutRequestId == 0 ||
			(SuccessText != TEXT("0") && SuccessText != TEXT("1")))
		{
			return false;
		}
		bOutSuccess = SuccessText == TEXT("1");
		return true;
	}

	bool IsStrictBase64(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() % 4 != 0) return false;
		int32 Padding = 0;
		if (Value.EndsWith(TEXT("=="))) Padding = 2;
		else if (Value.EndsWith(TEXT("="))) Padding = 1;
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			const bool bAlphaNumeric =
				(Character >= TEXT('A') && Character <= TEXT('Z')) ||
				(Character >= TEXT('a') && Character <= TEXT('z')) ||
				(Character >= TEXT('0') && Character <= TEXT('9'));
			if (bAlphaNumeric || Character == TEXT('+') || Character == TEXT('/')) continue;
			if (Character != TEXT('=') || Index < Value.Len() - Padding) return false;
		}
		return true;
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

	const TCHAR* LegendPositionName(const EEChartsLegendPosition Position)
	{
		switch (Position)
		{
		case EEChartsLegendPosition::Top: return TEXT("Top");
		case EEChartsLegendPosition::Bottom: return TEXT("Bottom");
		case EEChartsLegendPosition::Left: return TEXT("Left");
		case EEChartsLegendPosition::Right: return TEXT("Right");
		case EEChartsLegendPosition::Custom: return TEXT("Custom");
		default: return TEXT("Auto");
		}
	}

	const TCHAR* LegendOrientationName(const EEChartsLegendOrientation Orientation)
	{
		switch (Orientation)
		{
		case EEChartsLegendOrientation::Horizontal: return TEXT("Horizontal");
		case EEChartsLegendOrientation::Vertical: return TEXT("Vertical");
		default: return TEXT("Auto");
		}
	}

	FEChartsLegendSettings ClampLegendSettings(const FEChartsLegendSettings& Input)
	{
		FEChartsLegendSettings Result = Input;
		Result.FontSize = FMath::Clamp(Result.FontSize, 6, 72);
		Result.ItemGap = FMath::Clamp(Result.ItemGap, 0, 100);
		Result.ItemWidth = FMath::Clamp(Result.ItemWidth, 1, 100);
		Result.ItemHeight = FMath::Clamp(Result.ItemHeight, 1, 100);
		Result.CustomXPercent = FMath::Clamp(Result.CustomXPercent, 0.0f, 100.0f);
		Result.CustomYPercent = FMath::Clamp(Result.CustomYPercent, 0.0f, 100.0f);
		return Result;
	}

	FString EncodeLegendSettings(const FEChartsLegendSettings& Settings)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("bShow"), Settings.bShow);
		Json->SetStringField(TEXT("position"), LegendPositionName(Settings.Position));
		Json->SetStringField(TEXT("orientation"), LegendOrientationName(Settings.Orientation));
		Json->SetNumberField(TEXT("fontSize"), Settings.FontSize);
		Json->SetNumberField(TEXT("itemGap"), Settings.ItemGap);
		Json->SetNumberField(TEXT("itemWidth"), Settings.ItemWidth);
		Json->SetNumberField(TEXT("itemHeight"), Settings.ItemHeight);
		Json->SetNumberField(TEXT("customXPercent"), Settings.CustomXPercent);
		Json->SetNumberField(TEXT("customYPercent"), Settings.CustomYPercent);
		FString Text;
		const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(Json, Writer)) return FString();
		const FTCHARToUTF8 Utf8(*Text);
		return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
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
	const int32 BoundedCount = SeriesIndex == 0 && SeriesData[0].RingCapacity > 0 && bTimeSeriesEnabled
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
	if (bOptionBarrierActive) return;
	if (!bIsDirty)
	{
		bApplyRequested = false;
		return;
	}
	if (RuntimeState != EEChartsRuntimeState::Ready || InFlightRevision != 0 || bPayloadBuildInFlight) return;
	FString PayloadBase64;
	FString Error;
	int32 PointCount = 0;
	if (IsStreamingActive() && bStreamDeltaReady && !PendingStreamDeltas.IsEmpty())
	{
		const FEChartsPendingStreamDelta& Delta = PendingStreamDeltas[0];
		const int64 SubmissionRevision = Delta.Revision;
		if (!FEChartsPayloadBuilder::BuildStreamDeltaBase64(
			Delta.Added, Delta.DropCount, LastAppliedRevision, SubmissionRevision, PayloadBase64, Error))
		{
			bApplyRequested = false;
			FailStreaming(Error);
			return;
		}
		PointCount = GetTotalPointCount();
		PendingStreamDeltas.RemoveAt(0, 1, EAllowShrinking::No);
		DispatchDataPayload(PayloadBase64, PointCount, SubmissionRevision, true);
		return;
	}
	if (DataTableApplyRevision == DataRevision && !DataTablePayloadBase64.IsEmpty())
	{
		DispatchDataPayload(DataTablePayloadBase64, GetTotalPointCount(), DataRevision, false);
		return;
	}

	const uint64 BuildRequest = ++PayloadBuildRequest;
	const uint64 Generation = LoadGeneration;
	const int64 Revision = DataRevision;
	const EEChartsTemplate Template = CurrentTemplate;
	const EEChartsXAxisMode AxisMode = XAxisMode;
	const bool bPreserveOrder = bPreserveStreamCategoryOrder;
	auto Snapshot = SeriesData;
	bPayloadBuildInFlight = true;
	PayloadBuildRevision = Revision;
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	++PayloadBuildCountForTesting;
	const auto TestGate = PayloadBuildGateForTesting;
	const auto ThreadFlag = PayloadBuildThreadFlagForTesting;
#endif
	const TWeakObjectPtr<UEChartsWidget> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis, BuildRequest, Generation, Revision, Template, AxisMode,
		bPreserveOrder, Snapshot = MoveTemp(Snapshot)
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
		, TestGate, ThreadFlag
#endif
	]() mutable
	{
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
		if (ThreadFlag.IsValid()) *ThreadFlag = IsInGameThread();
		if (TestGate.IsValid() && TestGate->Event) TestGate->Event->Wait();
#endif
		FString BuiltPayload;
		FString BuildError;
		int32 BuiltPointCount = 0;
		const bool bBuilt = FEChartsPayloadBuilder::BuildBase64Payload(
			Template, AxisMode, Snapshot, Revision, BuiltPayload, BuiltPointCount, BuildError, bPreserveOrder);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, BuildRequest, Generation, Revision, Template, AxisMode,
			bPreserveOrder, bBuilt, BuiltPayload = MoveTemp(BuiltPayload), BuildError = MoveTemp(BuildError), BuiltPointCount]() mutable
		{
			UEChartsWidget* Widget = WeakThis.Get();
			if (!Widget || Widget->PayloadBuildRequest != BuildRequest) return;
			Widget->bPayloadBuildInFlight = false;
			if (Widget->RuntimeState != EEChartsRuntimeState::Ready)
			{
				return;
			}
			if (Widget->bOptionBarrierActive)
			{
				Widget->bApplyRequested = true;
				Widget->SendPendingOrCachedOption();
				return;
			}
			if (Generation != Widget->LoadGeneration || Revision != Widget->DataRevision ||
				Template != Widget->CurrentTemplate || AxisMode != Widget->XAxisMode ||
				bPreserveOrder != Widget->bPreserveStreamCategoryOrder)
			{
				Widget->bApplyRequested = true;
				Widget->SubmitLatestData();
				return;
			}
			if (!bBuilt)
			{
				Widget->bApplyRequested = false;
				if (Widget->IsStreamingActive()) Widget->FailStreaming(BuildError);
				else Widget->ReportDataError(BuildError);
				return;
			}
			Widget->DispatchDataPayload(BuiltPayload, BuiltPointCount, Revision, false);
			if (Widget->IsStreamingActive()) Widget->ScheduleStreamTicker();
		});
	});
}

void UEChartsWidget::DispatchDataPayload(
	const FString& PayloadBase64,
	const int32 PointCount,
	const int64 SubmissionRevision,
	const bool bSubmitDelta)
{
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
	if (bOptionBarrierActive || bPayloadBuildInFlight || !bAutoApplyEnabled || !bIsDirty || RuntimeState != EEChartsRuntimeState::Ready ||
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

uint64 UEChartsWidget::AllocateAdvancedRequestId()
{
	static constexpr uint64 MaxJavascriptSafeInteger = 9007199254740991ULL;
	if (NextAdvancedRequestId >= MaxJavascriptSafeInteger)
	{
		NextAdvancedRequestId = 0;
	}
	return ++NextAdvancedRequestId;
}

void UEChartsWidget::ClearPendingAdvancedRequests(const bool bPreserveOptionCandidate)
{
	if (bPreserveOptionCandidate && PendingOptionBase64.IsEmpty() &&
		bInFlightOptionIsCandidate && !InFlightOptionBase64.IsEmpty())
	{
		PendingOptionBase64 = InFlightOptionBase64;
	}
	else if (!bPreserveOptionCandidate)
	{
		PendingOptionBase64.Reset();
	}
	InFlightOptionBase64.Reset();
	bInFlightOptionIsCandidate = false;
	bInFlightOptionReplayPrerequisite = false;
	bReplayBeforePendingCandidate = false;
	PendingOptionRequestId = 0;
	PendingInteractionRequestId = 0;
	PendingLegendRequestId = 0;
	bInteractionModeQueued = false;
	bLegendSettingsQueued = false;
	PendingJavaScriptRequests.Reset();
}

void UEChartsWidget::SendOptionBase64(const FString& OptionBase64, const bool bCandidate)
{
	if (RuntimeState != EEChartsRuntimeState::Ready || PendingOptionRequestId != 0 || OptionBase64.IsEmpty())
	{
		return;
	}
	PendingOptionRequestId = AllocateAdvancedRequestId();
	InFlightOptionBase64 = OptionBase64;
	bInFlightOptionIsCandidate = bCandidate;
	ExecuteJavascript(FEChartsWidgetJavascript::BuildApplyOptionCommand(PendingOptionRequestId, InFlightOptionBase64));
}

void UEChartsWidget::SendPendingOrCachedOption()
{
	if (RuntimeState != EEChartsRuntimeState::Ready || PendingOptionRequestId != 0) return;
	if (bReplayBeforePendingCandidate && !CachedOptionBase64.IsEmpty())
	{
		bReplayBeforePendingCandidate = false;
		bInFlightOptionReplayPrerequisite = true;
		SendOptionBase64(CachedOptionBase64, false);
		return;
	}
	if (!PendingOptionBase64.IsEmpty())
	{
		if (bOptionBarrierActive &&
			(InFlightRevision != 0 || bPayloadBuildInFlight || DataTableLoadState == EEChartsDataTableLoadState::Processing)) return;
		const FString Candidate = MoveTemp(PendingOptionBase64);
		PendingOptionBase64.Reset();
		SendOptionBase64(Candidate, true);
		return;
	}
	if (bOptionReplayPending && CurrentTemplate == EEChartsTemplate::CustomOption && !CachedOptionBase64.IsEmpty())
	{
		SendOptionBase64(CachedOptionBase64, false);
	}
}

void UEChartsWidget::BeginOptionBarrier()
{
	if (bOptionBarrierActive) return;
	bOptionBarrierActive = true;
	bOptionBarrierChainCommitted = false;
	bReplayBeforePendingCandidate = false;
	bInFlightOptionReplayPrerequisite = false;
	CancelStreamTicker();
	if (DataTableTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DataTableTickerHandle);
		DataTableTickerHandle.Reset();
	}
	CancelAutoApply();
}

void UEChartsWidget::ResolveOptionBarrier(const bool bCommit)
{
	if (!bOptionBarrierActive) return;
	bOptionBarrierActive = false;
	if (bCommit)
	{
		if (IsStreamingActive()) StopStreaming(true);
		else if (DataTableLoadState == EEChartsDataTableLoadState::Reading ||
			DataTableLoadState == EEChartsDataTableLoadState::Processing ||
			DataTableLoadState == EEChartsDataTableLoadState::Applying)
		{
			StopDataTableLoad(true);
		}
		InvalidateStreamDelta();
		CancelAutoApply();
		bApplyRequested = false;
	}
	else
	{
		if (!bStreamingSuspended && DataTableLoadState == EEChartsDataTableLoadState::Reading &&
			DataTableSnapshot.IsValid() && !DataTableTickerHandle.IsValid())
		{
			const uint64 Request = DataTableRequest;
			DataTableTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateWeakLambda(this, [this, Request](float) { return ReadDataTableBatch(Request); }), 0.0001f);
		}
		ScheduleStreamTicker();
		if (bApplyRequested && RuntimeState == EEChartsRuntimeState::Ready && InFlightRevision == 0) SubmitLatestData();
		else ScheduleAutoApply();
	}
	bOptionBarrierChainCommitted = false;
	bReplayBeforePendingCandidate = false;
	bInFlightOptionReplayPrerequisite = false;
}

void UEChartsWidget::SendInteractionMode()
{
	if (RuntimeState != EEChartsRuntimeState::Ready || PendingInteractionRequestId != 0) return;
	PendingInteractionRequestId = AllocateAdvancedRequestId();
	InFlightInteractionMode = InteractionMode;
	bInteractionModeQueued = false;
	bInteractionReplayPending = true;
	ExecuteJavascript(FEChartsWidgetJavascript::BuildSetInteractionModeCommand(PendingInteractionRequestId, InteractionMode));
}

void UEChartsWidget::SetInteractionMode(const EEChartsInteractionMode Mode)
{
	if (!IsGameThreadMutation()) return;
	InteractionMode = Mode;
	bInteractionReplayPending = true;
	if (RuntimeState == EEChartsRuntimeState::Ready)
	{
		if (PendingInteractionRequestId != 0) bInteractionModeQueued = InFlightInteractionMode != InteractionMode;
		else SendInteractionMode();
	}
}

void UEChartsWidget::SendLegendSettings()
{
	if (RuntimeState != EEChartsRuntimeState::Ready || PendingLegendRequestId != 0) return;
	const FString Encoded = EncodeLegendSettings(RequestedLegendSettings);
	if (Encoded.IsEmpty()) return;
	PendingLegendRequestId = AllocateAdvancedRequestId();
	InFlightLegendSettings = RequestedLegendSettings;
	bLegendSettingsQueued = false;
	bLegendReplayPending = true;
	ExecuteJavascript(FEChartsWidgetJavascript::BuildSetLegendSettingsCommand(PendingLegendRequestId, Encoded));
}

void UEChartsWidget::SetLegendSettings(const FEChartsLegendSettings& Settings)
{
	if (!IsGameThreadMutation()) return;
	RequestedLegendSettings = ClampLegendSettings(Settings);
	bLegendReplayPending = true;
	if (RuntimeState == EEChartsRuntimeState::Ready)
	{
		if (PendingLegendRequestId != 0) bLegendSettingsQueued = InFlightLegendSettings != RequestedLegendSettings;
		else SendLegendSettings();
	}
}

void UEChartsWidget::ResetLegendSettings()
{
	SetLegendSettings(FEChartsLegendSettings{});
}

bool UEChartsWidget::SetEChartsOptionJSON(const FString& OptionJson)
{
	if (!IsGameThreadMutation() || OptionJson.TrimStartAndEnd().IsEmpty()) return false;
	const FTCHARToUTF8 Utf8(*OptionJson);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaxOptionJsonBytes) return false;

	TSharedPtr<FJsonObject> OptionObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OptionJson);
	if (!FJsonSerializer::Deserialize(Reader, OptionObject) || !OptionObject.IsValid()) return false;

	const FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	if (!IsStrictBase64(Encoded)) return false;

	BeginOptionBarrier();
	PendingOptionBase64 = Encoded;
	if (RuntimeState == EEChartsRuntimeState::Ready)
	{
		SendPendingOrCachedOption();
	}
	return true;
}

bool UEChartsWidget::ExecuteEChartsJavaScript(const FString& JavaScript, int64& OutRequestId)
{
	OutRequestId = 0;
	if (!IsGameThreadMutation() || RuntimeState != EEChartsRuntimeState::Ready || JavaScript.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	const FTCHARToUTF8 Utf8(*JavaScript);
	if (Utf8.Length() <= 0 || Utf8.Length() > MaxJavaScriptBytes) return false;
	const FString Encoded = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	if (!IsStrictBase64(Encoded)) return false;

	const uint64 RequestId = AllocateAdvancedRequestId();
	const FString Command = FEChartsWidgetJavascript::BuildExecuteJavaScriptCommand(RequestId, Encoded);
	if (Command.IsEmpty()) return false;
	PendingJavaScriptRequests.Add(RequestId);
	OutRequestId = static_cast<int64>(RequestId);
	ExecuteJavascript(Command);
	return true;
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
	bInteractionReplayPending = true;
	bLegendReplayPending = true;
	bOptionReplayPending = Template == EEChartsTemplate::CustomOption && !CachedOptionBase64.IsEmpty();
	bHasInitialized = true;
	bReloadOnRebuild = false;
	BeginLoadGeneration();
	LoadURL(InitialURL);
}

void UEChartsWidget::BeginLoadGeneration()
{
	CancelAutoApply();
	ClearPendingAdvancedRequests(true);
	if (bOptionBarrierActive && bOptionBarrierChainCommitted && !CachedOptionBase64.IsEmpty() && !PendingOptionBase64.IsEmpty())
	{
		bReplayBeforePendingCandidate = true;
	}
	bInteractionReplayPending = true;
	bLegendReplayPending = true;
	bOptionReplayPending = CurrentTemplate == EEChartsTemplate::CustomOption && !CachedOptionBase64.IsEmpty();
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
	if (bOptionBarrierActive || (DataTableSnapshot && DataTableSnapshot->bStreaming))
	{
		if (DataTableTickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(DataTableTickerHandle);
		DataTableTickerHandle.Reset();
	}
	else StopDataTableLoad(false);
	CancelAutoApply();
	ClearPendingAdvancedRequests(true);
	if (bOptionBarrierActive && bOptionBarrierChainCommitted && !CachedOptionBase64.IsEmpty() && !PendingOptionBase64.IsEmpty())
	{
		bReplayBeforePendingCandidate = true;
	}
	bInteractionReplayPending = bHasInitialized;
	bLegendReplayPending = bHasInitialized;
	bOptionReplayPending = bHasInitialized && CurrentTemplate == EEChartsTemplate::CustomOption && !CachedOptionBase64.IsEmpty();
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
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	ReleasePayloadBuildForTesting();
#endif
	StopStreaming(false);
	StopDataTableLoad(false);
	CancelAutoApply();
	ClearPendingAdvancedRequests(false);
	OnConsoleMessage.RemoveDynamic(this, &UEChartsWidget::HandleEChartsConsoleMessage);
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
			SendPendingOrCachedOption();
			if (bInteractionReplayPending) SendInteractionMode();
			if (bLegendReplayPending) SendLegendSettings();
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
			if (bOptionBarrierActive)
			{
				SendPendingOrCachedOption();
			}
			else if (bApplyRequested && bIsDirty)
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

	if (Message.StartsWith(OptionResultMarker))
	{
		uint64 MessageGeneration = 0;
		uint64 RequestId = 0;
		bool bSuccess = false;
		FString Detail;
		if (TryParseAdvancedResult(Message.RightChop(OptionResultMarker.Len()), MessageGeneration, RequestId, bSuccess, Detail) &&
			MessageGeneration == LoadGeneration && RequestId == PendingOptionRequestId && RuntimeState == EEChartsRuntimeState::Ready)
		{
			const bool bWasCandidate = bInFlightOptionIsCandidate;
			const bool bWasReplayPrerequisite = bInFlightOptionReplayPrerequisite;
			const FString CompletedOptionBase64 = MoveTemp(InFlightOptionBase64);
			PendingOptionRequestId = 0;
			InFlightOptionBase64.Reset();
			bInFlightOptionIsCandidate = false;
			bInFlightOptionReplayPrerequisite = false;
			// A cached replay is attempted at most once per generation. Keep the
			// last-good payload for a later generation, but never spin-retry it here.
			bOptionReplayPending = false;
			if (bSuccess)
			{
				if (bWasCandidate)
				{
					CachedOptionBase64 = CompletedOptionBase64;
					CurrentTemplate = EEChartsTemplate::CustomOption;
					bOptionBarrierChainCommitted = true;
				}
				EffectiveTemplate = FEChartsWidgetJavascript::TemplateName(EEChartsTemplate::CustomOption);
				CancelAutoApply();
			}
			if (bWasReplayPrerequisite && !bSuccess)
			{
				const uint64 TerminalGeneration = LoadGeneration;
				PendingOptionBase64.Reset();
				bOptionBarrierActive = false;
				bOptionBarrierChainCommitted = false;
				bReplayBeforePendingCandidate = false;
				bInFlightOptionReplayPrerequisite = false;
				++PayloadBuildRequest;
				bPayloadBuildInFlight = false;
				CancelAutoApply();
				RuntimeState = EEChartsRuntimeState::Error;
				LastError = TEXT("Could not restore the last successful CustomOption before applying the pending option.");
				if (IsStreamingActive()) { StopStreaming(false); StreamState = EEChartsDataTableStreamState::Error; }
				if (DataTableLoadState == EEChartsDataTableLoadState::Reading || DataTableLoadState == EEChartsDataTableLoadState::Processing || DataTableLoadState == EEChartsDataTableLoadState::Applying)
				{
					StopDataTableLoad(false); DataTableLoadState = EEChartsDataTableLoadState::Error; LastDataTableError = LastError;
				}
				CancelAutoApply();
				OnOptionApplied.Broadcast(false, Detail);
				if (LoadGeneration != TerminalGeneration || RuntimeState != EEChartsRuntimeState::Error) return;
				OnEChartsError.Broadcast(LastError);
				return;
			}
			OnOptionApplied.Broadcast(bSuccess, Detail);
			if (!Detail.Contains(TEXT("rollback failed:")))
			{
				if (bOptionBarrierActive && bWasCandidate && PendingOptionBase64.IsEmpty() && PendingOptionRequestId == 0)
				{
					ResolveOptionBarrier(bSuccess || bOptionBarrierChainCommitted);
				}
				else SendPendingOrCachedOption();
			}
		}
		return;
	}

	if (Message.StartsWith(InteractionResultMarker))
	{
		uint64 MessageGeneration = 0;
		uint64 RequestId = 0;
		bool bSuccess = false;
		FString Detail;
		if (TryParseAdvancedResult(Message.RightChop(InteractionResultMarker.Len()), MessageGeneration, RequestId, bSuccess, Detail) &&
			MessageGeneration == LoadGeneration && RequestId == PendingInteractionRequestId && RuntimeState == EEChartsRuntimeState::Ready)
		{
			const EEChartsInteractionMode AppliedMode = InFlightInteractionMode;
			const bool bSendLatest = bInteractionModeQueued || AppliedMode != InteractionMode;
			PendingInteractionRequestId = 0;
			bInteractionModeQueued = false;
			bInteractionReplayPending = !bSuccess && !bSendLatest;
			OnInteractionModeApplied.Broadcast(AppliedMode, bSuccess, Detail);
			if (bSendLatest && PendingInteractionRequestId == 0) SendInteractionMode();
		}
		return;
	}

	if (Message.StartsWith(LegendResultMarker))
	{
		uint64 MessageGeneration = 0;
		uint64 RequestId = 0;
		bool bSuccess = false;
		FString Detail;
		if (TryParseAdvancedResult(Message.RightChop(LegendResultMarker.Len()), MessageGeneration, RequestId, bSuccess, Detail) &&
			MessageGeneration == LoadGeneration && RequestId == PendingLegendRequestId && RuntimeState == EEChartsRuntimeState::Ready)
		{
			const FEChartsLegendSettings AppliedSettings = InFlightLegendSettings;
			const bool bSendLatest = bLegendSettingsQueued || AppliedSettings != RequestedLegendSettings;
			PendingLegendRequestId = 0;
			bLegendSettingsQueued = false;
			if (bSuccess) LegendSettings = AppliedSettings;
			else if (!bSendLatest) RequestedLegendSettings = LegendSettings;
			bLegendReplayPending = !bSuccess && !bSendLatest;
			OnLegendSettingsApplied.Broadcast(bSuccess, Detail);
			if (bSendLatest && PendingLegendRequestId == 0) SendLegendSettings();
		}
		return;
	}

	if (Message.StartsWith(JavaScriptResultMarker))
	{
		uint64 MessageGeneration = 0;
		uint64 RequestId = 0;
		bool bSuccess = false;
		FString Detail;
		if (TryParseAdvancedResult(Message.RightChop(JavaScriptResultMarker.Len()), MessageGeneration, RequestId, bSuccess, Detail) &&
			MessageGeneration == LoadGeneration && PendingJavaScriptRequests.Remove(RequestId) > 0 &&
			RuntimeState == EEChartsRuntimeState::Ready)
		{
			OnJavaScriptResult.Broadcast(static_cast<int64>(RequestId), bSuccess, Detail);
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
			bOptionBarrierActive = false;
			bOptionBarrierChainCommitted = false;
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
	if (!IsStrictBase64(PayloadBase64)) return FString();
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

FString FEChartsWidgetJavascript::BuildApplyOptionCommand(const uint64 RequestId, const FString& PayloadBase64)
{
	if (RequestId == 0 || RequestId > 9007199254740991ULL || !IsStrictBase64(PayloadBase64)) return FString();
	return FString::Printf(
		TEXT("window.UEEChartsHost.applyOptionBase64(%llu,\"%s\");"),
		RequestId,
		*PayloadBase64);
}

FString FEChartsWidgetJavascript::BuildSetInteractionModeCommand(
	const uint64 RequestId,
	const EEChartsInteractionMode InteractionMode)
{
	if (RequestId == 0 || RequestId > 9007199254740991ULL) return FString();
	return FString::Printf(
		TEXT("window.UEEChartsHost.setInteractionMode(%llu,\"%s\");"),
		RequestId,
		*InteractionModeName(InteractionMode));
}

FString FEChartsWidgetJavascript::BuildSetLegendSettingsCommand(
	const uint64 RequestId,
	const FString& PayloadBase64)
{
	if (RequestId == 0 || RequestId > 9007199254740991ULL || !IsStrictBase64(PayloadBase64)) return FString();
	return FString::Printf(
		TEXT("window.UEEChartsHost.setLegendSettingsBase64(%llu,\"%s\");"),
		RequestId,
		*PayloadBase64);
}

FString FEChartsWidgetJavascript::BuildExecuteJavaScriptCommand(const uint64 RequestId, const FString& PayloadBase64)
{
	if (RequestId == 0 || RequestId > 9007199254740991ULL || !IsStrictBase64(PayloadBase64)) return FString();
	return FString::Printf(
		TEXT("window.UEEChartsHost.executeJavaScriptBase64(%llu,\"%s\");"),
		RequestId,
		*PayloadBase64);
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
void UEChartsWidget::BlockNextPayloadBuildForTesting()
{
	PayloadBuildGateForTesting = MakeShared<FEChartsPayloadBuildTestGate, ESPMode::ThreadSafe>();
	PayloadBuildThreadFlagForTesting = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(false);
}

void UEChartsWidget::ReleasePayloadBuildForTesting()
{
	if (PayloadBuildGateForTesting.IsValid() && PayloadBuildGateForTesting->Event)
	{
		PayloadBuildGateForTesting->Event->Trigger();
	}
	PayloadBuildGateForTesting.Reset();
}

void UEChartsWidget::CompletePayloadBuildForTesting()
{
	if (!bPayloadBuildInFlight) return;
	++PayloadBuildRequest;
	bPayloadBuildInFlight = false;
	InFlightRevision = PayloadBuildRevision;
	bApplyRequested = false;
}

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
