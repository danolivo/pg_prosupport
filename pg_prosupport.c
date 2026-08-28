/*-------------------------------------------------------------------------
 *
 * pg_prosupport.c
 *	  Module entry point: the switches, the plan-cache discipline that goes
 *	  with them, and the one piece of reporting all the rewrites share.
 *
 * The extension is a home for plan-time rewrites that PostgreSQL's prosupport
 * machinery makes reachable from outside the core.  Each rewrite lives in its
 * own file and is switched separately; this file owns nothing but what they
 * have in common.
 *
 * There is one support function per target aggregate, and it is attached by
 * writing pg_proc.prosupport -- see the README.  Nothing here needs a patched
 * server: SupportRequestSimplifyAggref is stock, and so is the field.
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
 * function for a whole database at once, and taking it off by writing
 * pg_proc.prosupport needs superuser and is global.  That is not enough: when
 * it turns out at three in the morning that some row source does not honour
 * its own typmod, one needs a way to disable the optimisation in a single
 * session without touching the catalog.
 */
bool		pps_enabled = true;

/*
 * The module is loaded lazily -- on the first call of a support function, that
 * is from the middle of planning.  Until _PG_init() has finished, the plan
 * cache must not be touched; see pps_assign_enabled().
 */
static bool pps_ready = false;

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
 * The delicate part is pps_ready.  The module loads lazily: if a session did
 * SET pg_prosupport.enabled = off before the load, the value sat in a
 * placeholder, and DefineCustomBoolVariable() below reassigns it, firing this
 * hook.  That happens inside the first planning cycle, that is inside
 * BuildCachedPlan(), and ResetPlanCache() would mark invalid the very
 * CachedPlanSource being built.  There is also nothing to flush at that
 * moment: no plan knows about any rewrite yet, the extension has only just
 * been loaded.
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
