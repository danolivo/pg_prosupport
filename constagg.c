/*-------------------------------------------------------------------------
 *
 * constagg.c
 *	  Plan-time removal of sum() over a constant argument.
 *
 * This is a different optimisation from the one in numeric_support.c, called
 * independently and in its own right from pg_prosupport.c's
 * pps_simplify_aggref() -- the two share nothing now but
 * pps_get_sum_numeric_oid(), a read-only accessor for which aggregate is
 * pg_catalog.sum(numeric), so this module does not have to keep a second
 * cache of the same catalog fact.  There the aggregate stays and its
 * transition function is made cheaper; here the accumulation disappears
 * altogether, because the answer never depended on the rows in the first
 * place:
 *
 *		sum(c)  ->  c * NULLIF(count(*), 0)::numeric
 *
 * over n > 0 input rows sum(c) is exactly c * n, and over an empty input it is
 * NULL.  The NULLIF is what keeps that second case right, and it is not
 * optional: a scalar aggregate over an empty table produces one row holding
 * NULL, whereas count(*) produces 0 and c * 0 is 0 -- the wrong answer, and a
 * plausible-looking one.  With the NULLIF the rewrite needs to know nothing
 * about the query around it: it is equally correct for a plain scalar
 * aggregate, for GROUP BY, and for a grouping set that produces an empty
 * group.
 *
 * The dscale works out on its own.  numeric multiplication yields s1 + s2, the
 * int8-to-numeric conversion yields 0, so the product carries dscale(c) --
 * which is what sum() would have produced, every addend having the same dscale
 * as c.  NaN, +/-Infinity and a NULL constant fall out of the same expression
 * without a special case, numeric_mul() and numeric_out() being what they are.
 *
 * Two things are worth noticing about the shape of the replacement.
 *
 * It keeps an aggregate.  The rewrite could produce a bare constant for c = 0,
 * but then a query whose only aggregate was sum('0') would have none left
 * while Query.hasAggs still says it has, and count(*) is nearly free anyway --
 * int8inc against numeric_avg_accum, which allocates.
 *
 * All the rewritten aggregates in one query collapse into one.  ExecInitAgg()
 * matches Aggrefs with equal(), so twenty-six copies of sum('0') in a HAVING
 * -- the shape that prompted this -- become twenty-six references to a single
 * count(*) transition, and if the query already counts rows, to that one.
 *
 * avg(c) is deliberately not handled.  It is not c: avg() divides through
 * numeric_div(), whose result scale comes from select_div_scale() and carries
 * more fractional digits than c has, so the rewrite would have to reproduce
 * the division rather than fold it away.  That is a separate patch.
 *
 * Copyright (c) 2026, Andrei Lepikhov
 *
 * Released under the MIT licence; see the LICENSE file in this directory.
 *
 * IDENTIFICATION
 *	  contrib/pg_prosupport/constagg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "pg_prosupport.h"

/*
 * The off switch for this transformation alone.  It is separate from
 * pg_prosupport.bounded_numeric_agg because the two optimisations fail in
 * different ways: the specialised aggregates can be wrong about a scale,
 * whereas this one changes the expression tree of a query.  Whoever has to
 * decide at three in the morning which of the two to take out should not have
 * to take out both.
 *
 * Off by default, unlike pg_prosupport.bounded_numeric_agg.  numeric_support.c's
 * rewrite only ever changes which function accumulates the same aggregate;
 * this one removes the aggregate from the plan altogether and replaces it
 * with an equivalent built from count(*) -- a bigger change of plan shape,
 * and one worth opting into rather than discovering after the fact.
 *
 * The variable lives here; the GUC is defined in _PG_init() with the others,
 * so that the plan cache is flushed on the same terms.
 */
bool		pps_fold_const_sum = false;

/*
 * Operator OIDs of pg_catalog."*"(numeric, numeric) and pg_catalog."="(int8,
 * int8).  Both are pinned built-ins that cannot be dropped, so one lookup per
 * backend is enough and there is no invalidation to worry about.  The names
 * are schema-qualified: search_path must not decide whether a query is
 * rewritten.
 */
