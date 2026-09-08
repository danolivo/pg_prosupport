/*-------------------------------------------------------------------------
 *
 * pg_prosupport.c
 *	  Module entry point: the agg_simplify_hook registration, the switches,
 *	  the plan-cache discipline that goes with them, and the one piece of
 *	  reporting all the rewrites share.
 *
 * The extension is a home for plan-time rewrites reached through
 * agg_simplify_hook, a single global hook that the patched planner (see
 * patches/) calls for every Aggref it meets.  _PG_init() below installs
 * pps_agg_simplify_hook() there, chaining whatever hook -- if any -- was
 * already in place; numeric_support.c and constagg.c do the actual work,
 * recognising sum(numeric)/avg(numeric) by aggfnoid and declining everything
 * else.
 *
 * Because the hook is what does the work, and not a pg_proc.prosupport entry
 * that fmgr resolves on demand, the library has to be loaded before planning
 * ever happens: via shared_preload_libraries (the normal way), via
 * session_preload_libraries, or with LOAD.  CREATE EXTENSION alone creates
 * the catalog objects but does not by itself cause the .so to be dlopen'd,
 * so it is not enough on its own; see the README.
 *
 * Copyright (c) 2026, Andrei Lepikhov
 *
 * Released under the MIT licence; see the LICENSE file in this directory.
 *
 * IDENTIFICATION
 *	  contrib/pg_prosupport/pg_prosupport.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "optimizer/clauses.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/plancache.h"
#include "utils/syscache.h"

#include "pg_prosupport.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

/*
 * The off switch.  A rewrite changes the behaviour of a very widely used
 * function for a whole database at once, and the hook is global and
 * installed for as long as the server runs -- there is no equivalent of
 * writing pg_proc.prosupport back to 0 to reach for at three in the morning.
 * This is the way to disable the optimisations in a single session (or a
 * single database, with ALTER DATABASE) without a restart.
 */
bool		pps_enabled = true;

/* whatever agg_simplify_hook held before we installed ours */
static agg_simplify_hook_type prev_agg_simplify_hook = NULL;

/*
 * The module is loaded either at postmaster start (shared_preload_libraries)
 * or by LOAD/session_preload_libraries mid-session; either way _PG_init()
 * runs before any query of this session is planned, well before
 * pps_agg_simplify_hook() can ever be called.  Until _PG_init() has
 * finished, though, the plan cache must not be touched; see
 * pps_assign_enabled().
 */
static bool pps_ready = false;

/*
 * pps_agg_simplify_hook
 *		The planner's agg_simplify_hook, chaining to whatever was there
 *		before us.
 *
 * All the actual recognising-and-rewriting logic is pps_simplify_aggref()'s;
 * this exists only to fall through to prev_agg_simplify_hook when our own
 * rewrite declines, the standard pattern for a hook more than one loaded
 * module might want.
 */
static Node *
pps_agg_simplify_hook(PlannerInfo *root, Aggref *aggref)
{
	Node	   *result = pps_simplify_aggref(root, aggref);

	if (result != NULL)
		return result;

	return prev_agg_simplify_hook ? prev_agg_simplify_hook(root, aggref) : NULL;
}

/*
 * pps_assign_enabled
 *		Flush the plan cache when the switch is flipped.
 *
 * Without this the off switch does not switch anything off: a saved generic
 * plan was built with the rewrite in it and will not replan by itself, because
 * a GUC is not a source of plan invalidation.  In a connection pool, where a
 * generic plan can live for hours, those are precisely the sessions the switch
 * was added for.  Core does the same for session_replication_role
 * (assign_session_replication_role in trigger.c), including the "do not flush
 * needlessly" test.
 *
 * The hook runs before the variable is assigned, hence the comparison rather
 * than an unconditional flush.
 *
 * The delicate part is pps_ready.  If pg_prosupport.enabled = off already
 * sat in a placeholder GUC when this module loaded -- from postgresql.conf
 * together with shared_preload_libraries, or from an earlier SET in the
 * session together with session_preload_libraries or LOAD --
 * DefineCustomBoolVariable() below reassigns the real variable and fires
 * this hook while _PG_init() is still running.  Calling ResetPlanCache() at
 * that point would be wrong: nothing has been planned with our hook
 * installed yet, so there is nothing to flush, and during
 * shared_preload_libraries processing there is not even a plan cache to
 * flush.
 */
static void
pps_assign_enabled(bool newval, void *extra)
{
	if (pps_ready && pps_enabled != newval)
		ResetPlanCache();
}

/* the same for the switch of the constant rewrite; see constagg.c */
static void
pps_assign_fold_const_sum(bool newval, void *extra)
{
	if (pps_ready && pps_fold_const_sum != newval)
		ResetPlanCache();
}

void
_PG_init(void)
{
	DefineCustomBoolVariable("pg_prosupport.enabled",
							 "Enable the plan-time aggregate rewrites of pg_prosupport.",
							 "Turning this off leaves the support functions "
							 "attached but makes them decline every rewrite, "
							 "which is the way to disable the optimisations "
							 "without touching the catalog.",
							 &pps_enabled,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, pps_assign_enabled, NULL);

	DefineCustomBoolVariable("pg_prosupport.fold_const_sum",
							 "Replace sum() over a constant argument with a multiplication of count(*).",
							 "This is the transformation in constagg.c, which "
							 "removes the aggregation instead of specialising "
							 "it.  It is switched separately from the rest "
							 "because it rewrites the query's expression tree.",
							 &pps_fold_const_sum,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, pps_assign_fold_const_sum, NULL);

	CacheRegisterSyscacheCallback(PROCOID, pps_syscache_reset, (Datum) 0);

	MarkGUCPrefixReserved("pg_prosupport");

	prev_agg_simplify_hook = agg_simplify_hook;
	agg_simplify_hook = pps_agg_simplify_hook;

	/* from here on the assign hooks may touch the plan cache */
	pps_ready = true;
}

/*
 * pps_decline
 *		Record why a rewrite did not happen.
 *
 * There are many ways to decline and they all look identical from outside --
 * EXPLAIN simply still shows the built-in aggregate.  During an incident that
 * is the first question asked, so the reason has to be obtainable without a
 * rebuild: SET client_min_messages = debug1 and one EXPLAIN.
 *
 * elog() evaluates its arguments only when the level is interesting, so
 * neither the reason nor the get_func_name() lookup costs anything on the hot
 * planning path.
 */
void
pps_decline(Oid aggfnoid, const char *reason)
{
	elog(DEBUG1, "pg_prosupport: leaving %s() alone: %s",
		 get_func_name(aggfnoid), reason);
}
