#pragma once

#include "CoreMinimal.h"
#include "WebBrowser.h"
#include "EChartsWidget.generated.h"

UENUM(BlueprintType)
enum class EEChartsTemplate : uint8
{
	SegmentedAreaLine,
	Bar3DHeightMap,
	DataTableScatter3D,
	CustomOption
};

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

	UEChartsWidget(const FObjectInitializer& ObjectInitializer);

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

	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

#if WITH_EDITOR
	virtual const FText GetPaletteCategory() override;
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	void RebindConsoleMessageForTesting();
	void PrepareRebuildForTesting();
	void SetForceWebGLUnavailableForTesting(bool bForceUnavailable);
	void SetInitializationPayloadForTesting(const FString& PayloadJson, bool bReportSeriesCount);
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleEChartsConsoleMessage(const FString& Message, const FString& Source, int32 Line);

	void BindConsoleMessage();
	void BeginLoadGeneration();
	void PrepareAutomaticRebuild();

	uint64 LoadGeneration = 0;
	bool bReadyBroadcast = false;
	bool bRenderedBroadcast = false;
	bool bHasInitialized = false;
	bool bReloadOnRebuild = false;
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
	bool bForceWebGLUnavailableForTesting = false;
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
};

class ECHARTSWIDGET_API FEChartsWidgetResourceLocator
{
public:
	static FString GetChartHostPath();
	static FString GetChartHostUrl();
	static FString BuildHostPageUrlForPath(const FString& Path);
};