static Oid	pps_numeric_mul_op = InvalidOid;
static Oid	pps_int8_eq_op = InvalidOid;

static bool
pps_load_operators(void)
{
	if (OidIsValid(pps_numeric_mul_op) && OidIsValid(pps_int8_eq_op))
		return true;

	pps_numeric_mul_op =
		LookupOperName(NULL, list_make2(makeString("pg_catalog"),
										makeString("*")),
					   NUMERICOID, NUMERICOID, true, -1);
	pps_int8_eq_op =
		LookupOperName(NULL, list_make2(makeString("pg_catalog"),
										makeString("=")),
					   INT8OID, INT8OID, true, -1);

	return OidIsValid(pps_numeric_mul_op) && OidIsValid(pps_int8_eq_op);
}

/*
 * pps_const_arg
 *		The aggregate's argument, if it is a numeric constant.
 *
 * By the time a support function is called the argument has already been
 * through constant folding -- eval_const_expressions_mutator() recurses into
 * the Aggref before calling simplify_aggref() -- so the shapes 1C emits,
 * sum('0'::numeric) and sum(0.000::numeric(15,3)), both arrive here as a bare
 * Const.  Nothing has to be folded by hand.
 *
 * RelabelType is looked through for the sake of a domain over numeric; the
 * type check then rejects a Const that is itself of the domain type, which is
 * not worth the trouble of proving.
 */
static Const *
pps_const_arg(Aggref *agg)
{
	TargetEntry *tle;
	Node	   *node;

	if (list_length(agg->args) != 1)
		return NULL;

	tle = (TargetEntry *) linitial(agg->args);
	node = (Node *) tle->expr;

	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	if (node == NULL || !IsA(node, Const))
		return NULL;
	if (((Const *) node)->consttype != NUMERICOID)
		return NULL;

	return (Const *) node;
}

/*
 * pps_simplify_const_sum
 *		Build the replacement for sum(<numeric constant>), or return NULL.
 *
 * Called directly from pg_prosupport.c's pps_simplify_aggref() for every
 * Aggref, so unlike before this function establishes for itself, from
 * pps_get_sum_numeric_oid(), that agg really is pg_catalog.sum(numeric) --
 * nothing upstream has narrowed that down any more.
 */
