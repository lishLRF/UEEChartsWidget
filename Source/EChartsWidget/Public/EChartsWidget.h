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

	UFUNCTION(BlueprintCallable, Category = "ECharts")
	void InitializeECharts(
		EEChartsTemplate Template = EEChartsTemplate::SegmentedAreaLine,
		EEChartsInteractionMode InInteractionMode = EEChartsInteractionMode::ClickOnly);

	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsTemplate CurrentTemplate = EEChartsTemplate::SegmentedAreaLine;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsInteractionMode InteractionMode = EEChartsInteractionMode::ClickOnly;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	EEChartsRuntimeState RuntimeState = EEChartsRuntimeState::Uninitialized;

	UPROPERTY(BlueprintReadOnly, Category = "ECharts")
	FString LastError;

	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnChartReady OnChartReady;

	UPROPERTY(BlueprintAssignable, Category = "ECharts|Event")
	FOnEChartsWarning OnEChartsWarning;

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

	bool bReadyBroadcast = false;
};

class ECHARTSWIDGET_API FEChartsWidgetResourceLocator
{
public:
	static FString GetChartHostPath();
	static FString GetChartHostUrl();
	static FString ToFileUrl(const FString& AbsolutePath);
};
