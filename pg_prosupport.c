/*-------------------------------------------------------------------------
 *
 * pg_prosupport.c
 *	  Module entry point: how the planner reaches the rewrites, the switches,
 *	  the plan-cache discipline that goes with them, and the one piece of
 *	  reporting all the rewrites share.
 *
 * The extension is a home for plan-time rewrites of an Aggref.  How the
 * planner gets to them depends on the server it is built against.
 *
 * PostgreSQL 19 and later call the *aggregate's* pg_proc.prosupport function
 * with a SupportRequestSimplifyAggref while constant-folding -- see
 * simplify_aggref() in optimizer/util/clauses.c.  That is the entry point
 * used there, and it is the same one core uses to turn count(1) into
 * count(*): pps_agg_support() below is attached to pg_catalog.sum(numeric)
 * and pg_catalog.avg(numeric) by pps_attach_support(), which the install
 * script runs.  Nothing has to be preloaded there -- fmgr dlopen's the
 * library the first time the planner resolves the support function -- but
 * the attachment is a write to pg_proc, so it has its counterpart in
 * pps_detach_support(); see the README.
 *
 * PostgreSQL 18 has no such call.  There the rewrites are reached through
 * agg_simplify_hook, a single global planner hook added by the patch in
 * patches/, which the patched planner calls for every Aggref it meets;
 * _PG_init() installs pps_agg_simplify_hook() there, chaining whatever hook
 * -- if any -- was already in place.  Because a hook, unlike a
 * pg_proc.prosupport entry, is not resolved on demand, on 18 the library has
 * to be loaded before planning ever happens: via shared_preload_libraries
 * (the normal way), via session_preload_libraries, or with LOAD.  CREATE
 * EXTENSION alone creates the catalog objects but does not by itself cause
 * the .so to be dlopen'd, so it is not enough on its own.
 *
 * Either way the entry point recognises and rewrites nothing itself.
 * pps_simplify_aggref() is a fixed sequence of calls, one per rewrite, each
 * living in its own module (numeric_support.c, constagg.c) with its own GUC
 * and its own decline logging; the first one to return non-NULL wins.
 * Nothing here decides which aggregates a rewrite wants -- that stays
 * entirely inside the module that owns it, so a third rewrite later means a
 * third call in that sequence, not the two existing ones being touched.
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

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/clauses.h"
#include "parser/parse_func.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/plancache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

#include "pg_prosupport.h"

PG_MODULE_MAGIC;

/*
 * Which entry point the planner offers.  PostgreSQL 19 added
 * SupportRequestSimplifyAggref and the simplify_aggref() call that issues it;
 * on 18 that does not exist and agg_simplify_hook, from the patch in
 * patches/, takes its place.
 */
#if PG_VERSION_NUM >= 190000
#define PPS_USE_AGGREF_SUPPORT	1
#else
#define PPS_USE_AGGREF_SUPPORT	0
#endif

/* the built-in aggregates whose prosupport entry we claim on 19 and later */
static const char *const pps_target_aggregates[] = {"sum", "avg"};

void		_PG_init(void);

PG_FUNCTION_INFO_V1(pps_agg_support);
PG_FUNCTION_INFO_V1(pps_attach_support);
PG_FUNCTION_INFO_V1(pps_detach_support);

#if !PPS_USE_AGGREF_SUPPORT
/* whatever agg_simplify_hook held before we installed ours */
static agg_simplify_hook_type prev_agg_simplify_hook = NULL;
#endif

/*
 * The module is loaded at postmaster start (shared_preload_libraries), by
 * LOAD/session_preload_libraries mid-session, or -- on 19 and later -- by
 * fmgr when the planner first resolves pps_agg_support().  In every one of
 * those, _PG_init() has returned before anything of ours can run.  Until it
 * has, though, the plan cache must not be touched; see
 * pps_assign_bounded_numeric_agg() and pps_assign_fold_const_sum().
 */
static bool pps_ready = false;

