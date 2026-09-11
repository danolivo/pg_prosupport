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
 * already in place.
 *
 * pps_agg_simplify_hook() itself recognises and rewrites nothing.  It is a
 * fixed sequence of calls, one per rewrite, each living in its own module
 * (numeric_support.c, constagg.c) with its own GUC and its own decline
 * logging; the first one to return non-NULL wins.  Nothing here decides
 * which aggregates a rewrite wants -- that stays entirely inside the module
 * that owns it, so a third rewrite later means a third call added below, not
 * the two existing ones being touched.
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

/* whatever agg_simplify_hook held before we installed ours */
static agg_simplify_hook_type prev_agg_simplify_hook = NULL;

/*
 * The module is loaded either at postmaster start (shared_preload_libraries)
 * or by LOAD/session_preload_libraries mid-session; either way _PG_init()
 * runs before any query of this session is planned, well before
 * pps_agg_simplify_hook() can ever be called.  Until _PG_init() has
 * finished, though, the plan cache must not be touched; see
 * pps_assign_bounded_numeric_agg() and pps_assign_fold_const_sum().
 */
static bool pps_ready = false;

/*
 * pps_agg_simplify_hook
 *		The planner's agg_simplify_hook: a fixed sequence of independent
 *		rewrites, chaining to whatever was there before us when none of ours
 *		applies.
 *
 * Each call below is a whole rewrite owned by its own module -- its own
 * recognition of the aggregate shape, its own GUC, its own pps_decline()
 * logging -- and this function does not look inside any of them.  They are
 * tried in a fixed order rather than independently: sum() over a numeric
 * constant, with both switches on, could equally be eliminated by
 * pps_simplify_const_sum() or narrowed by pps_simplify_bounded_numeric_agg(),
 * and eliminating the aggregation outright is strictly the better plan, so
 * the constant fold goes first and whichever returns non-NULL first wins.
 * With pps_fold_const_sum off (the default), that case simply falls through
 * to the second call, unaffected.
 */
static Node *
pps_agg_simplify_hook(PlannerInfo *root, Aggref *aggref)
{
	Node	   *result;

	result = pps_simplify_const_sum(aggref);
	if (result != NULL)
		return result;

	result = pps_simplify_bounded_numeric_agg(root, aggref);
	if (result != NULL)
		return result;

	return prev_agg_simplify_hook ? prev_agg_simplify_hook(root, aggref) : NULL;
}

/*
 * pps_assign_bounded_numeric_agg
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
 * The delicate part is pps_ready.  If pg_prosupport.bounded_numeric_agg = off already
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
pps_assign_bounded_numeric_agg(bool newval, void *extra)
{
	if (pps_ready && pps_bounded_numeric_agg != newval)
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
	DefineCustomBoolVariable("pg_prosupport.bounded_numeric_agg",
							 "Specialise sum()/avg() over a numeric of bounded precision and scale.",
							 "This is the rewrite in numeric_support.c: sum()/avg() "
							 "narrowed to bounded_numeric_sum/bounded_numeric_avg "
							 "(and the product fold on top of it), whenever the "
							 "argument's precision and scale are known and small "
							 "enough.  On by default: narrowing an accumulator is "
							 "not a change of plan shape a DBA needs to opt into.",
							 &pps_bounded_numeric_agg,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, pps_assign_bounded_numeric_agg, NULL);

	DefineCustomBoolVariable("pg_prosupport.fold_const_sum",
							 "Replace sum() over a constant argument with a multiplication of count(*).",
							 "This is the transformation in constagg.c, which "
							 "removes the aggregation instead of specialising "
							 "it.  Off by default -- unlike pg_prosupport.bounded_numeric_agg, "
							 "this one changes the shape of the query's expression "
							 "tree, not just an aggregate's accumulator, so it is "
							 "opt-in.  It is switched separately from "
							 "pg_prosupport.bounded_numeric_agg because the two rewrites "
							 "fail in different ways.",
							 &pps_fold_const_sum,
							 false,
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
