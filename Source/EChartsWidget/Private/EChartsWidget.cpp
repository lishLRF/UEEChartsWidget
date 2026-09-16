#include "EChartsWidget.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "EChartsWidget"

namespace
{
	const FString ReadyMarker = TEXT("__UE_ECHARTS_READY__:");
	const FString RenderedMarker = TEXT("__UE_ECHARTS_RENDERED__:");
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
	BindConsoleMessage();
}

void UEChartsWidget::InitializeECharts(
	const EEChartsTemplate Template,
	const EEChartsInteractionMode InInteractionMode)
{
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
#endif
}

void UEChartsWidget::ReleaseSlateResources(const bool bReleaseChildren)
{
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
			ExecuteJavascript(FEChartsWidgetJavascript::BuildRenderCommand(
				CurrentTemplate,
				InteractionMode,
				TEXT("{}")));
			OnChartReady.Broadcast();
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