/*
 * pps_simplify_aggref
 *		Every rewrite this extension has, in a fixed order; NULL when none of
 *		them wanted this Aggref.
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
pps_simplify_aggref(PlannerInfo *root, Aggref *aggref)
{
	Node	   *result;

	result = pps_simplify_const_sum(aggref);
	if (result != NULL)
		return result;

	return pps_simplify_bounded_numeric_agg(root, aggref);
}

/*
 * pps_agg_support
 *		The planner support function of pg_catalog.sum(numeric) and
 *		pg_catalog.avg(numeric), once pps_attach_support() has put it there.
 *
 * Only SupportRequestSimplifyAggref is of interest; as the API requires, any
 * other request -- including every request type invented after this was
 * written -- gets a NULL pointer back rather than an error.  The contract of
 * that request is the contract the rewrites already keep: return a new node,
 * never a modified *aggref, or NULL to leave the aggregate alone.
 *
 * On PostgreSQL 18 nothing ever calls this: the request type does not exist
 * and pps_attach_support() refuses to run, so the function is here only so
 * that one install script works on every supported branch.
 */
Datum
pps_agg_support(PG_FUNCTION_ARGS)
{
	Node	   *result = NULL;

#if PPS_USE_AGGREF_SUPPORT
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplifyAggref))
	{
		SupportRequestSimplifyAggref *req;

		req = (SupportRequestSimplifyAggref *) rawreq;
		result = pps_simplify_aggref(req->root, req->aggref);
	}
#endif

	PG_RETURN_POINTER(result);
}

#if !PPS_USE_AGGREF_SUPPORT
/*
 * pps_agg_simplify_hook
 *		The patched planner's agg_simplify_hook on PostgreSQL 18: the same
 *		sequence of rewrites, chaining to whatever was there before us when
 *		none of ours applies.
 */
static Node *
pps_agg_simplify_hook(PlannerInfo *root, Aggref *aggref)
{
	Node	   *result;

	result = pps_simplify_aggref(root, aggref);
	if (result != NULL)
		return result;

	return prev_agg_simplify_hook ? prev_agg_simplify_hook(root, aggref) : NULL;
}
#endif							/* !PPS_USE_AGGREF_SUPPORT */

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

#if !PPS_USE_AGGREF_SUPPORT

	/*
	 * On 19 and later the planner finds us through pg_proc.prosupport and
	 * there is nothing to install here; on 18 this hook is the only way in.
	 */
	prev_agg_simplify_hook = agg_simplify_hook;
	agg_simplify_hook = pps_agg_simplify_hook;
#endif

	/* from here on the assign hooks may touch the plan cache */
	pps_ready = true;
}

/*
 * pps_agg_support_oid
 *		The OID of this extension's pps_agg_support(internal), looked up in
 *		the schema the calling function lives in.
 *
 * Taking the schema from the caller rather than from search_path is what
 * makes this survive CREATE EXTENSION ... SCHEMA and ALTER EXTENSION ... SET
 * SCHEMA: attach and detach then always mean *this* installation's support
 * function, never a same-named function that happens to come first in the
 * caller's search_path.
 */
static Oid
pps_agg_support_oid(FunctionCallInfo fcinfo)
{
	Oid			argtypes[1] = {INTERNALOID};
	Oid			nspoid = get_func_namespace(fcinfo->flinfo->fn_oid);
	List	   *funcname;

	funcname = list_make2(makeString(get_namespace_name(nspoid)),
						  makeString(pstrdup("pps_agg_support")));

	return LookupFuncName(funcname, 1, argtypes, false);
}

/*
 * pps_set_prosupport
 *		Point aggfnoid's pg_proc.prosupport at supportfn, or clear it when
 *		supportfn is InvalidOid, dependency included.
 *
 * This is ALTER FUNCTION ... SUPPORT in all but name.  It has to be spelled
 * out here because that command refuses an aggregate outright ("%s is an
 * aggregate function", AlterFunction() in functioncmds.c), and an aggregate
 * is exactly what we have to attach to: it is sum(numeric)'s own prosupport
 * entry that simplify_aggref() consults, nobody else's.
 *
 * The pg_depend record is the point of doing this properly rather than with
 * a bare UPDATE from the install script.  Without it, DROP EXTENSION would
 * leave sum(numeric) pointing at an OID that no longer exists, and every
 * query using sum(numeric) in that database would fail in the planner with
 * "cache lookup failed for function NNNN" -- a broken database, not a lost
 * optimisation.  With it, DROP EXTENSION fails cleanly instead, and so does
 * DROP EXTENSION ... CASCADE, because sum(numeric) is pinned and refuses to
 * be dropped along with us.  pps_detach_support() is then the one way out,
 * which is the intended one.
 */
