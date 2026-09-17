--
-- Installation, removal, re-installation.
--
-- Nothing here is about which plan a query gets; that is what numeric_agg.sql
-- and const_agg.sql are for.  This file is about the two catalog writes the
-- extension makes outside its own schema -- pg_proc.prosupport and the
-- pg_depend row that pins it -- surviving a full drop/create cycle, and about
-- a generic plan built while the rewrite was in force not outliving it.
--
-- Every check raises on failure and is silent on success, so the expected
-- output is the same on every branch.  On 18 there is no attachment to make
-- and the checks assert its absence instead; the library is deliberately not
-- LOADed here, so the rewrite is reached through pg_proc or not at all.
--
CREATE EXTENSION pg_prosupport;

-- Is the catalog in the state this branch calls "attached"?  Two prosupport
-- entries and two pg_depend rows on 19 and later, none of either on 18.
CREATE FUNCTION pps_check(attached boolean, ctx text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE
  ourfn oid;
  nsup  int;
  ndep  int;
  want  int := CASE WHEN attached
                     AND current_setting('server_version_num')::int >= 190000
                    THEN 2 ELSE 0 END;
BEGIN
  SELECT p.oid INTO ourfn
    FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace
   WHERE n.nspname = 'prosupport' AND p.proname = 'pps_agg_support';
  IF ourfn IS NULL THEN
    RAISE EXCEPTION '%: pps_agg_support() is not in schema prosupport', ctx;
  END IF;

  SELECT count(*) INTO nsup FROM pg_proc
   WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure::oid,
                 'pg_catalog.avg(numeric)'::regprocedure::oid)
     AND prosupport::oid = ourfn;

  SELECT count(*) INTO ndep FROM pg_depend
   WHERE classid = 'pg_proc'::regclass
     AND objid IN ('pg_catalog.sum(numeric)'::regprocedure::oid,
                   'pg_catalog.avg(numeric)'::regprocedure::oid)
     AND objsubid = 0
     AND refclassid = 'pg_proc'::regclass AND refobjid = ourfn
     AND refobjsubid = 0 AND deptype = 'n';

  IF nsup <> want OR ndep <> want THEN
    RAISE EXCEPTION '%: % prosupport entries, % pg_depend rows, wanted % of each',
                    ctx, nsup, ndep, want;
  END IF;
END
$$;


-- Is the rewrite reachable in *this* session right now?  Not the same question
-- as "which branch is this", which is what this file used to ask and got
-- wrong.  On 19 and later the planner reads sum(numeric)'s prosupport entry,
-- so the answer is whether we are attached.  On 18 the rewrite arrives through
-- agg_simplify_hook, which _PG_init() installs when the library is loaded --
-- and CREATE EXTENSION loads it in the session that runs it, because the
-- C-language validator dlopen's the library to check the symbols.  So on 18
-- the answer is simply whether the extension exists: this session created it.
CREATE FUNCTION pps_rewrite_live() RETURNS boolean
LANGUAGE sql STABLE AS $$
  SELECT CASE
           WHEN current_setting('server_version_num')::int >= 190000
             THEN EXISTS (SELECT 1 FROM pg_proc
                           WHERE oid = 'pg_catalog.sum(numeric)'::regprocedure
                             AND prosupport <> 0)
           ELSE EXISTS (SELECT 1 FROM pg_extension
                         WHERE extname = 'pg_prosupport')
         END
$$;

SELECT pps_check(true, 'after CREATE EXTENSION');

--
-- Idempotence.  pg_depend has no unique index, so a repeated attach that
-- inserted blindly would leave a duplicate row behind -- and an orphaned row
-- is a database where DROP EXTENSION never succeeds again.
--
DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 190000 THEN
    PERFORM prosupport.pps_attach_support();
    PERFORM prosupport.pps_attach_support();
  END IF;
END
$$;
SELECT pps_check(true, 'after attaching twice more');

--
-- Not relocatable.  pps_attach_support()/pps_detach_support() are plpgsql and
-- carry the schema name @extschema@ was substituted with, so a move would
-- strand them; asking for one has to be refused.
--
DO $$
BEGIN
  BEGIN
    ALTER EXTENSION pg_prosupport SET SCHEMA public;
    RAISE EXCEPTION 'ALTER EXTENSION ... SET SCHEMA was not refused';
  EXCEPTION WHEN others THEN
    IF SQLERRM NOT LIKE '%does not support SET SCHEMA%' THEN
      RAISE EXCEPTION 'unexpected error from SET SCHEMA: %', SQLERRM;
    END IF;
  END;
END
$$;
SELECT pps_check(true, 'after a refused SET SCHEMA');

--
-- Superuser only, on every branch: the privilege check inside
-- pps_set_support() comes before the version check for exactly this reason.
--
CREATE ROLE regress_pps_plain;
GRANT EXECUTE ON FUNCTION prosupport.pps_detach_support() TO regress_pps_plain;
SET ROLE regress_pps_plain;
DO $$
BEGIN
  BEGIN
    PERFORM prosupport.pps_detach_support();
    RAISE EXCEPTION 'a non-superuser was allowed to detach';
  EXCEPTION WHEN insufficient_privilege THEN
    NULL;
  END;
