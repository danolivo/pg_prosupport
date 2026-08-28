CREATE EXTENSION pg_prosupport;

-- The support function is attached by writing pg_proc.prosupport; see the
-- README, and the header of numeric_agg.sql.
UPDATE pg_proc SET prosupport = 'pps_agg_support'::regproc
 WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure,
			   'pg_catalog.avg(numeric)'::regprocedure);

--
-- sum() over a constant argument.  This one does not specialise the
-- aggregation, it removes it:
--
--		sum(c)  ->  c * NULLIF(count(*), 0)::numeric
--
-- The NULLIF is not decoration.  A scalar aggregate over an empty input
-- produces one row holding NULL, whereas count(*) produces 0 and c * 0 is 0 --
-- so without it the rewrite would be wrong in the one case nobody looks at.
--
CREATE TABLE t_const (grp int, v numeric(18,2));
INSERT INTO t_const SELECT i % 3, i * 1.25 FROM generate_series(1, 9) i;
CREATE TABLE t_empty (grp int, v numeric(18,2));

-- The shapes 1C emits: a bare literal, and the padded measure column of a
-- UNION ALL branch that has no value for it.
EXPLAIN (verbose, costs off) SELECT sum('0'::numeric) FROM t_const;
EXPLAIN (verbose, costs off) SELECT sum(0.000::numeric(15,3)) FROM t_const;
-- the constant does not have to be zero
EXPLAIN (verbose, costs off) SELECT sum('1.005'::numeric) FROM t_const;
-- grouped, which is where the wasted transitions actually are
EXPLAIN (verbose, costs off)
	SELECT grp, sum('0'::numeric) FROM t_const GROUP BY grp;
-- FILTER is carried over to the count(*): it counts exactly the rows sum()
-- would have added, and a filter matching nothing leaves count(*) at 0.
EXPLAIN (verbose, costs off)
	SELECT sum('2.5'::numeric) FILTER (WHERE grp = 1) FROM t_const;
-- The same expression twice.  EXPLAIN shows both; ExecInitAgg() matches
-- Aggrefs with equal(), so the two share one count(*) transition, and share it
-- with the count(*) that was already there.
EXPLAIN (verbose, costs off)
	SELECT sum('0'::numeric), sum('0'::numeric), count(*) FROM t_const;
-- In a HAVING, which is the case that started this: a sum of a literal zero
-- compared to zero, twenty-six times in one disjunction in the real query.
EXPLAIN (verbose, costs off)
	SELECT grp FROM t_const GROUP BY grp HAVING sum('0'::numeric) <> 0;

--
-- What is left alone.
--
-- sum(DISTINCT c) is c, not c * count(*); ORDER BY sorts by expressions that
-- live in the Aggref's argument list, which the replacement does not have.
-- Both fall through to the ordinary specialisation, which refuses them too.
EXPLAIN (verbose, costs off) SELECT sum(DISTINCT '1.005'::numeric) FROM t_const;
EXPLAIN (verbose, costs off) SELECT sum('1.005'::numeric ORDER BY v) FROM t_const;
-- avg(c) is not c: avg() divides through numeric_div(), whose result scale
-- comes from select_div_scale() and carries more fractional digits than c has.
-- So it is specialised in the ordinary way instead.
EXPLAIN (verbose, costs off) SELECT avg('1.005'::numeric) FROM t_const;
-- an argument that is not constant is none of this module's business
EXPLAIN (verbose, costs off) SELECT sum(v) FROM t_const;

--
-- The switches.  fold_const_sum takes out this transformation alone and leaves
-- the specialisation in place; enabled takes out both.
--
SET pg_prosupport.fold_const_sum = off;
EXPLAIN (verbose, costs off) SELECT sum('0'::numeric) FROM t_const;
RESET pg_prosupport.fold_const_sum;

SET pg_prosupport.enabled = off;
EXPLAIN (verbose, costs off) SELECT sum('0'::numeric) FROM t_const;
RESET pg_prosupport.enabled;

