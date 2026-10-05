#include <Columns/ColumnConst.h>
#include <Columns/ColumnString.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/JSONPath/ASTs/ASTJSONPath.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathMemberAccess.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathRange.h>
#include <Functions/JSONPath/ASTs/ASTJSONPathRoot.h>
#include <Functions/JSONPath/Parsers/ParserJSONPath.h>
#include <Interpreters/Context.h>
#include <Parsers/Lexer.h>
#include <Common/JSONParsers/RapidJSONMemoryTrackerAllocator.h>
#include <Common/VectorWithMemoryTracking.h>
#include "config.h"

#include <string_view>
#include <utility>

#if USE_RAPIDJSON

#define RAPIDJSON_PARSE_DEFAULT_FLAGS (kParseIterativeFlag)

#include <rapidjson/error/en.h>
#include <rapidjson/reader.h>

namespace DB
{
namespace Setting
{
extern const SettingsUInt64 max_parser_backtracks;
extern const SettingsUInt64 max_parser_depth;
} // namespace Setting

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int ILLEGAL_COLUMN;
extern const int ILLEGAL_TYPE_OF_ARGUMENT;
extern const int TOO_DEEP_RECURSION;
} // namespace ErrorCodes

namespace
{
using TrackedReader = rapidjson::GenericReader<rapidjson::UTF8<char>, rapidjson::UTF8<char>, RapidJSONMemoryTrackerAllocator>;

constexpr size_t max_json_remove_depth = 1000;

struct PathStep
{
    enum class Type
    {
        Member,
        Index,
    };

    Type type;
    String member_name;
    UInt32 index = 0;
};

using ParsedPath = VectorWithMemoryTracking<PathStep>;
using ParsedPaths = VectorWithMemoryTracking<ParsedPath>;

struct JSONSlice
{
    size_t offset = 0;
    size_t length = 0;
};

using JSONNodeIndex = size_t;

struct JSONObjectMember
{
    String name;
    JSONSlice raw_name;
    JSONNodeIndex value;
};

struct JSONNode
{
    enum class Type
    {
        Scalar,
        Object,
        Array,
    };

    Type type = Type::Scalar;
    JSONSlice raw_value;
    VectorWithMemoryTracking<JSONObjectMember> members;
    VectorWithMemoryTracking<JSONNodeIndex> elements;
};

using JSONNodes = VectorWithMemoryTracking<JSONNode>;

/// RapidJSON keeps an optimized local copy of MemoryStream while parsing strings and numbers,
/// so a SAX handler cannot observe its live cursor. This custom stream uses RapidJSON's default
/// reference semantics and remembers the start of each string token, letting the handler point
/// nodes directly at slices of the original JSON.
class JSONInputStream
{
public:
    using Ch = char;

    explicit JSONInputStream(std::string_view data_)
        : data(data_)
    {
    }

    Ch Peek() const { return position == data.size() ? '\0' : data[position]; }

    Ch Take()
    {
        if (position == data.size())
            return '\0';

        const size_t offset = position;
        const Ch value = data[position++];
        if (in_string)
        {
            if (escaped)
                escaped = false;
            else if (value == '\\')
                escaped = true;
            else if (value == '"')
                in_string = false;
        }
        else if (value == '"')
        {
            in_string = true;
            last_string_start = offset;
        }

        return value;
    }

    size_t Tell() const { return position; }
    size_t getLastStringStart() const { return last_string_start; }

    Ch * PutBegin() { return nullptr; }
    void Put(Ch) { }
    void Flush() { }
    size_t PutEnd(Ch *) { return 0; }

private:
    std::string_view data;
    size_t position = 0;
    size_t last_string_start = 0;
    bool in_string = false;
    bool escaped = false;
};

class JSONTreeBuilder : public rapidjson::BaseReaderHandler<rapidjson::UTF8<char>, JSONTreeBuilder>
{
public:
    JSONTreeBuilder(JSONInputStream & stream_, size_t max_depth_)
        : stream(stream_)
        , max_depth(max_depth_)
    {
    }

    bool Null()
    {
        addScalarFromEnd(4);
        return true;
    }

    bool Bool(bool value)
    {
        addScalarFromEnd(value ? 4 : 5);
        return true;
    }

