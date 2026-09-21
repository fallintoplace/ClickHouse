SET join_algorithm = 'partial_merge';

SET join_use_nulls = 1;
SELECT
    l.k,
    isNull(r.value_number),
    isNull(r.value_string),
    length(r.value_array)
FROM
(
    SELECT number AS k
    FROM numbers(4)
) AS l
ALL LEFT JOIN
(
    SELECT
        toUInt64(100) AS k,
        toUInt64(42) AS value_number,
        'x' AS value_string,
        [toUInt64(1), 2] AS value_array,
        CAST('b', 'Enum8(\'a\' = 1, \'b\' = 2)') AS value_enum
) AS r ON l.k = r.k
ORDER BY l.k;

SET join_use_nulls = 0;
SELECT
    l.k,
    r.value_number,
    length(r.value_string),
    length(r.value_array),
    toString(r.value_enum)
FROM
(
    SELECT number AS k
    FROM numbers(4)
) AS l
ALL LEFT JOIN
(
    SELECT
        toUInt64(100) AS k,
        toUInt64(42) AS value_number,
        'x' AS value_string,
        [toUInt64(1), 2] AS value_array,
        CAST('b', 'Enum8(\'a\' = 1, \'b\' = 2)') AS value_enum
) AS r ON l.k = r.k
ORDER BY l.k;
