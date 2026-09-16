#include "EChartsWidget.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "EChartsWidget"

namespace
{
	const FString ReadyMarker = TEXT("__UE_ECHARTS_READY__");
	const FString WarningMarker = TEXT("__UE_ECHARTS_WARNING__:");
	const FString ErrorMarker = TEXT("__UE_ECHARTS_ERROR__:");
}

UEChartsWidget::UEChartsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bSupportsTransparency = true;
	BindConsoleMessage();
}

void UEChartsWidget::InitializeECharts(
	const EEChartsTemplate Template,
	const EEChartsInteractionMode InInteractionMode)
{
	BindConsoleMessage();
	CurrentTemplate = Template;
	InteractionMode = InInteractionMode;
	RuntimeState = EEChartsRuntimeState::Loading;
	LastError.Reset();
	bReadyBroadcast = false;

	InitialURL = FEChartsWidgetResourceLocator::GetChartHostUrl();
	LoadURL(InitialURL);
}

void UEChartsWidget::ReleaseSlateResources(const bool bReleaseChildren)
{
	OnConsoleMessage.RemoveDynamic(this, &UEChartsWidget::HandleEChartsConsoleMessage);
	bReadyBroadcast = false;
	RuntimeState = EEChartsRuntimeState::Uninitialized;
	LastError.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

TSharedRef<SWidget> UEChartsWidget::RebuildWidget()
{
	BindConsoleMessage();
	return Super::RebuildWidget();
}

void UEChartsWidget::HandleEChartsConsoleMessage(
	const FString& Message,
	const FString& Source,
	const int32 Line)
{
	if (Message == ReadyMarker)
	{
		if (!bReadyBroadcast)
		{
			bReadyBroadcast = true;
			RuntimeState = EEChartsRuntimeState::Ready;
			OnChartReady.Broadcast();
		}
		return;
	}

	if (Message.StartsWith(WarningMarker))
	{
		OnEChartsWarning.Broadcast(Message.RightChop(WarningMarker.Len()));
		return;
	}

	if (Message.StartsWith(ErrorMarker))
	{
		LastError = Message.RightChop(ErrorMarker.Len());
		RuntimeState = EEChartsRuntimeState::Error;
		OnEChartsError.Broadcast(LastError);
	}
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

	FString EncodedPath = FGenericPlatformHttp::UrlEncode(AbsolutePath);
	EncodedPath.ReplaceInline(TEXT("%2F"), TEXT("/"), ESearchCase::IgnoreCase);
	EncodedPath.ReplaceInline(TEXT("%3A"), TEXT(":"), ESearchCase::IgnoreCase);
	return FString(TEXT("file:///")) + EncodedPath;
}

#undef LOCTEXT_NAMESPACE