    bool RawNumber(const char *, rapidjson::SizeType length, bool)
    {
        addScalarFromEnd(length);
        return true;
    }

    bool String(const char *, rapidjson::SizeType, bool)
    {
        checkValueDepth();
        addNode(JSONNode::Type::Scalar, getRawStringSlice());
        return true;
    }

    bool StartObject()
    {
        checkValueDepth();
        const auto node = addNode(JSONNode::Type::Object, {});
        stack.push_back({node, {}, {}});
        return true;
    }

    bool Key(const char * value, rapidjson::SizeType length, bool)
    {
        auto & frame = stack.back();
        frame.member_name.assign(value, length);
        frame.raw_member_name = getRawStringSlice();
        return true;
    }

    bool EndObject(rapidjson::SizeType)
    {
        stack.pop_back();
        return true;
    }

    bool StartArray()
    {
        checkValueDepth();
        const auto node = addNode(JSONNode::Type::Array, {});
        stack.push_back({node, {}, {}});
        return true;
    }

    bool EndArray(rapidjson::SizeType)
    {
        stack.pop_back();
        return true;
    }

    JSONNodes releaseNodes() { return std::move(nodes); }
    JSONNodeIndex getRoot() const { return root; }

private:
    struct ContainerFrame
    {
        JSONNodeIndex node;
        String member_name;
        JSONSlice raw_member_name;
    };

    using ContainerStack = VectorWithMemoryTracking<ContainerFrame>;

    void checkValueDepth() const
    {
        const size_t depth = stack.size() + 1;
        if (depth > max_depth)
            throw Exception(
                ErrorCodes::TOO_DEEP_RECURSION,
                "Too deep nesting in a JSON document passed to function JSONRemove: the limit is {}",
                max_depth);
    }

    JSONSlice getRawStringSlice() const
    {
        const size_t end_offset = stream.Tell();
        const size_t start_offset = stream.getLastStringStart();
        return {start_offset, end_offset - start_offset};
    }

    void addScalarFromEnd(size_t length)
    {
        checkValueDepth();
        const size_t end_offset = stream.Tell();
        addNode(JSONNode::Type::Scalar, {end_offset - length, length});
    }

    JSONNodeIndex addNode(JSONNode::Type type, JSONSlice raw_value)
    {
        JSONNode node;
        node.type = type;
        node.raw_value = raw_value;
        nodes.push_back(std::move(node));

        const auto node_index = nodes.size() - 1;
        if (stack.empty())
        {
            root = node_index;
            return node_index;
        }

        auto & frame = stack.back();
        auto & parent = nodes[frame.node];
        if (parent.type == JSONNode::Type::Object)
        {
            parent.members.push_back({std::move(frame.member_name), frame.raw_member_name, node_index});
            frame.member_name.clear();
        }
        else
        {
            parent.elements.push_back(node_index);
        }

        return node_index;
    }

    JSONInputStream & stream;
    const size_t max_depth;
    JSONNodes nodes;
    ContainerStack stack;
    JSONNodeIndex root = 0;
};

ParsedPath parseJSONPath(const String & path, uint32_t parse_depth, uint32_t parse_backtracks)
{
    Tokens tokens(path.data(), path.data() + path.size());
    IParser::Pos token_iterator(tokens, parse_depth, parse_backtracks);

    Expected expected;
    ASTPtr ast;
    ParserJSONPath parser;
    if (!parser.parse(token_iterator, ast, expected))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unable to parse JSONPath '{}' for function JSONRemove", path);

    const auto * json_path = ast ? ast->as<ASTJSONPath>() : nullptr;
    if (!json_path || !json_path->jsonpath_query)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid JSONPath for function JSONRemove: {}", path);

    const auto & children = json_path->jsonpath_query->children;
    if (children.empty() || !typeid_cast<const ASTJSONPathRoot *>(children.front().get()))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid JSONPath for function JSONRemove: {}", path);

    ParsedPath result;
    result.reserve(children.size() - 1);

    for (size_t i = 1; i < children.size(); ++i)
    {
        const auto * member = typeid_cast<const ASTJSONPathMemberAccess *>(children[i].get());
        if (member)
        {
            result.push_back({PathStep::Type::Member, member->member_name, 0});
            continue;
        }

        const auto * range = typeid_cast<const ASTJSONPathRange *>(children[i].get());
        if (range && !range->is_star && range->ranges.size() == 1)
        {
            const auto [begin, end] = range->ranges.front();
            if (end > begin && end - begin == 1)
            {
                result.push_back({PathStep::Type::Index, {}, begin});
                continue;
            }
        }

        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "JSONPath '{}' must select one object member or one array "
            "element for function JSONRemove",
            path);
    }

