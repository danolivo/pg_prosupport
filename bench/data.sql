--
-- pg_prosupport: benchmark data
--
-- Usage:
--     psql -f bench/schema.sql -f bench/data.sql
--     psql -v rows=500000 -f bench/schema.sql -f bench/data.sql   -- smaller
--
-- The row count is a psql variable so the same script serves a laptop and a
-- machine with memory to spare.  Group counts scale with it: grp_lo always has
-- 1/40th of the rows in groups, grp_hi always has one row per group.
--
-- Every multiplication is done in bigint before the cast, because i * 104729
-- overflows int well before two million rows.
--

\if :{?rows}
\else
\set rows 2000000
\endif

\set lo_groups 50000

INSERT INTO pps_bench
SELECT i % :lo_groups,
	   i,
	   ((i::bigint * 7919)   % 900000)::numeric / 100,
	   ((i::bigint * 7919)   % 900000)::numeric * 100000000000000 / 100,
	   ((i::bigint * 7919)   % 900000)::numeric / 100,
	   ((i::bigint * 104729) % 900000)::numeric / 10000
FROM generate_series(1, :rows) i;

INSERT INTO pps_bench12
SELECT i % :lo_groups,
	   ((i::bigint * 1  % 987654))::numeric / 100,
	   ((i::bigint * 2  % 987654))::numeric / 100,
	   ((i::bigint * 3  % 987654))::numeric / 100,
	   ((i::bigint * 4  % 987654))::numeric / 100,
	   ((i::bigint * 5  % 987654))::numeric / 100,
	   ((i::bigint * 6  % 987654))::numeric / 100,
	   ((i::bigint * 7  % 987654))::numeric / 100,
	   ((i::bigint * 8  % 987654))::numeric / 100,
	   ((i::bigint * 9  % 987654))::numeric / 100,
	   ((i::bigint * 10 % 987654))::numeric / 100,
	   ((i::bigint * 11 % 987654))::numeric / 100,
	   ((i::bigint * 12 % 987654))::numeric / 100,
	   ((i::bigint * 1  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 2  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 3  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 4  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 5  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 6  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 7  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 8  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 9  % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 10 % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 11 % 987654))::numeric * 100000000000000 / 100,
	   ((i::bigint * 12 % 987654))::numeric * 100000000000000 / 100
FROM generate_series(1, :rows) i;

VACUUM (ANALYZE) pps_bench;
VACUUM (ANALYZE) pps_bench12;

SELECT relname,
	   to_char(reltuples, 'FM999G999G999') AS rows,
	   pg_size_pretty(pg_total_relation_size(oid)) AS size
FROM pg_class WHERE relname IN ('pps_bench', 'pps_bench12') ORDER BY 1;