--
-- The values.  The rewrite has to reproduce core exactly, dscale included, so
-- the reference is taken with the whole extension switched off.
--
SET pg_prosupport.enabled = off;
CREATE TABLE t_constref AS
	SELECT grp,
		   sum('0'::numeric)::text						AS s1,
		   sum(0.000::numeric(15,3))::text				AS s2,
		   sum('1.005'::numeric)::text					AS s3,
		   sum('-2.50'::numeric)::text					AS s4,
		   sum('NaN'::numeric)::text					AS s5,
		   sum('Infinity'::numeric)::text				AS s6,
		   sum(NULL::numeric)::text						AS s7,
		   (sum('2.5'::numeric) FILTER (WHERE v > 5))::text AS s8
	FROM t_const GROUP BY grp;
RESET pg_prosupport.enabled;

SELECT count(*) AS const_mismatches
FROM t_constref r
FULL JOIN (
	SELECT grp,
		   sum('0'::numeric)::text						AS s1,
		   sum(0.000::numeric(15,3))::text				AS s2,
		   sum('1.005'::numeric)::text					AS s3,
		   sum('-2.50'::numeric)::text					AS s4,
		   sum('NaN'::numeric)::text					AS s5,
		   sum('Infinity'::numeric)::text				AS s6,
		   sum(NULL::numeric)::text						AS s7,
		   (sum('2.5'::numeric) FILTER (WHERE v > 5))::text AS s8
	FROM t_const GROUP BY grp) x USING (grp)
WHERE (r.s1, r.s2, r.s3, r.s4, r.s5, r.s6, r.s7, r.s8)
	  IS DISTINCT FROM (x.s1, x.s2, x.s3, x.s4, x.s5, x.s6, x.s7, x.s8);

-- The dscale of the result is the dscale of the constant: numeric
-- multiplication yields s1 + s2 and the int8 conversion yields 0.
SELECT sum(0.000::numeric(15,3)) AS trailing_zeroes FROM t_const;
SELECT sum('1.005'::numeric) AS scalar_sum FROM t_const;
SELECT sum('-2.50'::numeric) AS negative_sum FROM t_const;

-- The empty input, which is what the NULLIF is there for.
SELECT sum('0'::numeric) IS NULL AS empty_is_null FROM t_empty;
SELECT sum('0'::numeric) IS NULL AS empty_group_is_null
	FROM t_empty GROUP BY GROUPING SETS ((), (grp));
-- a FILTER that matches nothing is the same situation
SELECT sum('0'::numeric) FILTER (WHERE false) IS NULL AS empty_filter_is_null
	FROM t_const;

-- The special values go through numeric_mul() rather than through repeated
-- addition, and have to come out the same all the same.
SELECT sum('NaN'::numeric) AS nan_sum FROM t_const;
SELECT sum('Infinity'::numeric) AS inf_sum FROM t_const;
SELECT sum('-Infinity'::numeric) AS neginf_sum FROM t_const;
SELECT sum(NULL::numeric) IS NULL AS null_const_is_null FROM t_const;

-- The two shapes that are refused still have to compute the right answer.
SELECT sum(DISTINCT '1.005'::numeric) AS distinct_sum FROM t_const;
SELECT sum('1.005'::numeric ORDER BY v) AS ordered_sum FROM t_const;

-- Partial aggregation: the count(*) is combined across workers and the
-- multiplication happens once, above the Gather.
SET max_parallel_workers_per_gather = 0;
SELECT sum('1.005'::numeric)::text AS constseq FROM t_const \gset
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT sum('1.005'::numeric)::text = :'constseq' AS const_parallel_matches_serial
	FROM t_const;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

DROP TABLE t_const, t_constref, t_empty;
-- Detach before dropping.  Nothing records the dependency, and a prosupport
-- pointing at a function that no longer exists breaks every sum(numeric) in
-- the database.
UPDATE pg_proc SET prosupport = 0
 WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure,
			   'pg_catalog.avg(numeric)'::regprocedure);
DROP EXTENSION pg_prosupport;
