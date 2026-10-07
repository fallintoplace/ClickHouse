#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnMap.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Common/assert_cast.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{

struct NameMapGetOrDefault
{
    static constexpr auto name = "mapGetOrDefault";
};

class FunctionMapGetOrDefault final : public IFunction
{
public:
    static constexpr auto name = NameMapGetOrDefault::name;

    static FunctionPtr create(ContextPtr context) { return std::make_shared<FunctionMapGetOrDefault>(context); }

    explicit FunctionMapGetOrDefault(const ContextPtr & context)
        : index_of(FunctionFactory::instance().get("indexOf", context))
        , array_element(FunctionFactory::instance().get("arrayElement", context))
        , function_if(FunctionFactory::instance().get("if", context))
    {
    }

    String getName() const override { return name; }
    size_t getNumberOfArguments() const override { return 3; }
    bool useDefaultImplementationForNulls() const override { return false; }
    bool useDefaultImplementationForNothing() const override { return false; }
    bool useDefaultImplementationForConstants() const override { return true; }
    bool useDefaultImplementationForLowCardinalityColumns() const override { return false; }

    bool isShortCircuit(ShortCircuitSettings & settings, size_t number_of_arguments) const override
    {
        for (size_t i = 0; i + 1 < number_of_arguments; ++i)
            settings.arguments_with_disabled_lazy_execution.insert(i);

        settings.enable_lazy_execution_for_common_descendants_of_arguments = false;
        settings.force_enable_lazy_execution = false;
        return true;
    }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo &) const override { return true; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 3)
            throw Exception(
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Number of arguments for function {} doesn't match: passed {}, should be 3",
                getName(),
                arguments.size());

        const auto * map_type = checkAndGetDataType<DataTypeMap>(arguments[0].type.get());
        if (!map_type)
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "First argument for function {} must be a Map, found {}",
                getName(),
                arguments[0].type->getName());

        auto keys_type = std::make_shared<DataTypeArray>(map_type->getKeyType());
        ColumnsWithTypeAndName index_arguments{
            {nullptr, keys_type, ""},
            {nullptr, arguments[1].type, ""}};
        auto index_function = index_of->build(index_arguments);
        auto index_type = index_function->getResultType();

        auto values_type = std::make_shared<DataTypeArray>(map_type->getValueType());
        ColumnsWithTypeAndName element_arguments{
            {nullptr, values_type, ""},
            {nullptr, index_type, ""}};
        auto element_function = array_element->build(element_arguments);

        ColumnsWithTypeAndName if_arguments{
            {nullptr, std::make_shared<DataTypeUInt8>(), ""},
            {nullptr, element_function->getResultType(), ""},
            {nullptr, arguments[2].type, ""}};
        return function_if->build(if_arguments)->getResultType();
    }

    ColumnPtr executeImpl(
        const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        const auto * map = checkAndGetColumn<ColumnMap>(arguments[0].column.get());
        const auto * const_map = checkAndGetColumnConstData<ColumnMap>(arguments[0].column.get());
        if (!map && !const_map)
            throw Exception(
                ErrorCodes::ILLEGAL_COLUMN,
                "First argument for function {} must be a Map column, found {}",
                getName(),
                arguments[0].column->getName());

        const bool is_const_map = const_map != nullptr;
        if (is_const_map)
            map = const_map;

        const auto & map_type = assert_cast<const DataTypeMap &>(*arguments[0].type);
        const auto & nested = map->getNestedColumn();
        const auto & entries = map->getNestedData();

        ColumnPtr keys = ColumnArray::create(entries.getColumnPtr(0), nested.getOffsetsPtr());
        ColumnPtr values = ColumnArray::create(entries.getColumnPtr(1), nested.getOffsetsPtr());
        if (is_const_map)
        {
            keys = ColumnConst::create(std::move(keys), input_rows_count);
            values = ColumnConst::create(std::move(values), input_rows_count);
        }

        auto keys_type = std::make_shared<DataTypeArray>(map_type.getKeyType());
        ColumnsWithTypeAndName index_arguments{
            {std::move(keys), keys_type, ""},
            arguments[1]};
        auto index_function = index_of->build(index_arguments);
        auto index_type = index_function->getResultType();
        auto positions = index_function->execute(
            index_arguments, index_type, input_rows_count, /* dry_run = */ false);

        const auto * const_positions = checkAndGetColumnConst<ColumnUInt64>(positions.get());
        const UInt64 constant_position = const_positions ? const_positions->getValue<UInt64>() : 0;

        ColumnPtr found;
        if (const_positions)
        {
            found = DataTypeUInt8().createColumnConst(input_rows_count, constant_position != 0);
        }
        else
        {
            const auto & position_data = assert_cast<const ColumnUInt64 &>(*positions).getData();
            auto found_column = ColumnUInt8::create(input_rows_count);
            auto & found_data = found_column->getData();
            for (size_t row = 0; row < input_rows_count; ++row)
                found_data[row] = static_cast<UInt8>(position_data[row] != 0);
            found = std::move(found_column);
        }

        auto values_type = std::make_shared<DataTypeArray>(map_type.getValueType());
        ColumnsWithTypeAndName element_arguments{
            {std::move(values), values_type, ""},
            {positions, index_type, ""}};
        auto element_function = array_element->build(element_arguments);

        ColumnPtr element_column;
        if (const_positions && constant_position == 0)
        {
            /// `arrayElement` rejects a constant index 0. For a missing constant key, provide
            /// the same default element without materializing the position to a full column.
            element_column = element_function->getResultType()->createColumnConstWithDefaultValue(input_rows_count);
        }
        else
        {
            element_column = element_function->execute(
                element_arguments, element_function->getResultType(), input_rows_count, /* dry_run = */ false);
        }

        ColumnsWithTypeAndName if_arguments{
            {std::move(found), std::make_shared<DataTypeUInt8>(), ""},
            {std::move(element_column), element_function->getResultType(), ""},
            arguments[2]};

        auto if_function = function_if->build(if_arguments);
        return if_function->execute(if_arguments, result_type, input_rows_count, /* dry_run = */ false);
    }