    if (result.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "JSONPath '{}' cannot remove the root value", path);

    return result;
}

size_t findMember(const JSONNode & object, const String & name)
{
    for (size_t i = 0; i < object.members.size(); ++i)
    {
        if (object.members[i].name == name)
            return i;
    }
    return object.members.size();
}

bool removeAtPath(JSONNodes & nodes, JSONNodeIndex root, const ParsedPath & path)
{
    JSONNodeIndex parent_index = root;

    for (size_t i = 0; i + 1 < path.size(); ++i)
    {
        const auto & step = path[i];
        const auto & parent = nodes[parent_index];
        if (step.type == PathStep::Type::Member)
        {
            if (parent.type != JSONNode::Type::Object)
                return false;

            const size_t member_index = findMember(parent, step.member_name);
            if (member_index == parent.members.size())
                return false;

            parent_index = parent.members[member_index].value;
        }
        else
        {
            if (parent.type != JSONNode::Type::Array || step.index >= parent.elements.size())
                return false;

            parent_index = parent.elements[step.index];
        }
    }

    auto & parent = nodes[parent_index];
    const auto & target = path.back();
    if (target.type == PathStep::Type::Member)
    {
        if (parent.type != JSONNode::Type::Object)
            return false;

        const size_t member_index = findMember(parent, target.member_name);
        if (member_index == parent.members.size())
            return false;

        parent.members.erase(parent.members.begin() + member_index);
        return true;
    }

    if (parent.type != JSONNode::Type::Array || target.index >= parent.elements.size())
        return false;

    parent.elements.erase(parent.elements.begin() + target.index);
    return true;
}

void appendSlice(String & output, std::string_view json, JSONSlice slice)
{
    output.append(json.data() + slice.offset, slice.length);
}

void serializeJSON(const JSONNodes & nodes, JSONNodeIndex node_index, std::string_view json, String & output)
{
    const auto & node = nodes[node_index];
    if (node.type == JSONNode::Type::Scalar)
    {
        appendSlice(output, json, node.raw_value);
        return;
    }

    if (node.type == JSONNode::Type::Object)
    {
        output.push_back('{');
        for (size_t i = 0; i < node.members.size(); ++i)
        {
            if (i)
                output.push_back(',');

            const auto & member = node.members[i];
            appendSlice(output, json, member.raw_name);
            output.push_back(':');
            serializeJSON(nodes, member.value, json, output);
        }
        output.push_back('}');
        return;
    }

    output.push_back('[');
    for (size_t i = 0; i < node.elements.size(); ++i)
    {
        if (i)
            output.push_back(',');
        serializeJSON(nodes, node.elements[i], json, output);
    }
    output.push_back(']');
}

class FunctionJSONRemove final : public IFunction
{
public:
    static constexpr auto name = "JSONRemove";
    static FunctionPtr create(ContextPtr context) { return std::make_shared<FunctionJSONRemove>(context); }

