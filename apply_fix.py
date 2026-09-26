from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

SETTING = 'optimize_string_size_subcolumn_with_full_read'


def replace_once(path, old, new):
    file = Path(path)
    text = file.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected one replacement, found {count}: {old[:100]!r}')
    file.write_text(text.replace(old, new, 1))


analyzer = 'src/Analyzer/Passes/FunctionToSubcolumnsPass.cpp'
replace_once(analyzer, '    extern const SettingsBool optimize_functions_to_subcolumns;\n', '    extern const SettingsBool optimize_functions_to_subcolumns;\n' + f'    extern const SettingsBool {SETTING};\n')
replace_once(analyzer, '/// read for matching rows in SELECT. For legacy String parts where .size is virtual,\n', '/// read for matching rows in SELECT. String rewrites additionally require\n' + f'/// `{SETTING}` because splitting reads may add work\n' + '/// when the size filter does not reject whole granules. For legacy String parts where .size is virtual,\n')
replace_once(analyzer, '            if (transformers_optimize_in_filter_with_full_column.contains(transformer_key)\n                && !in_where_prewhere_stack.empty() && in_where_prewhere_stack.back())\n', '            if (transformers_optimize_in_filter_with_full_column.contains(transformer_key)\n                && !in_where_prewhere_stack.empty() && in_where_prewhere_stack.back()\n                && (transformer_key.first != TypeIndex::String\n' + f'                    || getSettings()[Setting::{SETTING}]))\n')
replace_once(analyzer, '                && identifiers_to_optimize.filter_only.contains(qualified_name)\n                && !in_where_prewhere_stack.empty()\n                && in_where_prewhere_stack.back())\n', '                && identifiers_to_optimize.filter_only.contains(qualified_name)\n                && !in_where_prewhere_stack.empty()\n                && in_where_prewhere_stack.back()\n                && (column.type->getTypeId() != TypeIndex::String\n' + f'                    || getSettings()[Setting::{SETTING}]))\n')

settings = 'src/Core/Settings.cpp'
for name in ('length', 'empty', 'notEmpty'):
    old = f'- [{name}](/reference/functions/regular-functions/array-functions#{name}) to read the [size0](/reference/data-types/array#array-size) subcolumn.'
    new = f'- [{name}](/reference/functions/regular-functions/array-functions#{name}) to read the [size0](/reference/data-types/array#array-size) subcolumn for arrays or the `size` subcolumn for `String` columns.'
    replace_once(settings, old, new)
old = '- [mapContainsValueLike](/reference/functions/regular-functions/tuple-map-functions#mapContainsValueLike) to read the [values](/reference/data-types/map#reading-subcolumns-of-map) subcolumn.\n\nPossible values:'
new = '- [mapContainsValueLike](/reference/functions/regular-functions/tuple-map-functions#mapContainsValueLike) to read the [values](/reference/data-types/map#reading-subcolumns-of-map) subcolumn.\n\n' + f'String filters in `WHERE` and `PREWHERE` can also use the `size` subcolumn when the full String is needed elsewhere, if [`{SETTING}`](#{SETTING}) is enabled.\n' + "On `MergeTree` parts written with `string_serialization_version = 'with_size_stream'`, this can avoid reading String payloads for rejected granules.\n" + 'On legacy `single_stream` parts, `size` is virtual and requires the regular String stream. When both the String and its size are needed, the reader reads them together to avoid scanning that stream twice.\n\nPossible values:'
replace_once(settings, old, new)
new_setting = f'''    DECLARE(Bool, {SETTING}, false, R"(
Allows `length`, `empty`, and `notEmpty` filters on `String` columns in `WHERE` and `PREWHERE` to read the `size` subcolumn when the query also needs the full String.
Requires [`optimize_functions_to_subcolumns`](#optimize_functions_to_subcolumns) to be enabled.

On `MergeTree` parts written with `string_serialization_version = 'with_size_stream'`, filtering on sizes can avoid reading String payloads for rejected granules.
The optimization is disabled by default because reading sizes separately can add overhead when the filter does not reject whole granules.
Enable it for workloads where measurements show a benefit, such as clustered or sufficiently rare String-length outliers.

On legacy `single_stream` parts, `size` is virtual and still requires the regular String stream. The reader reads the String and its size together to avoid scanning that stream twice.
This setting does not disable size-subcolumn rewrites when the full String is not needed, and does not affect explicit subcolumn access.

Possible values:

- 0 - Disabled (default).
- 1 - Enabled.
)", 0) \\
'''
replace_once(settings, '    DECLARE(Bool, optimize_using_constraints, false, R"(\n', new_setting + '    DECLARE(Bool, optimize_using_constraints, false, R"(\n')
replace_once('src/Core/SettingsChangesHistory.cpp', '        addSettingsChanges(settings_changes_history, "26.10",\n        {\n', '        addSettingsChanges(settings_changes_history, "26.10",\n        {\n' + f'            {{"{SETTING}", false, false, "New setting to opt in to String size-subcolumn filters when the query also reads the full String."}},\n')

