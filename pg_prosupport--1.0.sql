-- pg_prosupport extension
--
-- Copyright (c) 2026, Andrei Lepikhov
-- Released under the MIT licence; see the LICENSE file in this directory.

\echo Use "CREATE EXTENSION pg_prosupport" to load this file. \quit

CREATE FUNCTION pps_bounded_accum(internal, numeric, int4)
RETURNS internal
AS 'MODULE_PATHNAME', 'pps_bounded_accum'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pps_bounded_accum_mul(internal, numeric, numeric, int4, int4)
RETURNS internal
AS 'MODULE_PATHNAME', 'pps_bounded_accum_mul'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pps_bounded_combine(internal, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'pps_bounded_combine'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pps_bounded_serialize(internal)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pps_bounded_serialize'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION pps_bounded_deserialize(bytea, internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'pps_bounded_deserialize'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION pps_bounded_sum_final(internal)
RETURNS numeric
AS 'MODULE_PATHNAME', 'pps_bounded_sum_final'
LANGUAGE C IMMUTABLE;

CREATE FUNCTION pps_bounded_avg_final(internal)
RETURNS numeric
AS 'MODULE_PATHNAME', 'pps_bounded_avg_final'
LANGUAGE C IMMUTABLE;

-- The planner support function of pg_catalog.sum(numeric) and
-- pg_catalog.avg(numeric).
CREATE FUNCTION pps_agg_support(internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'pps_agg_support'
LANGUAGE C STRICT;

-- Attaching the prosupport function to built-in aggregates is a write to
-- pg_proc, which ALTER FUNCTION ... SUPPORT refuses to do for an aggregate
-- ("%s is an aggregate function", AlterFunction() in functioncmds.c) -- and an
-- aggregate is exactly what we have to attach to: it is sum(numeric)'s own
-- prosupport entry that simplify_aggref() consults, nobody else's.  So the
-- catalog row is written here by hand.
--
-- The pg_depend record is the point of doing this rather than a bare UPDATE.
-- Without it, DROP EXTENSION would leave sum(numeric) pointing at an OID that
-- no longer exists, and every query using sum(numeric) in that database would
-- fail in the planner with "cache lookup failed for function NNNN" -- a broken
-- database, not a lost optimisation.  With it, DROP EXTENSION fails cleanly
-- instead, and so does DROP EXTENSION ... CASCADE, because sum(numeric) is
-- pinned and refuses to be dropped along with us.  pps_detach_support() is
-- then the one way out, which is the intended one.
--
-- Both directions share one body, because the invariant they keep is a single
-- one: pg_proc.prosupport and the pg_depend row move together or not at all.
-- The whole thing is plpgsql rather than C for the same reason it is short:
-- there is nothing here that SQL cannot say.  ResetPlanCache() is DISCARD
-- PLANS, recordDependencyOn() is an INSERT that is a no-op when the row is
-- already there, and the one thing C had over SQL -- resolving pps_agg_support
-- through the caller's own namespace, so that ALTER EXTENSION ... SET SCHEMA
-- could not strand it -- is bought instead by relocatable = false in the
-- control file.
--
-- @extschema@ below is therefore constant for the life of the installation.
-- Do not replace it with an unqualified name: the body runs under the caller's
-- search_path, and a same-named function elsewhere would be a silent hijack of
-- a catalog write.
CREATE FUNCTION pps_set_support(attach boolean)
RETURNS void
LANGUAGE plpgsql
SET search_path = pg_catalog, pg_temp
AS $body$
DECLARE
  ourfn   oid := '@extschema@.pps_agg_support(internal)'::regprocedure;
  aggfn   oid;
  oldsup  oid;
  newsup  oid;
BEGIN
  -- Not the security boundary -- that is the REVOKE at the foot of this file,
  -- plus pg_proc's own ACL, which a SECURITY INVOKER function cannot get past
  -- whatever anybody GRANTs on this one.  What the check buys is an error that
  -- says what is actually required, instead of "permission denied for table
  -- pg_proc" leaking the implementation, in the one case that reaches here: a
  -- superuser having GRANTed EXECUTE to somebody.  It also keeps the contract
  -- true if this ever becomes SECURITY DEFINER.  Before the version check, so
  -- that the answer to "may I?" does not depend on the branch.
  PERFORM 1 FROM pg_roles WHERE rolname = CURRENT_USER AND rolsuper;
  IF NOT FOUND THEN
    RAISE EXCEPTION
      'must be superuser to change the support function of a built-in aggregate'
      USING ERRCODE = 'insufficient_privilege';
  END IF;

  -- On 18 the planner never reads an aggregate's prosupport entry; the
  -- rewrites are reached through agg_simplify_hook instead, so writing the
  -- entry would be a catalog change that buys nothing.
  IF current_setting('server_version_num')::int < 190000 THEN
    RAISE EXCEPTION 'aggregate support functions require PostgreSQL 19 or later'
      USING ERRCODE = 'feature_not_supported',
            DETAIL  = 'On PostgreSQL 18 the rewrites are reached through '
                      'agg_simplify_hook instead (needs patch); load the library with '
                      'shared_preload_libraries, session_preload_libraries or LOAD.';
  END IF;

  FOREACH aggfn IN ARRAY ARRAY['pg_catalog.sum(numeric)'::regprocedure::oid,
                               'pg_catalog.avg(numeric)'::regprocedure::oid]
  LOOP
    SELECT p.prosupport INTO oldsup FROM pg_proc p WHERE p.oid = aggfn;

    IF attach THEN
      newsup := ourfn;
    ELSE
      -- On detach, leave alone anything that is not ours.  A future release
      -- could give sum(numeric) a support function of its own, and clearing
      -- it here would disable a core optimisation with nothing to show for it.
      CONTINUE WHEN oldsup <> ourfn;
      newsup := 0;
    END IF;

    CONTINUE WHEN oldsup = newsup;		-- already in the state asked for

    -- Refuse to take over an entry somebody else owns, for the same reason.
    IF oldsup <> 0 AND newsup <> 0 THEN
      RAISE EXCEPTION '% already has the support function %',
                      aggfn::regprocedure, oldsup::regprocedure
        USING ERRCODE = 'object_not_in_prerequisite_state',
              HINT = 'Only one support function can be attached to an aggregate.';
    END IF;

    UPDATE pg_proc SET prosupport = newsup WHERE oid = aggfn;

    IF oldsup <> 0 THEN
      DELETE FROM pg_depend
       WHERE classid = 'pg_proc'::regclass AND objid = aggfn AND objsubid = 0
         AND refclassid = 'pg_proc'::regclass AND refobjid = oldsup
         AND refobjsubid = 0 AND deptype = 'n';
    END IF;

    -- recordDependencyOn(), spelled out.  The WHERE NOT EXISTS is what keeps
    -- it idempotent: pg_depend has no unique index, so a second attach over a
    -- hand-edited catalog would otherwise leave a duplicate row behind, and an
    -- orphaned row is a database where DROP EXTENSION never succeeds again.
    IF newsup <> 0 THEN
      INSERT INTO pg_depend
             (classid, objid, objsubid, refclassid, refobjid, refobjsubid, deptype)
      SELECT 'pg_proc'::regclass, aggfn, 0, 'pg_proc'::regclass, newsup, 0, 'n'
       WHERE NOT EXISTS (SELECT 1 FROM pg_depend
                          WHERE classid = 'pg_proc'::regclass AND objid = aggfn
                            AND objsubid = 0
                            AND refclassid = 'pg_proc'::regclass
                            AND refobjid = newsup AND refobjsubid = 0
                            AND deptype = 'n');
    END IF;
  END LOOP;

  -- A generic plan built before this call has the old shape in it and will not
  -- replan by itself; on detach that means the rewrite outlives its removal.
  -- Backend-local, exactly like the ResetPlanCache() it replaces.
  EXECUTE 'DISCARD PLANS';
END
$body$;

-- Run by the install script on 19 and later, and available by hand after
-- anything that resets pg_proc without running it -- a dump and restore, for
-- one: pg_dump does not carry catalog rows of pg_catalog objects, so a
-- restored database has the extension but not the attachment.
CREATE FUNCTION pps_attach_support()
RETURNS void
  LANGUAGE sql AS $$ SELECT @extschema@.pps_set_support(true) $$;

-- Give sum(numeric) and avg(numeric) back.  Run this before DROP EXTENSION --
-- the dependency recorded by the attachment makes the drop fail until you do.
CREATE FUNCTION pps_detach_support()
RETURNS void
  LANGUAGE sql AS $$ SELECT @extschema@.pps_set_support(false) $$;

-- The schema has to be usable by everyone.
--
-- pps_simplify_bounded_numeric_agg() finds the substitute aggregate with
-- LookupFuncName(), which goes through FuncnameGetCandidates() and
-- LookupExplicitNamespace() -- and that checks ACL_USAGE on the schema against
-- the *calling* user.  Without this grant the planner fails with "permission
-- denied for schema prosupport" the moment an ordinary user writes
-- sum(numeric) over a bounded column: not a lost optimisation, a broken query.
-- The extension used to live in public, where PUBLIC has USAGE already, so
-- this only became necessary when it moved into a schema of its own.
--
-- It gives away nothing: every function in here is revoked below, and what is
-- left callable is exactly what the planner needs to be callable.
GRANT USAGE ON SCHEMA @extschema@ TO PUBLIC;

REVOKE ALL ON FUNCTION pps_set_support(boolean) FROM PUBLIC;
REVOKE ALL ON FUNCTION pps_attach_support() FROM PUBLIC;
REVOKE ALL ON FUNCTION pps_detach_support() FROM PUBLIC;

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
-- sspace = 64 is sizeof(BoundedAggState) plus an aset chunk header, and the C code
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
-- later, where the planner reads that entry; on 18 pps_set_support() refuses
-- to run, and the hook from patches/ does the same job instead.
DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 190000 THEN
    PERFORM @extschema@.pps_attach_support();
  END IF;
END
$$;