END
$$;
RESET ROLE;
SELECT pps_check(true, 'after a refused detach');

--
-- A generic plan built while the rewrite was in force must not outlive it.
-- Without the plan-cache flush in pps_set_support() the entry below keeps
-- bounded_numeric_sum in its cached generic plan: harmless until DROP
-- EXTENSION, and then "cache lookup failed for function NNNN" on the next
-- EXECUTE.  That is the failure this section is here to catch.
--
CREATE TABLE pps_gp (v numeric(18,2));
INSERT INTO pps_gp SELECT g / 100.0 FROM generate_series(1, 100) g;
ANALYZE pps_gp;

--
-- Before any of that: the rewrite has to work for a user who is not the owner
-- of anything here.  ExecInitAgg() checks ACL_EXECUTE on aggref->aggfnoid
-- against the calling user, and the planner substitutes bounded_numeric_sum
-- behind that user's back, so the REVOKE block in the install script has to
-- stop short of the aggregates -- while the transition functions, checked
-- against the aggregate's owner instead, may be and are revoked.  If that
-- distinction is ever lost, this is where it shows.
--
GRANT SELECT ON pps_gp TO regress_pps_plain;
SET ROLE regress_pps_plain;
DO $$
DECLARE
  r     record;
  plan  text := '';
  live  boolean := pps_rewrite_live();
  total numeric;
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (costs off, verbose) SELECT sum(v) FROM pps_gp' LOOP
    plan := plan || r."QUERY PLAN" || E'\n';
  END LOOP;
  IF (plan LIKE '%bounded_numeric_sum%') <> live THEN
    RAISE EXCEPTION 'plan for a non-superuser does not match the session: %', plan;
  END IF;

  SELECT sum(v) INTO total FROM pps_gp;		-- must not raise
  IF total <> 50.50 THEN
    RAISE EXCEPTION 'a non-superuser summed to % instead of 50.50', total;
  END IF;
END
$$;
RESET ROLE;
REVOKE SELECT ON pps_gp FROM regress_pps_plain;

SET plan_cache_mode = force_generic_plan;
PREPARE gp(numeric) AS SELECT sum(v) FROM pps_gp WHERE v > $1;
EXECUTE gp(0);
EXECUTE gp(0);

-- The cached generic plan has the rewrite in it exactly when this branch
-- attaches, and the right answer either way.
CREATE FUNCTION pps_plan_has_rewrite() RETURNS boolean
LANGUAGE plpgsql AS $$
DECLARE
  r    record;
  plan text := '';
BEGIN
  FOR r IN EXECUTE 'EXPLAIN (costs off, verbose) EXECUTE gp(0)' LOOP
    plan := plan || r."QUERY PLAN" || E'\n';
  END LOOP;
  RETURN plan LIKE '%bounded_numeric_sum%';
END
$$;

DO $$
DECLARE
  live boolean := pps_rewrite_live();
BEGIN
  IF pps_plan_has_rewrite() <> live THEN
    RAISE EXCEPTION 'generic plan before detach does not match the session';
  END IF;
END
$$;

-- Detach, and the plan must follow.  Twice, because detaching what is already
-- detached is a no-op, not an error.
DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 190000 THEN
    PERFORM prosupport.pps_detach_support();
    PERFORM prosupport.pps_detach_support();
  END IF;
END
$$;
SELECT pps_check(false, 'after detach');

DO $$
BEGIN
  IF pps_plan_has_rewrite() <> pps_rewrite_live() THEN
    RAISE EXCEPTION 'the generic plan does not match the session after detach';
  END IF;
END
$$;
EXECUTE gp(0);

--
-- Drop it.  The prepared statement is still there, still generic, and must
-- keep working once the aggregates it could have named are gone.
--
DROP EXTENSION pg_prosupport;
-- One, deliberately.  CREATE EXTENSION creates the schema named in the control
-- file but records no ownership of it, so DROP EXTENSION leaves it behind --
-- empty, and reused by the next CREATE EXTENSION.  Asserted rather than
-- wished away: if a later release starts dropping it, this line is where we
-- find out.
SELECT count(*) AS prosupport_schemas_left
  FROM pg_namespace WHERE nspname = 'prosupport';
EXECUTE gp(0);

--
-- And put it back.  A second CREATE EXTENSION has to reach the same catalog
-- state as the first, not an accumulated one.
--
CREATE EXTENSION pg_prosupport;
SELECT pps_check(true, 'after re-creating the extension');
EXECUTE gp(0);

-- Leaving it as we found it.
DO $$
BEGIN
  IF current_setting('server_version_num')::int >= 190000 THEN
    PERFORM prosupport.pps_detach_support();
  END IF;
END
$$;
DROP EXTENSION pg_prosupport;

DEALLOCATE gp;
RESET plan_cache_mode;
DROP TABLE pps_gp;
DROP FUNCTION pps_plan_has_rewrite();
DROP FUNCTION pps_rewrite_live();
DROP FUNCTION pps_check(boolean, text);
DROP ROLE regress_pps_plain;
