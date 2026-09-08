/*-------------------------------------------------------------------------
 *
 * numeric_support.c
 *	  Plan-time substitution of specialised aggregates for sum(numeric) and
 *	  avg(numeric).
 *
 * A support function attached to the aggregate itself receives a
 * SupportRequestSimplifyAggref and, if the input can be shown to be narrow
 * enough, returns a new Aggref carrying a different aggfnoid.
 *
 * The proof is static.  It comes either from the argument's declared typmod or
 * from arithmetic over declared operands (see pps_derive_bounds): at
 * 1 <= p <= 28 the mantissa value * 10^s is below 10^28 ~ 2^93, so the int128
 * accumulator cannot overflow in fewer than about 1.7e10 rows.
 *
 * The scale is passed as a second, constant argument.  That makes it an
 * explicit input of the transition function rather than something guessed from
 * the first value seen.
 *
 * Copyright (c) 2026, Andrei Lepikhov
 *
 * Released under the MIT licence; see the LICENSE file in this directory.
 *
 * IDENTIFICATION
 *	  contrib/pg_prosupport/numeric_support.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "nodes/supportnodes.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/plancache.h"
#include "utils/syscache.h"

#include "pg_prosupport.h"

/*
 * numeric's typmod encoding changed in PG15: the scale became signed and now
 * occupies eleven bits.  pps_typmod_scale() is written for that encoding.  On
 * an older server it would quietly return the wrong scale, and a wrong scale
 * produces a wrong sum with nothing to show for it -- better not to build.
 */
#if PG_VERSION_NUM < 150000
#error "pg_prosupport requires PostgreSQL 15 or later (numeric typmod encoding)"
#endif

PG_FUNCTION_INFO_V1(pps_agg_support);

/* the mantissa fits in int128 and the accumulator holds ~1.7e10 addends */
#define PPS_MAX_PRECISION	28

/*
 * The highest precision a factor of a folded product may have.  Both mantissas
 * have to fit an int64, because that is what int128_add_int64_mul_int64()
 * takes; see pps_scaled_accum_mul().
 */
#define PPS_MAX_FACTOR_PRECISION	18

/*
 * Cached OIDs.  Dropped on any PROCOID invalidation: the built-in aggregates
 * and ours are all pg_proc rows, and the extension can be dropped and
 * recreated in a different schema without reconnecting.
 */
static Oid	pps_sum_numeric_oid = InvalidOid;
static Oid	pps_avg_numeric_oid = InvalidOid;
static Oid	pps_scaled_sum_oid = InvalidOid;
static Oid	pps_scaled_avg_oid = InvalidOid;
static Oid	pps_scaled_sum_mul_oid = InvalidOid;
static Oid	pps_scaled_avg_mul_oid = InvalidOid;
static Oid	pps_cached_for_support = InvalidOid;
static bool pps_cache_valid = false;
static bool pps_oids_ok = false;

/* see pps_agg_support(): complain about a wrong binding once per backend */
static bool pps_warned_wrong_aggregate = false;

/*
 * Dropped on any change to pg_proc, not just to our two rows.  Comparing
 * hashvalue would be more precise, but over-invalidating costs two syscache
 * hits on the next planning cycle, and pg_proc rarely changes in a running
 * database.  If a profile ever says otherwise, this is the place to sharpen.
 */
void
pps_syscache_reset(Datum arg, int cacheid, uint32 hashvalue)
{
	pps_cache_valid = false;
	pps_oids_ok = false;
	pps_warned_wrong_aggregate = false;
}