length_file = 'src/Functions/array/length.cpp'
replace_once(length_file, 'This also applies in `WHERE` and `PREWHERE` when the query needs the full String column elsewhere, for example `SELECT s FROM t PREWHERE length(s) > 0`.\n', f'When [`{SETTING} = 1`](/reference/settings/session-settings/optimize#{SETTING}), this also applies in `WHERE` and `PREWHERE` when the query needs the full String column elsewhere.\n' + 'For example, `SELECT s FROM t PREWHERE length(s) > 1000` can benefit when the filter rejects whole granules. This extra optimization is disabled by default.\n')
replace_once(length_file, "On MergeTree parts written with `string_serialization_version = 'with_size_stream'`, PREWHERE can filter on the size stream before reading String data for surviving rows.\n", "On MergeTree parts written with `string_serialization_version = 'with_size_stream'`, PREWHERE can filter on the size stream before reading String data for surviving granules.\n")
empty_file = Path('src/Functions/empty.cpp')
text = empty_file.read_text()
old = ', including in `WHERE` and `PREWHERE` when the query also needs the full `s`.\n'
assert text.count(old) == 4
text = text.replace(old, '.\n' + f'When the query also needs the full `s`, rewrites in `WHERE` and `PREWHERE` additionally require [`{SETTING} = 1`](/reference/settings/session-settings/optimize#{SETTING}).\n' + 'This extra optimization is disabled by default because it may add overhead when the filter does not reject whole granules.\n')
text = text.replace('PREWHERE can filter on sizes before reading String data for surviving rows.', 'PREWHERE can filter on sizes before reading String data for surviving granules.')
empty_file.write_text(text)
for path in ('src/Functions/empty.cpp', 'src/Functions/array/length.cpp'):
    p = Path(path)
    p.write_text(p.read_text().replace('On MergeTree parts written with', 'On `MergeTree` parts written with').replace('`, PREWHERE can filter', '`, `PREWHERE` can filter'))
for path in ('tests/queries/0_stateless/00091_prewhere_two_conditions.sql', 'tests/queries/0_stateless/04032_and_comparison_filter_optimization_2.sql'):
    replace_once(path, 'SET optimize_functions_to_subcolumns = 0;\n', '')

sql_path = 'tests/queries/0_stateless/05234_functions_to_subcolumns_string_filter_only.sql'
reference_path = 'tests/queries/0_stateless/05234_functions_to_subcolumns_string_filter_only.reference'
replace_once(sql_path, 'SET optimize_empty_string_comparisons = 1;\n', 'SET optimize_empty_string_comparisons = 1;\nSET optimize_functions_to_subcolumns = 1;\n' + f'SET {SETTING} = 0;\n')
checks = ["SELECT 'full String filter rewrite is opt-in';", f"SELECT `default` = '0' FROM system.settings WHERE name = '{SETTING}';"]
for predicate in ('length(s) > 0', 'empty(s)', 'notEmpty(s)', "s = ''", "s != ''"):
    checks.append(f'''SELECT countIf(explain ILIKE '%s.size%') = 0
FROM (EXPLAIN actions = 1, compact = 0, pretty = 0
    SELECT s
    FROM test_string_filter_only
    PREWHERE {predicate}
    SETTINGS optimize_functions_to_subcolumns = 1, optimize_move_to_prewhere = 0);''')
checks.extend(["SELECT 'String size-only rewrites remain enabled';", '''SELECT countIf(explain ILIKE '%s.size%') > 0
FROM (EXPLAIN actions = 1, compact = 0, pretty = 0
    SELECT length(s), empty(s), notEmpty(s)
    FROM test_string_filter_only
    SETTINGS optimize_functions_to_subcolumns = 1);''', '''SELECT countIf(explain ILIKE '%s.size%') > 0
FROM (EXPLAIN actions = 1, compact = 0, pretty = 0
    SELECT id
    FROM test_string_filter_only
    PREWHERE notEmpty(s)
    SETTINGS optimize_functions_to_subcolumns = 1, optimize_move_to_prewhere = 0);''', f'SET {SETTING} = 1;', ''])
