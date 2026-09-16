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
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEChartsWarning, const FString&, Message);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEChartsError, const FString&, Message);

	UEChartsWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * Starts an asynchronous local-page load for a new generation.
	 * Each call supersedes earlier loads; console events from older generations are ignored.
	 * Chart commands issued before OnChartReady should be cached for the current generation.
	 * Error is terminal for one generation, so call InitializeECharts again to recover.
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

	/** Current generation state. Error remains terminal until InitializeECharts starts a new generation. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsRuntimeState RuntimeState = EEChartsRuntimeState::Uninitialized;

	/** Current generation error text, cleared by InitializeECharts. */
	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	FString LastError;

	/** Broadcast once when the current generation reports Ready. */
	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnChartReady OnChartReady;

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
#endif

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleEChartsConsoleMessage(const FString& Message, const FString& Source, int32 Line);

	void BindConsoleMessage();

	uint64 LoadGeneration = 0;
	bool bReadyBroadcast = false;
};

class ECHARTSWIDGET_API FEChartsWidgetResourceLocator
{
public:
	static FString GetChartHostPath();
	static FString GetChartHostUrl();
	static FString BuildHostPageUrlForPath(const FString& Path);
};
