--
-- pg_prosupport: the queries the README's numbers come from
--
-- Usage:
--     psql -f bench/queries.sql            -- substituted (the default)
--     psql -v enabled=off -f bench/queries.sql   -- stock aggregates
--
-- Run it both ways and subtract.  Each query is reported by EXPLAIN with
-- ANALYZE and TIMING OFF, so the numbers are execution time and, for the
-- grouped ones, the hash table's memory and whether it spilled.
--
-- Take a median of several runs rather than a single number: the stock
-- aggregate's spread between neighbouring runs reached 20% on the machine
-- these were measured on, which is wide enough to invent a result that is not
-- there.
--
-- The extension has to be installed and prosupport attached; see the README.
--

\if :{?enabled}
\else
\set enabled on
\endif

\set ECHO queries

SET pg_prosupport.enabled = :enabled;
SET max_parallel_workers_per_gather = 0;
SET work_mem = '1GB';

\set ea 'EXPLAIN (ANALYZE, TIMING OFF, COSTS OFF, BUFFERS OFF)'

--
-- The scan on its own.  Everything below has to be read net of this: on a
-- table this shape the scan is a third to a half of the total, and quoting
-- the totals alone overstates how much of the query the aggregate is.
--
:ea SELECT count(*) FROM pps_bench;

--
-- One aggregate over a plain column.  This is where the specialisation has
-- the most room: the argument costs nothing to compute, so the accumulation
-- is the whole of the non-scan work.
--
:ea SELECT sum(v) FROM pps_bench;
:ea SELECT avg(v) FROM pps_bench;
:ea SELECT sum(w) FROM pps_bench;

--
-- The same over an arithmetic argument.  The per-row numeric_mul() is
-- untouched by the substitution and dominates what is left, so the gain here
-- is much smaller.  This is the honest number for a reporting query.
--
:ea SELECT sum(a * b) FROM pps_bench;
:ea SELECT avg(a * b) FROM pps_bench;

--
-- Grouped, low cardinality.  The hash table fits, so this measures the
-- arithmetic and nothing else.
--
:ea SELECT grp_lo, sum(v) FROM pps_bench GROUP BY grp_lo;
:ea SELECT grp_lo, sum(a * b) FROM pps_bench GROUP BY grp_lo;

--
-- Grouped, one row per group.  Now the state size decides the memory, and the
-- final function runs as many times as there are rows.
--
:ea SELECT grp_hi, sum(v) FROM pps_bench GROUP BY grp_hi;

--
-- A dozen aggregates, the shape of an actual report.  The saving multiplies by
-- the number of aggregates, because each one has its own state per group.
--
:ea SELECT grp,
	   sum(n01), sum(n02), sum(n03), sum(n04), sum(n05), sum(n06),
	   sum(n07), sum(n08), sum(n09), sum(n10), sum(n11), sum(n12)
	FROM pps_bench12 GROUP BY grp;

--
-- The same with numeric(25,2), where every mantissa needs the full int128.
--
:ea SELECT grp,
	   sum(w01), sum(w02), sum(w03), sum(w04), sum(w05), sum(w06),
	   sum(w07), sum(w08), sum(w09), sum(w10), sum(w11), sum(w12)
	FROM pps_bench12 GROUP BY grp;

--
-- Spill.  hash_mem_multiplier is 2 by default, so the effective limit is twice
-- work_mem; at 64MB that is 128MB, which the stock states exceed and ours do
-- not.  Look at "Batches" and "Disk Usage" rather than at the time.
--
SET work_mem = '64MB';
:ea SELECT grp,
	   sum(n01), sum(n02), sum(n03), sum(n04), sum(n05), sum(n06),
	   sum(n07), sum(n08), sum(n09), sum(n10), sum(n11), sum(n12)
	FROM pps_bench12 GROUP BY grp;
RESET work_mem;

--
-- Parallel.  Every partial state crosses the worker-to-leader boundary once
-- per group, so this is what exercises serialize and deserialize.
--
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;

:ea SELECT grp,
	   sum(n01), sum(n02), sum(n03), sum(n04), sum(n05), sum(n06),
	   sum(n07), sum(n08), sum(n09), sum(n10), sum(n11), sum(n12)
	FROM pps_bench12 GROUP BY grp;

RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET pg_prosupport.enabled;

--
-- Planning cost, measured separately: twenty-five aggregates, no execution.
-- Compare with :enabled off to see what the support function costs when it
-- fires and when it declines.
--
EXPLAIN (SUMMARY ON, COSTS OFF)
SELECT sum(n01), sum(n02), sum(n03), sum(n04), sum(n05), sum(n06),
	   sum(n07), sum(n08), sum(n09), sum(n10), sum(n11), sum(n12),
	   sum(w01), sum(w02), sum(w03), sum(w04), sum(w05), sum(w06),
	   sum(w07), sum(w08), sum(w09), sum(w10), sum(w11), sum(w12),
	   count(*)
FROM pps_bench12;
