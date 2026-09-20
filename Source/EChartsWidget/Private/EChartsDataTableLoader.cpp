#include "EChartsDataTableLoader.h"
#include "EChartsPayloadBuilder.h"
#include "Algo/StableSort.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"

namespace
{
EEChartsDataTableColumnType Classify(const FProperty* Property)
{
	if (!Property || Property->ArrayDim != 1)
		return EEChartsDataTableColumnType::Unsupported;
	if (CastField<FEnumProperty>(Property) ||
	    (CastField<FByteProperty>(Property) && CastField<FByteProperty>(Property)->Enum))
		return EEChartsDataTableColumnType::Category;
	if (CastField<FNumericProperty>(Property))
		return EEChartsDataTableColumnType::Numeric;
	if (CastField<FStrProperty>(Property) || CastField<FNameProperty>(Property) || CastField<FTextProperty>(Property))
		return EEChartsDataTableColumnType::Category;
	return EEChartsDataTableColumnType::Unsupported;
}
bool Numeric(const FProperty* Property, const uint8* Row, double& Out)
{
	const FNumericProperty* Number = CastField<FNumericProperty>(Property);
	if (!Number)
		return false;
	const void* Value = Number->ContainerPtrToValuePtr<void>(Row);
	const bool bUnsigned = CastField<FByteProperty>(Property) || CastField<FUInt16Property>(Property) ||
	                       CastField<FUInt32Property>(Property) || CastField<FUInt64Property>(Property);
	Out = Number->IsFloatingPoint() ? Number->GetFloatingPointPropertyValue(Value)
	      : bUnsigned               ? static_cast<double>(Number->GetUnsignedIntPropertyValue(Value))
	                                : static_cast<double>(Number->GetSignedIntPropertyValue(Value));
	return FMath::IsFinite(Out);
}
FString Category(const FProperty* Property, const uint8* Row)
{
	const void* Value = Property->ContainerPtrToValuePtr<void>(Row);
	if (const FStrProperty* P = CastField<FStrProperty>(Property))
		return P->GetPropertyValue(Value);
	if (const FNameProperty* P = CastField<FNameProperty>(Property))
		return P->GetPropertyValue(Value).ToString();
	if (const FTextProperty* P = CastField<FTextProperty>(Property))
		return P->GetPropertyValue(Value).ToString();
	if (const FEnumProperty* P = CastField<FEnumProperty>(Property))
		return P->GetEnum()->GetNameStringByValue(P->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
	if (const FByteProperty* P = CastField<FByteProperty>(Property))
		return P->Enum->GetNameStringByValue(P->GetPropertyValue(Value));
	return FString();
}
void StableSortRows(TArray<FEChartsDataTableRow>& Rows, const bool bCategory, const EEChartsDataTableOrder Order)
{
	Algo::StableSort(Rows, [=](const auto& A, const auto& B)
	{
		if (A.bValidSortKey != B.bValidSortKey) return A.bValidSortKey;
		if (!A.bValidSortKey) return false;
		if (Order == EEChartsDataTableOrder::RowName)
		{
			const int32 Comparison = A.RowName.Compare(B.RowName, ESearchCase::IgnoreCase);
			return Comparison == 0 ? A.RowNameNumber < B.RowNameNumber : Comparison < 0;
		}
		return bCategory ? A.Category.Compare(B.Category, ESearchCase::CaseSensitive) < 0 : A.Point.X < B.Point.X;
	});
}
} // namespace
bool EChartsDataTableLoader::Columns(UDataTable* Table, TArray<FEChartsDataTableColumn>& Out, FString& Error)
{
	check(IsInGameThread());
	Out.Reset();
	Error.Reset();
	if (!Table || !Table->GetRowStruct())
	{
		Error = TEXT("DataTable and its row structure are required.");
		return false;
	}
	for (TFieldIterator<FProperty> It(Table->GetRowStruct()); It; ++It)
	{
		FEChartsDataTableColumn C;
		C.Name = It->GetFName();
		C.ColumnType = Classify(*It);
		C.bCanUseAsNumeric = C.ColumnType == EEChartsDataTableColumnType::Numeric;
		C.bCanUseAsX = C.ColumnType != EEChartsDataTableColumnType::Unsupported;
		Out.Add(C);
	}
	return true;
}
bool EChartsDataTableLoader::Validate(
    UDataTable* Table, const FEChartsDataTableMapping& M, EEChartsTemplate Template, bool& bCategory, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	if (!Table || !Table->GetRowStruct())
	{
		Error = TEXT("DataTable and its row structure are required.");
		return false;
	}
	const bool b3D = Template == EEChartsTemplate::Bar3DHeightMap || Template == EEChartsTemplate::DataTableScatter3D;
	auto Type = [Table](FName Name) { return Classify(FindFProperty<FProperty>(Table->GetRowStruct(), Name)); };
	const auto XType = Type(M.X);
	bCategory = XType == EEChartsDataTableColumnType::Category;
	if (M.X.IsNone() || XType == EEChartsDataTableColumnType::Unsupported || (b3D && bCategory))
	{
		Error = TEXT("X must be a supported category or numeric column; 3D requires numeric X.");
		return false;
	}
	if (M.Y.IsNone() || Type(M.Y) != EEChartsDataTableColumnType::Numeric)
	{
		Error = TEXT("Y must name a numeric column (bool is not numeric).");
		return false;
	}
	if (b3D && (M.Z.IsNone() || Type(M.Z) != EEChartsDataTableColumnType::Numeric))
	{
		Error = TEXT("3D Z must name a numeric column.");
		return false;
	}
	for (FName Optional : {M.Color, M.SymbolSize})
		if (!Optional.IsNone() && Type(Optional) != EEChartsDataTableColumnType::Numeric)
		{
			Error = TEXT("Mapped Color and SymbolSize columns must be numeric.");
			return false;
		}
	return true;
}
bool EChartsDataTableLoader::ReadRow(
    UDataTable* Table, FName RowName, const FEChartsDataTableSnapshot& S, FEChartsDataTableRow& Out)
{
	check(IsInGameThread());
	const uint8* const* Found = Table->GetRowMap().Find(RowName);
	if (!Found || !*Found)
		return false;
	const uint8* Row = *Found;
	auto Prop = [Table](FName Name) { return FindFProperty<FProperty>(Table->GetRowStruct(), Name); };
	Out.RowKey = RowName;
	Out.RowName = RowName.GetPlainNameString();
	Out.RowNameNumber = RowName.GetNumber();
	if (S.bCategory)
	{
		Out.Category = Category(Prop(S.Mapping.X), Row);
		if (Out.Category.TrimStartAndEnd().IsEmpty())
			return false;
	}
	else if (!Numeric(Prop(S.Mapping.X), Row, Out.Point.X))
		return false;
	if (!Numeric(Prop(S.Mapping.Y), Row, Out.Point.Y))
		return false;
	if (S.b3D && !Numeric(Prop(S.Mapping.Z), Row, Out.Point.Z))
		return false;
	Out.Point.ColorValue = Out.Point.Z;
	Out.Point.SymbolSizeValue = 12.0;
	if (!S.Mapping.Color.IsNone() && !Numeric(Prop(S.Mapping.Color), Row, Out.Point.ColorValue))
		return false;
	if (!S.Mapping.SymbolSize.IsNone() && !Numeric(Prop(S.Mapping.SymbolSize), Row, Out.Point.SymbolSizeValue))
		return false;
	Out.bValidSortKey = true;
	return true;
}
bool EChartsDataTableLoader::ReadSortKey(
	UDataTable* Table, FName RowName, const FEChartsDataTableSnapshot& S,
	int64& InOutEstimatedJsonBytes, FEChartsDataTableRow& Out, FString& Error)
{
	check(IsInGameThread());
	Out.RowKey = RowName;
	if (S.Mapping.Order == EEChartsDataTableOrder::RowName)
	{
		Out.RowName = RowName.GetPlainNameString();
		Out.RowNameNumber = RowName.GetNumber();
		Out.bValidSortKey = true;
		return true;
	}
	const uint8* const* Found = Table->GetRowMap().Find(RowName);
	if (!Found || !*Found) return true;
	const FProperty* X = FindFProperty<FProperty>(Table->GetRowStruct(), S.Mapping.X);
	if (S.bCategory)
	{
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(X))
		{
			const FString* Value = StringProperty->ContainerPtrToValuePtr<FString>(*Found);
			if (!Value || !FEChartsPayloadBuilder::AccumulateJsonStringBytes(
				*Value, FEChartsPayloadBuilder::MaxJsonBytes, InOutEstimatedJsonBytes))
			{
				Error = FString::Printf(TEXT("DataTable stream sort keys exceed the %d byte JSON safety limit."),
					FEChartsPayloadBuilder::MaxJsonBytes);
				return false;
			}
			Out.Category = *Value;
		}
		else
		{
			FString Value = Category(X, *Found);
			if (!FEChartsPayloadBuilder::AccumulateJsonStringBytes(
				Value, FEChartsPayloadBuilder::MaxJsonBytes, InOutEstimatedJsonBytes))
			{
				Error = FString::Printf(TEXT("DataTable stream sort keys exceed the %d byte JSON safety limit."),
					FEChartsPayloadBuilder::MaxJsonBytes);
				return false;
			}
			Out.Category = MoveTemp(Value);
		}
		Out.bValidSortKey = !Out.Category.TrimStartAndEnd().IsEmpty();
	}
	else
	{
		Out.bValidSortKey = Numeric(X, *Found, Out.Point.X);
	}
	return true;
}
TArray<FName> EChartsDataTableLoader::SortRowNames(
	TArray<FEChartsDataTableRow> Rows, const bool bCategory, const EEChartsDataTableOrder Order)
{
	StableSortRows(Rows, bCategory, Order);
	TArray<FName> Result;
	Result.Reserve(Rows.Num());
	for (const FEChartsDataTableRow& Row : Rows) Result.Add(Row.RowKey);
	return Result;
}
FEChartsSeriesData EChartsDataTableLoader::Convert(
    TArray<FEChartsDataTableRow> Rows, bool bCategory, bool b3D, EEChartsDataTableOrder Order)
{
	StableSortRows(Rows, bCategory, Order);
	FEChartsSeriesData Series;
	Series.Type = b3D         ? EEChartsSeriesDataType::Data3D
	              : bCategory ? EEChartsSeriesDataType::Category
	                          : EEChartsSeriesDataType::Numeric2D;
	for (auto& Row : Rows)
	{
		if (b3D)
			Series.Data3D.Add(Row.Point);
		else if (bCategory)
			Series.Category.Add({MoveTemp(Row.Category), Row.Point.Y});
		else
			Series.Numeric2D.Add({Row.Point.X, Row.Point.Y});
	}
	return Series;
}