private:
    FunctionOverloadResolverPtr index_of;
    FunctionOverloadResolverPtr array_element;
    FunctionOverloadResolverPtr function_if;
};

}

REGISTER_FUNCTION(MapGetOrDefault)
{
    FunctionDocumentation::Description description = R"(
Returns the value associated with a key in a map, or a caller-provided default value when the key is absent.
If the map contains duplicate keys, the first matching value is returned.
)";
    FunctionDocumentation::Syntax syntax = "mapGetOrDefault(map, key, default_value)";
    FunctionDocumentation::Arguments arguments = {
        {"map", "Map to search.", {"Map(K, V)"}},
        {"key", "Key to search for. It follows the same comparison rules as mapContainsKey.", {"Any"}},
        {"default_value", "Value returned when the key is absent.", {"Any"}}
    };
    FunctionDocumentation::ReturnedValue returned_value = {
        "Returns the value associated with key if present, otherwise default_value. The result type is the common type of map element access and default_value.",
        {"Any"}};
    FunctionDocumentation::Examples examples = {
        {"Present key", "SELECT mapGetOrDefault(map('a', 1, 'b', 2), 'b', 42)", "2"},
        {"Missing key", "SELECT mapGetOrDefault(map('a', 1, 'b', 2), 'c', 42)", "42"},
        {"Nullable default", "SELECT mapGetOrDefault(map('a', toUInt8(1)), 'c', NULL)", "\\N"}
    };
    FunctionDocumentation::IntroducedIn introduced_in = {26, 10};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::Map;
    FunctionDocumentation documentation = {
        description,
        syntax,
        arguments,
        {},
        returned_value,
        examples,
        introduced_in,
        category};
    factory.registerFunction<FunctionMapGetOrDefault>(documentation);
}

}