    explicit FunctionJSONRemove(ContextPtr context)
        : max_parser_depth(context->getSettingsRef()[Setting::max_parser_depth])
        , max_parser_backtracks(context->getSettingsRef()[Setting::max_parser_backtracks])
    {
    }

    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    bool canBeExecutedOnDefaultArguments() const override { return false; }
    bool useDefaultImplementationForConstants() const override { return false; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        const auto string_validator = static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString);
        FunctionArgumentDescriptors mandatory_args{
            {"json", string_validator, nullptr, "String"},
            {"path", string_validator, nullptr, "String"},
        };
        FunctionArgumentDescriptor path_arg{"path", string_validator, nullptr, "String"};
        validateFunctionArgumentsWithVariadics(*this, arguments, mandatory_args, path_arg);
        return std::make_shared<DataTypeString>();
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr &, size_t input_rows_count) const override
    {
        ParsedPaths paths;
        paths.reserve(arguments.size() - 1);

        const auto parse_depth = static_cast<uint32_t>(max_parser_depth);
        const auto parse_backtracks = static_cast<uint32_t>(max_parser_backtracks);
        for (size_t i = 1; i < arguments.size(); ++i)
        {
            if (!isColumnConst(*arguments[i].column))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Path arguments of function {} must be constant Strings", getName());

            const auto * path_column = checkAndGetColumnConstData<ColumnString>(arguments[i].column.get());
            if (!path_column)
                throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Path arguments of function {} must be Strings", getName());

            paths.push_back(parseJSONPath(String{path_column->getDataAt(0)}, parse_depth, parse_backtracks));
        }

        const bool json_is_const = isColumnConst(*arguments[0].column);
        const auto * json_column = json_is_const ? checkAndGetColumnConstData<ColumnString>(arguments[0].column.get())
                                                 : checkAndGetColumn<ColumnString>(arguments[0].column.get());
        if (!json_column)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "First argument of function {} must be a String", getName());

        auto result = ColumnString::create();
        result->reserve(json_is_const ? 1 : input_rows_count);
        if (!input_rows_count)
            return result;

        const size_t rows_to_process = json_is_const ? 1 : input_rows_count;
        for (size_t row = 0; row < rows_to_process; ++row)
        {
            const auto json = json_column->getDataAt(json_is_const ? 0 : row);
            /// RapidJSON uses '\0' as the end-of-stream marker, so accepting an embedded NUL would
            /// silently ignore the suffix after it instead of reporting trailing invalid JSON.
            if (json.contains('\0'))
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Invalid JSON string in function {}: embedded NULL byte", getName());

            JSONInputStream stream(json);
            JSONTreeBuilder builder(stream, max_json_remove_depth);
            TrackedReader reader;
            const auto parse_result = reader.Parse<kParseIterativeFlag | kParseNumbersAsStringsFlag>(stream, builder);
            if (parse_result.IsError())
                throw Exception(
                    ErrorCodes::BAD_ARGUMENTS,
                    "Wrong JSON string passed to function JSONRemove: {}",
                    rapidjson::GetParseError_En(parse_result.Code()));

            const auto root = builder.getRoot();
            auto nodes = builder.releaseNodes();
            for (const auto & path : paths)
                removeAtPath(nodes, root, path);

            String output;
            output.reserve(json.size());
            serializeJSON(nodes, root, json, output);
            result->insertData(output.data(), output.size());
        }

        if (json_is_const)
            return ColumnConst::create(std::move(result), input_rows_count);

        return result;
    }

private:
    const UInt64 max_parser_depth;
    const UInt64 max_parser_backtracks;
};
} // namespace

REGISTER_FUNCTION(JSONRemove)
{
    FunctionDocumentation::Description description = R"(
Removes one or more object members or array elements from a JSON string using JSONPath.
Each path must select exactly one member or element. Paths are applied from left to right. Missing paths do not change the JSON document.
        )";
    FunctionDocumentation::Syntax syntax = "JSONRemove(json, path[, path ...])";
    FunctionDocumentation::Arguments arguments
        = {{"json", "A string containing valid JSON.", {"String"}},
           {"path[, path ...]",
            "One or more constant strings containing JSONPath expressions. Each "
            "path must select one object member or array element.",
            {"String"}}};
    FunctionDocumentation::ReturnedValue returned_value = {"Returns the JSON document as a compact string.", {"String"}};
    FunctionDocumentation::Examples examples
        = {{"Usage example",
            R"(
SELECT JSONRemove('{"a":1,"b":2}', '$.a');
SELECT JSON_REMOVE('[0,1,2]', '$[0]', '$[1]');
            )",
            R"(
{"b":2}
[1]
            )"}};
    FunctionDocumentation::IntroducedIn introduced_in = {26, 10};
    FunctionDocumentation::Category category = FunctionDocumentation::Category::JSON;
    FunctionDocumentation documentation = {description, syntax, arguments, {}, returned_value, examples, introduced_in, category};

    factory.registerFunction<FunctionJSONRemove>(documentation);
    factory.registerAlias("JSON_REMOVE", "JSONRemove", FunctionFactory::Case::Insensitive);
}

} // namespace DB

#endif