static void
pps_set_prosupport(Oid aggfnoid, Oid supportfn)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_proc form;
	Oid			oldsupport;
	ObjectAddress myself;
	ObjectAddress referenced;

	rel = table_open(ProcedureRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", aggfnoid);
	form = (Form_pg_proc) GETSTRUCT(tup);
	oldsupport = form->prosupport;

	/* already in the state the caller wants */
	if (oldsupport == supportfn)
	{
		heap_freetuple(tup);
		table_close(rel, RowExclusiveLock);
		return;
	}

	/*
	 * Refuse to take over an entry somebody else owns, in either direction.
	 * A future release could give sum(numeric) a support function of its own,
	 * and silently replacing it -- or clearing it on detach -- would disable
	 * a core optimisation with nothing to show for it.
	 */
	if (OidIsValid(oldsupport) && OidIsValid(supportfn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s already has the support function %s",
						format_procedure(aggfnoid),
						format_procedure(oldsupport)),
				 errhint("Only one support function can be attached to an aggregate.")));

	form->prosupport = supportfn;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	ObjectAddressSet(myself, ProcedureRelationId, aggfnoid);

	if (OidIsValid(oldsupport))
		deleteDependencyRecordsForSpecific(ProcedureRelationId, aggfnoid,
										   DEPENDENCY_NORMAL,
										   ProcedureRelationId, oldsupport);
	if (OidIsValid(supportfn))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, supportfn);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	/* the next lookup of aggfnoid has to see this */
	CommandCounterIncrement();
}

/*
 * pps_attach_or_detach
 *		The body of both SQL-callable functions: sum(numeric) and
 *		avg(numeric) either point at pps_agg_support() or they do not.
 *
 * Whether the plan cache needs flushing is the same question the GUC assign
 * hooks answer: a saved generic plan built before the attachment does not
 * replan by itself.  Attaching happens during CREATE EXTENSION, where there
 * is nothing planned with it yet; detaching is the case that matters, and
 * both are cheap enough to just do unconditionally.
 */
static void
pps_attach_or_detach(FunctionCallInfo fcinfo, bool attach)
{
	Oid			ourfn;
	int			i;

#if !PPS_USE_AGGREF_SUPPORT
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aggregate support functions require PostgreSQL 19 or later"),
			 errdetail("On PostgreSQL 18 the rewrites are reached through agg_simplify_hook instead; load the library with shared_preload_libraries, session_preload_libraries or LOAD.")));
#endif

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to change the support function of a built-in aggregate")));

	ourfn = pps_agg_support_oid(fcinfo);

	for (i = 0; i < lengthof(pps_target_aggregates); i++)
	{
		Oid			argtypes[1] = {NUMERICOID};
		List	   *aggname;
		Oid			aggfnoid;

		aggname = list_make2(makeString(pstrdup("pg_catalog")),
							 makeString(pstrdup(pps_target_aggregates[i])));
		aggfnoid = LookupFuncName(aggname, 1, argtypes, false);

		/*
		 * On detach, leave alone anything that is not ours: see the comment
		 * in pps_set_prosupport() about a future core support function.
		 */
		if (!attach && get_func_support(aggfnoid) != ourfn)
			continue;

		pps_set_prosupport(aggfnoid, attach ? ourfn : InvalidOid);
	}

	ResetPlanCache();
}

/*
 * pps_attach_support
 *		Make the planner call us for sum(numeric) and avg(numeric).
 *
 * Run by the install script on 19 and later, and available by hand after
 * anything that resets pg_proc without running it -- a dump and restore, for
 * one: pg_dump does not carry catalog rows of pg_catalog objects, so a
 * restored database has the extension but not the attachment.
 */
Datum
pps_attach_support(PG_FUNCTION_ARGS)
{
	pps_attach_or_detach(fcinfo, true);

	PG_RETURN_VOID();
}

/*
 * pps_detach_support
 *		Give sum(numeric) and avg(numeric) back.  Run this before DROP
 *		EXTENSION -- the dependency recorded by the attachment makes the drop
 *		fail until you do.
 */
Datum
pps_detach_support(PG_FUNCTION_ARGS)
{
	pps_attach_or_detach(fcinfo, false);

	PG_RETURN_VOID();
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
