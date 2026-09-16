#pragma once
#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EChartsDataTableTestRow.generated.h"

UENUM()
enum class EEChartsTestCategory : uint8
{
	Apple,
	Banana
};
USTRUCT()
struct FEChartsDataTableTestRow : public FTableRowBase
{
	GENERATED_BODY()
	UPROPERTY() double X = 0;
	UPROPERTY() float Y = 0;
	UPROPERTY() int32 Z = 7;
	UPROPERTY() int64 Integer = 9;
	UPROPERTY() FString Category = TEXT("Beta");
	UPROPERTY() FName Name = TEXT("Name");
	UPROPERTY() FText Text = FText::FromString(TEXT("Text"));
	UPROPERTY() EEChartsTestCategory Enum = EEChartsTestCategory::Apple;
	UPROPERTY() bool Flag = true;
	UPROPERTY() FVector Nested = FVector::ZeroVector;
	UPROPERTY() TArray<float> Array;
};
