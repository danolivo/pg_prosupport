-- pg_prosupport extension
--
-- Copyright (c) 2026, Andrei Lepikhov
-- Released under the MIT licence; see the LICENSE file in this directory.

\echo Use "CREATE EXTENSION pg_prosupport" to load this file. \quit

CREATE FUNCTION pps_bounded_accum(internal, numeric, int4) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_bounded_accum' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_bounded_accum_mul(internal, numeric, numeric, int4, int4)
  RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_bounded_accum_mul' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_bounded_combine(internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_bounded_combine' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_bounded_serialize(internal) RETURNS bytea
  AS 'MODULE_PATHNAME', 'pps_bounded_serialize' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION pps_bounded_deserialize(bytea, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_bounded_deserialize' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION pps_bounded_sum_final(internal) RETURNS numeric
  AS 'MODULE_PATHNAME', 'pps_bounded_sum_final' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_bounded_avg_final(internal) RETURNS numeric
  AS 'MODULE_PATHNAME', 'pps_bounded_avg_final' LANGUAGE C IMMUTABLE;

-- The planner support function of pg_catalog.sum(numeric) and
-- pg_catalog.avg(numeric) on PostgreSQL 19 and later, where constant folding
-- calls an aggregate's prosupport entry with a SupportRequestSimplifyAggref.
CREATE FUNCTION pps_agg_support(internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_agg_support' LANGUAGE C STRICT;

-- Attaching the prosupport function to built-in aggregates is a write to
-- pg_proc, which ALTER FUNCTION ... SUPPORT will not do for an aggregate, so
-- these two do it (dependency included, so that DROP EXTENSION cannot leave
-- sum(numeric) pointing at an OID that no longer exists).  Superuser only.
CREATE FUNCTION pps_attach_support() RETURNS void
  AS 'MODULE_PATHNAME', 'pps_attach_support' LANGUAGE C;
CREATE FUNCTION pps_detach_support() RETURNS void
  AS 'MODULE_PATHNAME', 'pps_detach_support' LANGUAGE C;

-- Two aggregates covering 1 <= p <= 28, differing only in the final function,
-- exactly as core shares numeric_avg_accum between sum() and avg().  They are
-- only ever put into a plan by agg_simplify_hook; there is no reason to call
-- them by hand.
--
-- bounded_numeric_sum below serves both shapes pps_simplify_bounded_numeric_agg()
-- (numeric_support.c) recognises: the plain-column case -- sum(c) where c is
-- itself numeric(p,s) or a cast, CASE or domain declaring one, read straight
-- off the typmod -- and an *arithmetic expression* over numeric columns --
-- sum(a + b), sum(a - b), sum(round(a * b, 2)) -- via pps_derive_bounds()'s
-- recursion through OpExpr and FuncExpr. Both go through the same hook, the
-- same bounds derivation, and the same aggregate; there is no separate
-- core-level path for either one, on PG18 or otherwise (see the top-level
-- README).
--
-- sspace = 64 is sizeof(NasAggState) plus an aset chunk header, and the C code
-- carries a StaticAssertDecl that fires if the struct changes size.  The
-- planner takes the size of a hash table entry from here
-- (hash_agg_entry_size()).  The real saving is larger than the declared one:
-- the stock sum(numeric) keeps a NumericAggState holding NumericVars whose
-- digit arrays are allocated separately, so it also pays for those chunks and
-- for the cache misses of chasing the pointers -- 1.17 GB against 467 MB over
-- three million groups.
CREATE AGGREGATE bounded_numeric_sum(numeric, int4) (
  sfunc        = pps_bounded_accum,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_bounded_combine,
  serialfunc   = pps_bounded_serialize,
  deserialfunc = pps_bounded_deserialize,
  finalfunc    = pps_bounded_sum_final,
  parallel     = safe
);

CREATE AGGREGATE bounded_numeric_avg(numeric, int4) (
  sfunc        = pps_bounded_accum,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_bounded_combine,
  serialfunc   = pps_bounded_serialize,
  deserialfunc = pps_bounded_deserialize,
  finalfunc    = pps_bounded_avg_final,
  parallel     = safe
);

-- The same two with the multiplication folded in, for sum(a * b) and
-- avg(a * b).  The product is never built as a numeric: both mantissas are
-- taken from the packed operands and int128_add_int64_mul_int64() accumulates
-- the product directly.  Everything but the transition function is shared with
-- the aggregates above, state included.
CREATE AGGREGATE bounded_numeric_sum_mul(numeric, numeric, int4, int4) (
  sfunc        = pps_bounded_accum_mul,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_bounded_combine,
  serialfunc   = pps_bounded_serialize,
  deserialfunc = pps_bounded_deserialize,
  finalfunc    = pps_bounded_sum_final,
  parallel     = safe
);

CREATE AGGREGATE bounded_numeric_avg_mul(numeric, numeric, int4, int4) (
  sfunc        = pps_bounded_accum_mul,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_bounded_combine,
  serialfunc   = pps_bounded_serialize,
  deserialfunc = pps_bounded_deserialize,
  finalfunc    = pps_bounded_avg_final,
  parallel     = safe
);

-- Last, once every aggregate the rewrites can substitute exists: hand
-- sum(numeric) and avg(numeric) over to pps_agg_support().  Only on 19 and
-- later, where the planner reads that entry; on 18 pps_attach_support()
-- refuses to run, and the hook from patches/ does the same job instead.
DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 190000 THEN
    PERFORM pps_attach_support();
  END IF;
END
$$;
