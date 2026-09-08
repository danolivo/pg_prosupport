-- pg_prosupport extension
--
-- Copyright (c) 2026, Andrei Lepikhov
-- Released under the MIT licence; see the LICENSE file in this directory.

\echo Use "CREATE EXTENSION pg_prosupport" to load this file. \quit

CREATE FUNCTION pps_scaled_accum(internal, numeric, int4) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_scaled_accum' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_scaled_accum_mul(internal, numeric, numeric, int4, int4)
  RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_scaled_accum_mul' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_scaled_combine(internal, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_scaled_combine' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_scaled_serialize(internal) RETURNS bytea
  AS 'MODULE_PATHNAME', 'pps_scaled_serialize' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION pps_scaled_deserialize(bytea, internal) RETURNS internal
  AS 'MODULE_PATHNAME', 'pps_scaled_deserialize' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION pps_scaled_sum_final(internal) RETURNS numeric
  AS 'MODULE_PATHNAME', 'pps_scaled_sum_final' LANGUAGE C IMMUTABLE;
CREATE FUNCTION pps_scaled_avg_final(internal) RETURNS numeric
  AS 'MODULE_PATHNAME', 'pps_scaled_avg_final' LANGUAGE C IMMUTABLE;

-- No support function to create and attach here: the substitution is done by
-- agg_simplify_hook, a global planner hook that _PG_init() installs when the
-- library is loaded (shared_preload_libraries, session_preload_libraries, or
-- LOAD) -- see the README and pg_prosupport.c.

-- Two aggregates covering 1 <= p <= 28, differing only in the final function,
-- exactly as core shares numeric_avg_accum between sum() and avg().  They are
-- only ever put into a plan by the support function; there is no reason to
-- call them by hand.
--
-- sspace = 64 is sizeof(NasAggState) plus an aset chunk header, and the C code
-- carries a StaticAssertDecl that fires if the struct changes size.  The
-- planner takes the size of a hash table entry from here
-- (hash_agg_entry_size()).  The real saving is larger than the declared one:
-- the stock sum(numeric) keeps a NumericAggState holding NumericVars whose
-- digit arrays are allocated separately, so it also pays for those chunks and
-- for the cache misses of chasing the pointers -- 1.17 GB against 467 MB over
-- three million groups.
CREATE AGGREGATE numeric_scaled_sum(numeric, int4) (
  sfunc        = pps_scaled_accum,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_scaled_combine,
  serialfunc   = pps_scaled_serialize,
  deserialfunc = pps_scaled_deserialize,
  finalfunc    = pps_scaled_sum_final,
  parallel     = safe
);

CREATE AGGREGATE numeric_scaled_avg(numeric, int4) (
  sfunc        = pps_scaled_accum,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_scaled_combine,
  serialfunc   = pps_scaled_serialize,
  deserialfunc = pps_scaled_deserialize,
  finalfunc    = pps_scaled_avg_final,
  parallel     = safe
);

-- The same two with the multiplication folded in, for sum(a * b) and
-- avg(a * b).  The product is never built as a numeric: both mantissas are
-- taken from the packed operands and int128_add_int64_mul_int64() accumulates
-- the product directly.  Everything but the transition function is shared with
-- the aggregates above, state included.
CREATE AGGREGATE numeric_scaled_sum_mul(numeric, numeric, int4, int4) (
  sfunc        = pps_scaled_accum_mul,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_scaled_combine,
  serialfunc   = pps_scaled_serialize,
  deserialfunc = pps_scaled_deserialize,
  finalfunc    = pps_scaled_sum_final,
  parallel     = safe
);

CREATE AGGREGATE numeric_scaled_avg_mul(numeric, numeric, int4, int4) (
  sfunc        = pps_scaled_accum_mul,
  stype        = internal,
  sspace       = 64,
  combinefunc  = pps_scaled_combine,
  serialfunc   = pps_scaled_serialize,
  deserialfunc = pps_scaled_deserialize,
  finalfunc    = pps_scaled_avg_final,
  parallel     = safe
);
