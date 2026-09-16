#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "Containers/Ticker.h"
#include "EChartsDataTypes.h"
#include "EChartsDataTableTypes.h"
#include "Engine/DataTable.h"
#include "EChartsPayloadBuilder.h"
#include "WebBrowser.h"
#include "EChartsWidget.generated.h"

UENUM(BlueprintType)
enum class EEChartsInteractionMode : uint8
{
	Disabled,
	ClickOnly,
	FullHover
};

UENUM(BlueprintType)
enum class EEChartsRuntimeState : uint8
{
	Uninitialized,
	Loading,
	Ready,
	Error
};

UCLASS(meta = (DisplayName = "ECharts Widget"))
class ECHARTSWIDGET_API UEChartsWidget : public UWebBrowser
{
	GENERATED_BODY()

public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnChartReady);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnChartRendered, EEChartsTemplate, RequestedTemplate, const FString&, EffectiveTemplate);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEChartsWarning, const FString&, Message);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEChartsError, const FString&, Message);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEChartsApplied, int64, Revision, int32, PointCount);

	UEChartsWidget(const FObjectInitializer& ObjectInitializer);

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDataTableLoadProgress, int32, Processed, int32, Total);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDataTableLoaded, int32, Succeeded, int32, Skipped);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDataTableLoadCancelled);
	UFUNCTION(BlueprintCallable, Category="ECharts|DataTable", meta=(DisplayName="Get ECharts DataTable Columns"))
	bool GetEChartsDataTableColumns(UDataTable* Table, TArray<FEChartsDataTableColumn>& OutColumns, FString& OutError) const;
	UFUNCTION(BlueprintCallable, Category="ECharts|DataTable", meta=(DisplayName="Set DataTable Mapping"))
	bool SetDataTableMapping(UDataTable* Table, const FEChartsDataTableMapping& Mapping);
	UFUNCTION(BlueprintCallable, Category="ECharts|DataTable", meta=(DisplayName="Load Data Table"))
	void LoadDataTable(int32 RowsPerFrame = 256);
	UFUNCTION(BlueprintCallable, Category="ECharts|DataTable", meta=(DisplayName="Cancel Data Table Load"))
	void CancelDataTableLoad();
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") EEChartsDataTableLoadState DataTableLoadState = EEChartsDataTableLoadState::Idle;
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") int32 RowsProcessed = 0;
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") int32 RowsSucceeded = 0;
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") int32 RowsSkipped = 0;
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") int32 TotalRows = 0;
	UPROPERTY(BlueprintReadOnly, Category="ECharts|DataTable") FString LastDataTableError;
	UPROPERTY(BlueprintAssignable, Category="ECharts|Event") FOnDataTableLoadProgress OnDataTableLoadProgress;
	UPROPERTY(BlueprintAssignable, Category="ECharts|Event") FOnDataTableLoaded OnDataTableLoaded;
	UPROPERTY(BlueprintAssignable, Category="ECharts|Event") FOnDataTableLoadCancelled OnDataTableLoadCancelled;

	/**
	 * Starts an asynchronous local-page load for a new generation.
	 * Each call supersedes earlier loads; an automatic Slate rebuild after release also starts a new generation.
	 * Console events from older generations are ignored.
	 * Chart commands issued before OnChartReady should be cached for the current generation.
	 * Error is terminal for one generation; InitializeECharts or an automatic Slate rebuild starts recovery in a new generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "ECharts")
	void InitializeECharts(
		EEChartsTemplate Template = EEChartsTemplate::SegmentedAreaLine,
		EEChartsInteractionMode InInteractionMode = EEChartsInteractionMode::ClickOnly);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Add Data Point"))
	bool AddDataPoint(int32 SeriesIndex, double X, double Y);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Add Category Data Point"))
	bool AddCategoryDataPoint(int32 SeriesIndex, const FString& X, double Y);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set Series Data"))
	bool SetSeriesData(int32 SeriesIndex, const TArray<FEChartsDataPoint2D>& Data);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Append Series Data"))
	bool AppendSeriesData(int32 SeriesIndex, const TArray<FEChartsDataPoint2D>& Data);

	UFUNCTION(BlueprintPure, Category = "ECharts|Data", meta = (DisplayName = "Get Series Data"))
	TArray<FEChartsDataPoint2D> GetSeriesData(int32 SeriesIndex) const;

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set Category Series Data"))
	bool SetCategorySeriesData(int32 SeriesIndex, const TArray<FEChartsCategoryDataPoint>& Data);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Append Category Series Data"))
	bool AppendCategorySeriesData(int32 SeriesIndex, const TArray<FEChartsCategoryDataPoint>& Data);

	UFUNCTION(BlueprintPure, Category = "ECharts|Data", meta = (DisplayName = "Get Category Series Data"))
	TArray<FEChartsCategoryDataPoint> GetCategorySeriesData(int32 SeriesIndex) const;

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set 3D Data"))
	bool Set3DData(int32 SeriesIndex, const TArray<FEChartsDataPoint3D>& Data);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Append 3D Data"))
	bool Append3DData(int32 SeriesIndex, const TArray<FEChartsDataPoint3D>& Data);

	UFUNCTION(BlueprintPure, Category = "ECharts|Data", meta = (DisplayName = "Get 3D Data"))
	TArray<FEChartsDataPoint3D> Get3DData(int32 SeriesIndex) const;

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Clear Series"))
	bool ClearSeries(int32 SeriesIndex);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Clear All"))
	void ClearAll();

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set Series Name"))
	bool SetSeriesName(int32 SeriesIndex, const FString& Name);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set X Axis Mode"))
	void SetXAxisMode(EEChartsXAxisMode Mode);

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Apply ECharts Changes"))
	void ApplyEChartsChanges();

	UFUNCTION(BlueprintCallable, Category = "ECharts|Data", meta = (DisplayName = "Set Auto Apply Enabled"))
	void SetAutoApplyEnabled(bool bEnabled, float InMaxUpdatesPerSecond = 10.0f);

	/** Template selected for the current asynchronous load generation. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsTemplate CurrentTemplate = EEChartsTemplate::SegmentedAreaLine;

	/** Interaction behavior selected for the current asynchronous load generation. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsInteractionMode InteractionMode = EEChartsInteractionMode::ClickOnly;

	/** Current generation state. Error remains terminal until InitializeECharts or Slate rebuild starts a new generation. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsRuntimeState RuntimeState = EEChartsRuntimeState::Uninitialized;

	/** Current generation error text, cleared by InitializeECharts. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	FString LastError;

	/** Current generation warning text, including any WebGL fallback reason. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	FString LastWarning;

	/** Template that was actually rendered; may name a 2D WebGL fallback. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	FString EffectiveTemplate;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	EEChartsXAxisMode XAxisMode = EEChartsXAxisMode::ShowAll;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	bool bIsDirty = false;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	int64 LastAppliedRevision = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	int32 LastAppliedPointCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	bool bAutoApplyEnabled = false;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts|Data")
	float MaxUpdatesPerSecond = 10.0f;

	/** Broadcast once when the current generation reports Ready. */
	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnChartReady OnChartReady;

	/** Broadcast once after ECharts accepts the current template option. */
	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnChartRendered OnChartRendered;

	/** Broadcast for a warning emitted by the current non-terminal generation. */
	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnEChartsWarning OnEChartsWarning;

	/** Broadcast once when the current generation enters its terminal Error state. */
	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnEChartsError OnEChartsError;

	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnEChartsApplied OnEChartsApplied;

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
	virtual void BeginDestroy() override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	void RebindConsoleMessageForTesting();
	void PrepareRebuildForTesting();
	void SetForceWebGLUnavailableForTesting(bool bForceUnavailable);
	void SetInitializationPayloadForTesting(const FString& PayloadJson, bool bReportSeriesCount);
	bool IsAutoApplyScheduledForTesting() const;
	int32 GetDataTableWorkerSnapshotPointCountForTesting() const { return DataTableWorkerSnapshotPointCountForTesting; }
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleEChartsConsoleMessage(const FString& Message, const FString& Source, int32 Line);

	void BindConsoleMessage();
	void BeginLoadGeneration();
	void PrepareAutomaticRebuild();
	bool IsValidSeriesIndex(int32 SeriesIndex) const;
	bool IsGameThreadMutation() const;
	bool CanReplacePointCount(int32 SeriesIndex, int32 NewSeriesPointCount) const;
	void MarkDataChanged();
	void ReportDataError(const FString& Message);
	void SubmitLatestData();
	void ScheduleAutoApply();
	void CancelAutoApply();
	int32 GetTotalPointCount() const;
	void StopDataTableLoad(bool bNotify);
	void FailDataTableLoad(const FString& Error);
	bool ReadDataTableBatch(uint64 Request);
	void ProcessDataTableSnapshot(uint64 Request);
	UPROPERTY(Transient) TObjectPtr<UDataTable> MappedDataTable;
	FEChartsDataTableMapping DataTableMapping;
	TSharedPtr<struct FEChartsDataTableSnapshot> DataTableSnapshot;
	FTSTicker::FDelegateHandle DataTableTickerHandle;
	uint64 DataTableRequest = 0;
	uint64 LastDataTableReadFrame = MAX_uint64;
	int64 DataTableApplyRevision = 0;
	FString DataTablePayloadBase64;
	FEChartsSeriesData DataTablePreviousSeries;
	EEChartsXAxisMode DataTablePreviousAxis = EEChartsXAxisMode::ShowAll;
	EEChartsTemplate DataTableRequestTemplate = EEChartsTemplate::SegmentedAreaLine;
	bool bHasDataTableCacheSnapshot = false;
	bool bInstallingDataTable = false;

	uint64 LoadGeneration = 0;
	bool bReadyBroadcast = false;
	bool bRenderedBroadcast = false;
	bool bHasInitialized = false;
	bool bReloadOnRebuild = false;
	bool bHasPresentationState = false;
	TStaticArray<FEChartsSeriesData, FEChartsPayloadBuilder::MaxSeriesCount> SeriesData;
	int64 DataRevision = 0;
	int64 InFlightRevision = 0;
	bool bApplyRequested = false;
	double LastSubmitSeconds = 0.0;
	FTSTicker::FDelegateHandle AutoApplyTickerHandle;
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	bool bForceWebGLUnavailableForTesting = false;
	int32 DataTableWorkerSnapshotPointCountForTesting = 0;
	bool bReportSeriesCountForTesting = false;
	FString InitializationPayloadForTesting = TEXT("{}");
#endif
};

class ECHARTSWIDGET_API FEChartsWidgetJavascript
{
public:
	static FString TemplateName(EEChartsTemplate Template);
	static FString InteractionModeName(EEChartsInteractionMode InteractionMode);
	static FString BuildRenderCommand(
		EEChartsTemplate Template,
		EEChartsInteractionMode InteractionMode,
		const FString& PayloadJson);
	static FString BuildApplyDataCommand(const FString& PayloadBase64);
};

class ECHARTSWIDGET_API FEChartsWidgetResourceLocator
{
public:
	static FString GetChartHostPath();
	static FString GetChartHostUrl();
	static FString BuildHostPageUrlForPath(const FString& Path);
};