replace_once(sql_path, "SELECT 'length uses String size with full column';\n", '\n'.join(checks) + "\nSELECT 'length uses String size with full column';\n")
reference = Path(reference_path)
reference.write_text('full String filter rewrite is opt-in\n' + '1\n' * 6 + 'String size-only rewrites remain enabled\n1\n1\n' + reference.read_text())
sql = Path(sql_path)
sqltext = sql.read_text()
reftext = reference.read_text()
for enabled in (0, 1):
    text = f"SELECT 'grouped CTE String filter with opt-in {enabled}';\n"
    text += f'''SELECT countIf(explain ILIKE '%s.size%') {'> 0' if enabled else '= 0'}
FROM (EXPLAIN actions = 1, compact = 0, pretty = 0
    WITH cte AS (SELECT s, id FROM test_string_filter_only WHERE s != '')
    SELECT s, count() FROM cte GROUP BY s
    SETTINGS optimize_move_to_prewhere = 1);
WITH cte AS (SELECT s, id FROM test_string_filter_only WHERE s != '')
SELECT s, count() FROM cte GROUP BY s ORDER BY s
SETTINGS optimize_move_to_prewhere = 1;

'''
    marker = "SELECT 'String size-only rewrites remain enabled';\n" if not enabled else "SELECT 'length uses String size with full column';\n"
    assert sqltext.count(marker) == 1
    sqltext = sqltext.replace(marker, text + marker)
    refmarker = 'String size-only rewrites remain enabled\n' if not enabled else 'length uses String size with full column\n'
    assert reftext.count(refmarker) == 1
    reftext = reftext.replace(refmarker, f'grouped CTE String filter with opt-in {enabled}\n1\nhello\t1\nworld\t1\n' + refmarker)
sql.write_text(sqltext)
reference.write_text(reftext)

perf_path = Path('tests/performance/string_size_filter_only.xml')
text = perf_path.read_text()
assert text.count('optimize_functions_to_subcolumns = 1,') == 4
assert text.count('optimize_functions_to_subcolumns = 0,') == 4
text = text.replace('optimize_functions_to_subcolumns = 1,', 'optimize_functions_to_subcolumns = 1,\n' + f'            {SETTING} = 1,')
text = text.replace('optimize_functions_to_subcolumns = 0,', 'optimize_functions_to_subcolumns = 1,\n' + f'            {SETTING} = 0,')
text = text.replace('when the subcolumn optimization is disabled.', 'when the full-String size optimization is disabled.')
text = text.replace('without the subcolumn rewrite.', 'without the full-String size rewrite.')
negative_pair = f'''
    <!-- Most granules survive: enabling split reads must not be assumed to help this workload. -->
    <query tag='String size nonselective filter-only optimization'>
        SELECT s
        FROM string_size_filter_only_perf
        PREWHERE notEmpty(s)
        FORMAT Null
        SETTINGS
            max_threads = 1,
            optimize_functions_to_subcolumns = 1,
            {SETTING} = 1,
            optimize_move_to_prewhere = 0
    </query>

    <query tag='String size nonselective filter-only control'>
        SELECT s
        FROM string_size_filter_only_perf
        PREWHERE notEmpty(s)
        FORMAT Null
        SETTINGS
            max_threads = 1,
            optimize_functions_to_subcolumns = 1,
            {SETTING} = 0,
            optimize_move_to_prewhere = 0
    </query>

'''
marker = '    <drop_query>DROP TABLE IF EXISTS string_size_filter_only_perf</drop_query>'
assert text.count(marker) == 1
text = text.replace(marker, negative_pair + marker)
text = text.replace('    <create_query>\n', '''    <!-- The reference build for this PR does not have the new setting yet. -->
    <create_query do_not_check_in_pr="121377">
        SELECT getSetting('optimize_string_size_subcolumn_with_full_read')
    </create_query>

    <create_query>
''', 1)
perf_path.write_text(text)
root = ET.parse(perf_path).getroot()
queries = root.findall('query')
assert len(queries) == 10
for query in queries:
    assert 'optimize_functions_to_subcolumns = 1' in (query.text or '')
    assert SETTING in (query.text or '')
assert sum(f'{SETTING} = 1' in (q.text or '') for q in queries) == 5
assert sum(f'{SETTING} = 0' in (q.text or '') for q in queries) == 5
assert Path(analyzer).read_text().count(f'getSettings()[Setting::{SETTING}]') == 2
assert Path(settings).read_text().count(f'DECLARE(Bool, {SETTING}, false,') == 1
subprocess.run(['git', 'diff', '--check'], check=True)
print('Validated the default-off declaration, both analyzer gates, and five isolated benchmark pairs.')