Node *
pps_simplify_const_sum(Aggref *agg)
{
	Const	   *con;
	Aggref	   *count;
	NullIfExpr *nullif;
	FuncExpr   *tonumeric;
	OpExpr	   *mul;

	/*
	 * Is this even pg_catalog.sum(numeric)?  Every Aggref in every query
	 * reaches this function now, so -- exactly as in numeric_support.c's
	 * equivalent check -- declining anything else is the overwhelmingly
	 * common case, not a misconfiguration, and there is nothing to log
	 * about it.
	 */
	if (agg->aggfnoid != pps_get_sum_numeric_oid())
		return NULL;

	if (!pps_fold_const_sum)
	{
		pps_decline(agg->aggfnoid,
					"pg_prosupport.fold_const_sum is off");
		return NULL;
	}

	/*
	 * Look at the argument before anything else: almost every sum() in almost
	 * every query has a Var somewhere in it, and that one test ends the matter
	 * without a syscache lookup.  It also keeps the DEBUG chatter below to the
	 * cases that were nearly rewritten.
	 */
	con = pps_const_arg(agg);
	if (con == NULL)
		return NULL;

	/*
	 * DISTINCT is not the same aggregate at all: sum(DISTINCT c) is c, once.
	 * ORDER BY sorts by expressions held in agg->args, which the replacement
	 * does not have.  An ordered-set or hypothetical aggregate cannot be
	 * pg_catalog.sum(), but the argument list would be read differently if it
	 * were.  And an Aggref belonging to an outer query level must be left
	 * alone: preprocess_aggref() asserts agglevelsup == 0 for the aggregates
	 * of the query it is preprocessing, so the level this rewrite would be
	 * planted at is not the level that will evaluate it.
	 *
	 * FILTER, by contrast, is carried over untouched: count(*) FILTER (WHERE
	 * p) counts exactly the rows sum() FILTER (WHERE p) would have added, and
	 * a filter that matches nothing leaves count(*) at 0, which the NULLIF
	 * turns back into the NULL that sum() returns.
	 */
	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggdirectargs != NIL || agg->aggvariadic ||
		agg->aggkind != AGGKIND_NORMAL || agg->agglevelsup != 0)
	{
		pps_decline(agg->aggfnoid,
					"DISTINCT, ORDER BY, VARIADIC or an outer reference is "
					"present");
		return NULL;
	}

	if (!pps_load_operators())
	{
		pps_decline(agg->aggfnoid,
					"could not look up pg_catalog.\"*\"(numeric, numeric) or "
					"pg_catalog.\"=\"(int8, int8)");
		return NULL;
	}

	/*
	 * The support function runs during constant folding, long before
	 * preprocess_aggrefs() resolves transition types and numbers the
	 * aggregates.  The new Aggref is created in that same unresolved state and
	 * is picked up later along with the rest, being reachable from the target
	 * list or the HAVING qual exactly where the old one was.
	 */
	Assert(agg->aggtranstype == InvalidOid);
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	/*
	 * count(*).  Copying the whole struct and patching what differs is what
	 * int8inc_support() does for count(x) -> count(*), and it is the safer
	 * habit: a field added to Aggref later is inherited rather than left as
	 * whatever makeNode() zeroed it to.  aggfilter is inherited on purpose,
	 * see above; aggtype is not, sum(numeric) returning numeric where count()
	 * returns bigint.
	 */
	count = makeNode(Aggref);
	memcpy(count, agg, sizeof(Aggref));
	count->aggfnoid = F_COUNT_;
	count->aggtype = INT8OID;
	count->aggcollid = InvalidOid;
	count->inputcollid = InvalidOid;
	count->aggargtypes = NIL;
	count->args = NIL;
	count->aggstar = true;
	count->location = -1;

	/*
	 * NULLIF(count(*), 0).  NullIfExpr is an OpExpr with a different tag; the
	 * operator is the one the comparison is made with, but opresulttype is the
	 * type of the first argument, since that is what the node returns.  The
	 * executor reads opfuncid and inputcollid and nothing else, so opfuncid is
	 * filled in here rather than left for fix_opfuncids().
	 */
	nullif = makeNode(NullIfExpr);
	nullif->opno = pps_int8_eq_op;
	nullif->opfuncid = get_opcode(pps_int8_eq_op);
	nullif->opresulttype = INT8OID;
	nullif->opretset = false;
	nullif->opcollid = InvalidOid;
	nullif->inputcollid = InvalidOid;
	nullif->args = list_make2(count,
							  makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
										Int64GetDatum(0), false,
										FLOAT8PASSBYVAL));
	nullif->location = -1;

	/* the int8 -> numeric conversion, spelled out so that EXPLAIN shows it */
	tonumeric = makeFuncExpr(F_NUMERIC_INT8, NUMERICOID, list_make1(nullif),
							 InvalidOid, InvalidOid, COERCE_EXPLICIT_CAST);

	/*
	 * c * that.  The constant is copied rather than borrowed from the Aggref:
	 * the request node must be left as it was found, and a Const is cheap.
	 */
	mul = makeNode(OpExpr);
	mul->opno = pps_numeric_mul_op;
	mul->opfuncid = get_opcode(pps_numeric_mul_op);
	mul->opresulttype = NUMERICOID;
	mul->opretset = false;
	mul->opcollid = InvalidOid;
	mul->inputcollid = InvalidOid;
	mul->args = list_make2(copyObject(con), tonumeric);
	mul->location = -1;

	/*
	 * numeric is not collatable and neither is int8, so every collation above
	 * is InvalidOid by construction rather than by omission; and the result is
	 * numeric with no typmod, which is what the Aggref it replaces was.
	 */
	Assert(exprType((Node *) mul) == exprType((Node *) agg));
	Assert(exprCollation((Node *) mul) == exprCollation((Node *) agg));

	return (Node *) mul;
}
