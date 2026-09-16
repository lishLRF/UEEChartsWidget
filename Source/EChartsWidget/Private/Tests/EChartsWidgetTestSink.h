#pragma once

#include "CoreMinimal.h"
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
	int32 WarningCount = 0;
	int32 ErrorCount = 0;
	FString LastWarning;
	FString LastError;
};
