# Does a detach in one backend reach a generic plan held by another?
#
# install.sql covers the single-session half of this.  The half that needs two
# sessions is the one that decides whether pps_detach_support() detaches
# anything at all for a connection pool: s1 holds a generic plan with the
# rewrite in it, s2 does the catalog work, and every s1 step below asks s1's
# own plan what it now thinks.
#
# Each check returns 'ok' on success and a word about the failure otherwise, so
# the expected output is the same on every branch: on 18 there is no attachment
# to make and the checks expect the stock aggregate throughout.

setup
{
    CREATE TABLE pps_t (v numeric(18,2));
    INSERT INTO pps_t SELECT g / 100.0 FROM generate_series(1, 100) g;
    ANALYZE pps_t;

    -- Does the calling session's cached plan for gp carry the rewrite?  It has
    -- to run in the session that asks, which is the whole point.
    CREATE FUNCTION pps_plan_rewritten() RETURNS boolean
    LANGUAGE plpgsql AS $$
    DECLARE
      r    record;
      plan text := '';
    BEGIN
      FOR r IN EXECUTE 'EXPLAIN (costs off, verbose) EXECUTE gp(0)' LOOP
        plan := plan || r."QUERY PLAN" || E'\n';
      END LOOP;
      RETURN plan LIKE '%bounded_numeric_sum%';
    END $$;

    CREATE FUNCTION pps_plan_ok(want_rewrite boolean) RETURNS text
    LANGUAGE plpgsql AS $$
    DECLARE
      want boolean := want_rewrite
                      AND current_setting('server_version_num')::int >= 190000;
    BEGIN
      IF pps_plan_rewritten() = want THEN
        RETURN 'ok';
      END IF;
      RETURN CASE WHEN want THEN 'rewrite missing' ELSE 'rewrite is stale' END;
    END $$;
}

teardown
{
    DO $$
    BEGIN
      IF EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'pg_prosupport') THEN
        IF current_setting('server_version_num')::int >= 190000 THEN
          PERFORM prosupport.pps_detach_support();
        END IF;
        DROP EXTENSION pg_prosupport;
      END IF;
    END $$;
    DROP FUNCTION pps_plan_ok(boolean);
    DROP FUNCTION pps_plan_rewritten();
    DROP TABLE pps_t;
}

# The session that holds the plan.  It never touches a catalog.
session s1
step s1_generic   { SET plan_cache_mode = force_generic_plan; }
step s1_prepare   { PREPARE gp(numeric) AS SELECT sum(v) FROM pps_t WHERE v > $1; }
step s1_forget    { DEALLOCATE gp; }
step s1_run       { EXECUTE gp(0); }
step s1_rewritten { SELECT pps_plan_ok(true) AS plan; }
step s1_stock     { SELECT pps_plan_ok(false) AS plan; }

# The session that installs, attaches, detaches and drops.
session s2
step s2_create { CREATE EXTENSION pg_prosupport; }
step s2_detach {
    DO $$
    BEGIN
      IF current_setting('server_version_num')::int >= 190000 THEN
        PERFORM prosupport.pps_detach_support();
      END IF;
    END $$;
}
step s2_drop { DROP EXTENSION pg_prosupport; }

# One permutation, read as a story.
#
#  1. s2 installs the extension; s1 then builds a generic plan and it carries
#     the rewrite.
#  2. s2 detaches.  s1's plan has to follow -- this is the line the no-op
#     UPDATE of bounded_numeric_sum's pg_proc row exists for.  Without it s1
#     keeps the rewrite indefinitely and the detach is a detach in name only.
#  3. s2 drops the extension.  s1 keeps working either way; the aggregates
#     disappearing is an invalidation s1's plan was always subscribed to.
#  4. s2 installs it again, and s1 *does* pick the rewrite back up -- but not
#     through anything the attach did.  Nothing could reach it that way: s1's
#     plan names the stock sum(numeric), an OID below FirstUnpinnedObjectId,
#     which no plan ever records a dependency on.  What reaches it is the
#     install script's GRANT USAGE ON SCHEMA: plancache.c registers
#     PlanCacheSysCallback on NAMESPACEOID, and that one throws away the whole
#     plan cache of every backend it reaches, dependencies or no.  Documented
#     here because it is a side effect, not a guarantee -- an install script
#     that stopped touching pg_namespace would silently stop doing it.
#  5. Re-preparing picks it up for reasons that are guaranteed, and a last
#     detach/drop leaves the database as it was found.
permutation
    s2_create s1_generic s1_prepare s1_run s1_rewritten
    s2_detach s1_stock s1_run
    s2_drop   s1_stock s1_run
    s2_create s1_rewritten s1_forget s1_prepare s1_run s1_rewritten
    s2_detach s1_stock
    s2_drop
