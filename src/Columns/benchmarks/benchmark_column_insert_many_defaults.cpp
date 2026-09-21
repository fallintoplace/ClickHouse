#include <cstddef>

#include <Columns/IColumn.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/IDataType.h>
#include <base/types.h>

#include <benchmark/benchmark.h>

using namespace DB;

static NO_INLINE void insertManyDefaultsScalar(const IDataType & type, IColumn & column, size_t length)
{
    /// This is the old IDataType::insertManyDefaultsInto implementation.
    column.reserve(column.size() + length);
    for (size_t i = 0; i < length; ++i)
        type.insertDefaultInto(column);
}

static NO_INLINE void insertManyDefaultsBulk(const IDataType & type, IColumn & column, size_t length)
{
    type.insertManyDefaultsInto(column, length);
}

template <const String & type_name, bool scalar>
static void BM_insertManyDefaults(benchmark::State & state)
{
    auto type = DataTypeFactory::instance().get(type_name);
    const size_t length = static_cast<size_t>(state.range(0));
    MutableColumnPtr column;

    for (auto _ [[maybe_unused]] : state)
    {
        state.PauseTiming();
        column = type->createColumn();
        state.ResumeTiming();

        if constexpr (scalar)
            insertManyDefaultsScalar(*type, *column, length);
        else
            insertManyDefaultsBulk(*type, *column, length);

        benchmark::DoNotOptimize(column);

        state.PauseTiming();
        column.reset();
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(length));
}

static const String type_uint64 = "UInt64";
static const String type_string = "String";
static const String type_array_uint64 = "Array(UInt64)";
static const String type_tuple = "Tuple(UInt64, String)";
static const String type_nullable_uint64 = "Nullable(UInt64)";
static const String type_nullable_string = "Nullable(String)";

#define REGISTER_DEFAULT_BENCHMARKS(type) \
    BENCHMARK_TEMPLATE(BM_insertManyDefaults, type, true) \
        ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256)->Arg(4096)->Arg(65536); \
    BENCHMARK_TEMPLATE(BM_insertManyDefaults, type, false) \
        ->Arg(1)->Arg(4)->Arg(16)->Arg(64)->Arg(256)->Arg(4096)->Arg(65536)

REGISTER_DEFAULT_BENCHMARKS(type_uint64);
REGISTER_DEFAULT_BENCHMARKS(type_string);
REGISTER_DEFAULT_BENCHMARKS(type_array_uint64);
REGISTER_DEFAULT_BENCHMARKS(type_tuple);
REGISTER_DEFAULT_BENCHMARKS(type_nullable_uint64);
REGISTER_DEFAULT_BENCHMARKS(type_nullable_string);

#undef REGISTER_DEFAULT_BENCHMARKS