/* numeric typmod -> precision and scale; the scale is sign-extended */
static inline int
pps_typmod_precision(int32 typmod)
{
	return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

static inline int
pps_typmod_scale(int32 typmod)
{
	return (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
}

/*
 * pps_load_oids
 *		Look up the built-in aggregates we know about and our replacements.
 *
 * All names are schema-qualified, so search_path has no say in the result --
 * which matters, or the substitution would happen or not depending on a
 * session setting.  The extension's own schema is resolved dynamically because
 * the extension is relocatable.
 *
 * Returns false when our aggregates are missing: the extension may have been
 * dropped with the binding left in place, and there is no reason to fail
 * planning over that -- the query simply computes the stock aggregate.  A
 * missing built-in, on the other hand, means a broken catalog, and that is an
 * error rather than a reason to carry on quietly.
 */
static bool
pps_load_oids(Oid self)
{
	Oid			nsp;
	Oid			builtin_args[1] = {NUMERICOID};
	Oid			scaled_args[2] = {NUMERICOID, INT4OID};
	Oid			mul_args[4] = {NUMERICOID, NUMERICOID, INT4OID, INT4OID};

	if (pps_cache_valid && pps_cached_for_support == self)
		return pps_oids_ok;

	pps_sum_numeric_oid = LookupFuncName(list_make2(makeString("pg_catalog"),
													makeString("sum")),
										 1, builtin_args, true);
	pps_avg_numeric_oid = LookupFuncName(list_make2(makeString("pg_catalog"),
													makeString("avg")),
										 1, builtin_args, true);
	if (!OidIsValid(pps_sum_numeric_oid) || !OidIsValid(pps_avg_numeric_oid))
		elog(ERROR, "could not find pg_catalog.sum(numeric) or pg_catalog.avg(numeric)");

	nsp = get_func_namespace(self);
	if (OidIsValid(nsp))
	{
		char	   *nspname = get_namespace_name(nsp);

		pps_scaled_sum_oid =
			LookupFuncName(list_make2(makeString(nspname),
									  makeString("numeric_scaled_sum")),
						   2, scaled_args, true);
		pps_scaled_avg_oid =
			LookupFuncName(list_make2(makeString(nspname),
									  makeString("numeric_scaled_avg")),
						   2, scaled_args, true);
		pps_scaled_sum_mul_oid =
			LookupFuncName(list_make2(makeString(nspname),
									  makeString("numeric_scaled_sum_mul")),
						   4, mul_args, true);
		pps_scaled_avg_mul_oid =
			LookupFuncName(list_make2(makeString(nspname),
									  makeString("numeric_scaled_avg_mul")),
						   4, mul_args, true);
	}
	else
	{
		pps_scaled_sum_oid = InvalidOid;
		pps_scaled_avg_oid = InvalidOid;
		pps_scaled_sum_mul_oid = InvalidOid;
		pps_scaled_avg_mul_oid = InvalidOid;
	}

	pps_oids_ok = OidIsValid(pps_scaled_sum_oid) &&
		OidIsValid(pps_scaled_avg_oid) &&
		OidIsValid(pps_scaled_sum_mul_oid) &&
		OidIsValid(pps_scaled_avg_mul_oid);
	pps_cached_for_support = self;
	pps_cache_valid = true;

	if (!pps_oids_ok)
		elog(DEBUG1, "pg_prosupport: the specialised aggregates were "
			 "not found; the extension was probably dropped without "
			 "detaching the support function first");

	return pps_oids_ok;
}

/*
 * pps_numeric_typmod
 *		The expression's numeric typmod, or -1.
 *
 * Looks through RelabelType in order to catch domains: a domain over numeric
 * is coerced to its base type by relabelling, after which exprType() says
 * NUMERICOID and exprTypmod() says -1, so the column's declaration is lost.
 * We unwrap the domain ourselves and take the typmod from there.
 *
 * The argument need not be a bare column.  exprTypmod() also yields a typmod
 * for CaseExpr and CoalesceExpr -- but only when it is the same in every arm,
 * which is exactly the condition under which every value really does carry the
 * required dscale.  So sum(CASE WHEN ... THEN a ELSE b END) over two
 * numeric(18,2) columns is substituted as well, and that is covered by a test
 * rather than deduced from first principles.  PREPARE parameters, by contrast,
 * never qualify: their paramtypmod is -1 even when the type was declared as
 * numeric(18,2).
 */
static int32
pps_numeric_typmod(Node *node)
{
	Oid			typid;
	int32		typmod;

	typid = exprType(node);
	typmod = exprTypmod(node);

	if (typid != NUMERICOID)
	{
		/* exprTypmod() of a domain-typed expression is always -1 */
		if (typmod != -1)
			return -1;
		typid = getBaseTypeAndTypmod(typid, &typmod);
		if (typid != NUMERICOID)
			return -1;
	}

	return typmod;
}

/*
 * pps_const_bounds
 *		Exact precision and scale of a numeric constant.
 *
 * A numeric Const carries no typmod -- exprTypmod() returns -1 for it -- so
 * without this a single literal anywhere in the argument would sink the whole
 * derivation, and sum(qty * 1.2) would never be specialised.
 *
 * The value is printed and its digits are counted.  numeric_out() always emits
 * plain decimal notation, never an exponent, and it prints exactly as many
 * fractional digits as the value's dscale -- which is the number we need,
 * because the dscale of a product or a sum is derived from the dscales of the
 * operands.  This runs once per Const at plan time, so the cost of formatting
 * does not matter.
 */
static bool
pps_const_bounds(Const *con, int *p, int *s)
{
	char	   *str;
	char	   *digits;
	char	   *dot;
	int			intlen;
	int			fraclen;

	if (con->constisnull || con->consttype != NUMERICOID)
		return false;

	str = DatumGetCString(DirectFunctionCall1(numeric_out, con->constvalue));

	digits = str;
	if (*digits == '-' || *digits == '+')
		digits++;

	/* NaN, Infinity and -Infinity start with a letter; they are not ours */
	if (*digits < '0' || *digits > '9')
	{
		pfree(str);
		return false;
	}

	dot = strchr(digits, '.');
	if (dot != NULL)
	{
		intlen = (int) (dot - digits);
		fraclen = (int) strlen(dot + 1);
	}
	else
	{
		intlen = (int) strlen(digits);
		fraclen = 0;
	}

	/* leading zeroes are not significant digits: 0.05 needs numeric(2,2) */
	while (intlen > 0 && *digits == '0')
	{
		digits++;
		intlen--;
	}

	*s = fraclen;
	*p = Max(intlen + fraclen, 1);

	pfree(str);

	return true;
}

/*
 * pps_const_int
 *		The value of an int4 constant, if that is what this node is.
 *
 * Used for the second argument of round() and trunc(), which has to be known
 * at plan time for the result's scale to be known at all.
 */
static bool
pps_const_int(Node *node, int *value)
{
	Const	   *con;

	if (!IsA(node, Const))
		return false;

	con = (Const *) node;
	if (con->constisnull || con->consttype != INT4OID)
		return false;

	*value = DatumGetInt32(con->constvalue);
	return true;
}

/*
 * pps_derive_bounds
 *		Precision and scale of an arithmetic expression over numeric.
 *
 * exprTypmod() gives up on an OpExpr and returns -1, so without this
 * sum(a * b) and sum(a - b) are never specialised -- and those are the shapes
 * that actually show up in reporting queries, where the plain sum(column) is
 * the exception rather than the rule.
 *
 * The rules below are exact, not conservative, and that matters in two
 * different ways.
 *
 * The scale has to be exact, because it becomes the dscale of the result.  An
 * over-estimate would make the aggregate print 1.230 where core prints 1.23
 * -- a silent difference, the worst kind.  For avg() it matters one step
 * further on, through select_div_scale().  numeric guarantees the scale of these
 * three operations: multiplication yields s1 + s2, addition and subtraction
 * yield max(s1, s2), and both keep trailing zeroes rather than stripping them
 * (mul_var and add_var set result->dscale accordingly).  Division is
 * deliberately absent: select_div_scale() derives the quotient's scale from
 * the operands' *values*, so it is not a function of the typmods at all.
 *
 * The precision only decides whether we accept, so an over-estimate there
 * would merely cost an optimisation.  It is exact anyway: a product has at
 * most p1 + p2 digits, and a sum at most max(p1-s1, p2-s2) + 1 + max(s1, s2).
 *
 * round(x, n) and trunc(x, n) are handled too, but only with a constant n:
 * numeric_round() sets the result's dscale to exactly max(n, 0), and rounding
 * can carry into one more integer digit -- round(9.99, 1) is 10.0 -- which the
 * +1 in the precision accounts for.  Division is not, and neither is division
 * by a constant: select_div_scale() looks at the weights of the operand
 * *values*, so even a / 100 has a scale that varies from row to row.
 *
 * A mistake here does not produce a wrong answer.  The transition function
 * checks every value against the scale it was specialised on and raises an
 * error if it does not fit, so a derivation bug surfaces as an error rather
 * than as a corrupted sum.  That is the reason this is allowed to be clever at
 * all.
 *
 * depth bounds the recursion.  Eight is not a computed limit; it is more than
 * twice the depth of anything seen in the reporting queries this was written
 * for, and walking further into a user-written expression for an optimisation
 * we are free to decline buys nothing.  A long left-deep chain such as
 * a+b+c+d+... will therefore be declined, which is the intended trade.
 */
#define PPS_MAX_DERIVE_DEPTH	8

static bool
pps_derive_bounds(Node *node, int depth, int *p, int *s)
{
	int32		typmod;

	if (node == NULL || depth > PPS_MAX_DERIVE_DEPTH)
		return false;

	while (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	/* A declared typmod, on a column, a cast, a CASE or a domain */
	typmod = pps_numeric_typmod(node);
	if (typmod >= (int32) VARHDRSZ)
	{
		*p = pps_typmod_precision(typmod);
		*s = pps_typmod_scale(typmod);
		return (*p >= 1 && *p <= PPS_MAX_PRECISION && *s >= 0 && *s <= *p);
	}

	if (IsA(node, Const))
		return pps_const_bounds((Const *) node, p, s) &&
			*p <= PPS_MAX_PRECISION;

	if (IsA(node, OpExpr))
	{
		OpExpr	   *op = (OpExpr *) node;
		Oid			opfunc = get_opcode(op->opno);
		int			p1,
					s1,
					p2,
					s2;

		if (opfunc == F_NUMERIC_UMINUS)
		{
			if (list_length(op->args) != 1)
				return false;
			return pps_derive_bounds((Node *) linitial(op->args), depth + 1,
									 p, s);
		}

		if (list_length(op->args) != 2)
			return false;
		if (opfunc != F_NUMERIC_ADD && opfunc != F_NUMERIC_SUB &&
			opfunc != F_NUMERIC_MUL)
			return false;

		if (!pps_derive_bounds((Node *) linitial(op->args), depth + 1,
							   &p1, &s1) ||
			!pps_derive_bounds((Node *) lsecond(op->args), depth + 1,
							   &p2, &s2))
			return false;

		if (opfunc == F_NUMERIC_MUL)
		{
			*s = s1 + s2;
			*p = p1 + p2;
		}
		else
		{
			*s = Max(s1, s2);
			*p = Max(p1 - s1, p2 - s2) + 1 + *s;
		}

		/*
		 * Bail out as soon as the bound is exceeded rather than at the end:
		 * the intermediate numbers stay small, and a subtree that cannot help
		 * is not walked any further.
		 */
		return (*p <= PPS_MAX_PRECISION && *s <= *p);
	}

	if (IsA(node, FuncExpr))
	{
		FuncExpr   *fe = (FuncExpr *) node;
		int			p1,
					s1,
					n;

		if (fe->funcid != F_ROUND_NUMERIC_INT4 &&
			fe->funcid != F_TRUNC_NUMERIC_INT4)
			return false;
		if (list_length(fe->args) != 2)
			return false;

		/*
		 * The single-argument round(numeric) and trunc(numeric) are SQL
		 * functions that the planner inlines into these two with a constant
		 * zero, so they are covered here as well.
		 */
		if (!pps_const_int((Node *) lsecond(fe->args), &n))
			return false;
		if (!pps_derive_bounds((Node *) linitial(fe->args), depth + 1,
							   &p1, &s1))
			return false;

		*s = Max(n, 0);
		*p = (p1 - s1) + 1 + *s;

		return (*p <= PPS_MAX_PRECISION && *s <= *p);
	}

	return false;
}

/*
 * pps_split_product
 *		Is the argument a numeric multiplication whose factors we can fold?
 *
 * When it is, the whole sum(a * b) becomes a four-argument aggregate that
 * multiplies inside the transition function, and no intermediate numeric is
 * built at all.  That is worth a special case of its own: left to
 * numeric_mul(), a product costs about four times what accumulating it does,
 * almost all of it in allocating and packing a value that is read once and
 * thrown away.
 *
 * The factors need not be columns -- each side goes through
 * pps_derive_bounds() like any other argument, so (a + 1) * b qualifies too.
 * Both have to come out at PPS_MAX_FACTOR_PRECISION or less so that each
 * mantissa fits an int64, and their precisions have to add up within
 * PPS_MAX_PRECISION, which is the same headroom rule as everywhere else.
 *
 * Returns false whenever anything does not fit; the caller then falls back to
 * specialising only the accumulation, which is still better than nothing.
 */
static bool
pps_split_product(Node *node, Node **larg, Node **rarg, int *s1, int *s2)
{
	OpExpr	   *op;
	Node	   *lhs;
	Node	   *rhs;
	int			p1;
	int			p2;

	while (IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;

	if (!IsA(node, OpExpr))
		return false;

	op = (OpExpr *) node;
	if (list_length(op->args) != 2 || get_opcode(op->opno) != F_NUMERIC_MUL)
		return false;

	lhs = (Node *) linitial(op->args);
	rhs = (Node *) lsecond(op->args);

	if (!pps_derive_bounds(lhs, 0, &p1, s1) ||
		!pps_derive_bounds(rhs, 0, &p2, s2))
		return false;

	if (p1 > PPS_MAX_FACTOR_PRECISION || p2 > PPS_MAX_FACTOR_PRECISION)
		return false;
	if (p1 + p2 > PPS_MAX_PRECISION)
		return false;

	*larg = lhs;
	*rarg = rhs;
	return true;
}

/*
 * pps_scale_const
 *		A plain int4 constant to plant in the rewritten Aggref.
 */
static TargetEntry *
pps_scale_const(int value, AttrNumber resno)
{
	Const	   *con = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
								Int32GetDatum(value), false, true);

	return makeTargetEntry((Expr *) con, resno, NULL, false);
}

/*
 * pps_simplify_aggref
 *		The whole rewrite, with no dependency on the support-function protocol.
 *
 * Returns the replacement node, or NULL to leave the aggregate alone.  The
 * caller passes the OID of the function this code is reached through --
 * pps_load_oids() needs it to find the extension's own schema.
 *
 * This lives apart from pps_agg_support() so that a fork without
 * SupportRequestSimplifyAggref can call the rewrite from wherever it does have
 * an Aggref in hand.
 */
Node *
pps_simplify_aggref(Aggref *agg, Oid supportfnoid)
{
	TargetEntry *tle;
	Node	   *lhs;
	Node	   *rhs;
	int			p,
				s,
				s1,
				s2;
	Oid			newfn;
	Aggref	   *newagg;
	Node	   *folded;
	bool		is_sum;
	bool		scaled_ok;

	/*
	 * pps_load_oids() prints its own reason when the specialised aggregates
	 * cannot be found.  That is no longer a reason to give up immediately: the
	 * constant-argument rewrite below does not need them, so the answer is
	 * remembered and acted on only where it matters.
	 */
	scaled_ok = pps_load_oids(supportfnoid);

	/*
	 * Which aggregate were we attached to?  The binding lives in an ALTER
	 * FUNCTION, that is outside this code, so a support function attached to
	 * something we do not know how to rewrite would otherwise be free to
	 * produce nonsense.  Here it costs two comparisons.
	 */
	if (agg->aggfnoid == pps_sum_numeric_oid)
	{
		is_sum = true;
		newfn = pps_scaled_sum_oid;
	}
	else if (agg->aggfnoid == pps_avg_numeric_oid)
	{
		is_sum = false;
		newfn = pps_scaled_avg_oid;
	}
	else
	{
		/*
		 * Complain once per backend rather than once per Aggref.  This is
		 * planning: an unconditional WARNING on a hot query would flood both
		 * the log and the client, and would do so precisely when something
		 * else in the log needs to be found.  The flag is cleared along with
		 * the OID cache, so a binding that is fixed and then broken again will
		 * speak up.
		 */
		if (!pps_warned_wrong_aggregate)
		{
			pps_warned_wrong_aggregate = true;
			ereport(WARNING,
					(errmsg("pg_prosupport support function is attached to aggregate %u",
							agg->aggfnoid),
					 errdetail("This support function only knows how to rewrite pg_catalog.sum(numeric) and pg_catalog.avg(numeric); the aggregate is left alone."),
					 errhint("Detach it by setting that aggregate's pg_proc.prosupport back to 0.")));
		}
		return NULL;
	}

	/*
	 * A constant argument is the one case where the aggregation does not have
	 * to happen at all, so it is tried before any specialisation.  constagg.c
	 * owns that transformation, its conditions and its switch, and returns
	 * NULL whenever it does not apply.
	 */
	if (is_sum)
	{
		folded = pps_simplify_const_sum(agg);

		if (folded != NULL)
			return folded;
	}

	/* everything below replaces the aggregate with one of ours */
	if (!scaled_ok)
		return NULL;

	/*
	 * The conditions under which we fire.  Each one declines rather than tries
	 * to work around the problem: the price of a mistake here is a silently
	 * wrong sum, not a slow query.
	 *
	 * aggsplit is not tested: SupportRequestSimplifyAggref arrives during
	 * preprocessing, before the planner splits the aggregate, so it is always
	 * AGGSPLIT_SIMPLE here.  Partial aggregation is in fact supported --
	 * through combinefunc -- and a test for it would mislead the reader.
	 */
	Assert(agg->aggsplit == AGGSPLIT_SIMPLE);

	if (agg->aggdistinct != NIL || agg->aggorder != NIL ||
		agg->aggfilter != NULL || agg->aggvariadic ||
		agg->agglevelsup != 0)
	{
		pps_decline(agg->aggfnoid,
					"DISTINCT, ORDER BY, FILTER, VARIADIC or an outer "
					"reference is present");
		return NULL;
	}

	if (list_length(agg->args) != 1)
	{
		pps_decline(agg->aggfnoid,
					"aggregate does not have exactly one argument");
		return NULL;
	}

	tle = (TargetEntry *) linitial(agg->args);

	/*
	 * First choice: the argument is a product we can take apart, in which case
	 * the multiplication is folded into the transition function and no
	 * intermediate numeric is ever built.  Falling back to the ordinary form
	 * when it is not still specialises the accumulation.
	 */
	if (pps_split_product((Node *) tle->expr, &lhs, &rhs, &s1, &s2))
	{
		newfn = (newfn == pps_scaled_sum_oid) ? pps_scaled_sum_mul_oid
			: pps_scaled_avg_mul_oid;

		newagg = copyObject(agg);
		newagg->aggfnoid = newfn;
		newagg->args = list_make4(makeTargetEntry((Expr *) copyObject(lhs),
												  1, NULL, false),
								  makeTargetEntry((Expr *) copyObject(rhs),
												  2, NULL, false),
								  pps_scale_const(s1, 3),
								  pps_scale_const(s2, 4));
		newagg->aggargtypes = list_make4_oid(NUMERICOID, NUMERICOID,
											 INT4OID, INT4OID);

		return (Node *) newagg;
	}

	/*
	 * pps_derive_bounds() enforces the range as it goes -- at p <= 28 the
	 * mantissa is below 10^28 ~ 2^93 and the int128 accumulator holds ~1.7e10
	 * addends -- so there is nothing left to check here.  A negative scale
	 * (possible since PG15) is rejected there too, or a -2 would end up in
	 * 10^s and break everything quietly.
	 */
	if (!pps_derive_bounds((Node *) tle->expr, 0, &p, &s))
	{
		pps_decline(agg->aggfnoid,
					"could not derive a precision and scale for the "
					"argument within the supported range");
		return NULL;
	}

	Assert(p >= 1 && p <= PPS_MAX_PRECISION && s >= 0 && s <= p);

	/*
	 * Build a copy rather than editing the node in place: the comment on
	 * SupportRequestSimplifyAggref requires it.
	 */
	newagg = copyObject(agg);
	newagg->aggfnoid = newfn;
	newagg->args = lappend(newagg->args, pps_scale_const(s, 2));
	newagg->aggargtypes = lappend_oid(newagg->aggargtypes, INT4OID);

	return (Node *) newagg;
}

Datum
pps_agg_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (IsA(rawreq, SupportRequestSimplifyAggref))
	{
		SupportRequestSimplifyAggref *req;

		if (!pps_enabled)
		{
			pps_decline(agg->aggfnoid,
						"pg_prosupport.enabled is off");
			return NULL;
		}

		req = (SupportRequestSimplifyAggref *) rawreq;

		PG_RETURN_POINTER(pps_simplify_aggref(req->aggref,
											  fcinfo->flinfo->fn_oid));
	}

	PG_RETURN_POINTER(NULL);
}
