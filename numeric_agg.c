/*-------------------------------------------------------------------------
 *
 * numeric_agg.c
 *	  sum(numeric) and avg(numeric) specialised on a scale known at plan time.
 *
 * The support function in numeric_support.c proves that the argument's
 * precision and scale satisfy 1 <= p <= 28 and 0 <= s <= p, and rewrites the
 * Aggref to call bounded_numeric_sum(numeric, int4) or
 * bounded_numeric_avg(numeric, int4) with s supplied as a constant.  Both share
 * everything but the final function, just as core shares numeric_avg_accum
 * between sum() and avg().
 *
 * That buys the thing this was written for: the per-group state is a single
 * cache line with no pointers in it, whereas NumericAggState carries
 * NumericVars whose digit arrays are allocated separately.  It is that, rather
 * than the ratio of the two sizeofs, that moves the spill point of a
 * HashAggregate.
 *
 * Where the contract ends.  p <= 28 means the mantissa value * 10^s is below
 * 10^28 ~ 2^93, so the int128 accumulator cannot overflow in fewer than about
 * 1.7e10 rows.  But a typmod is a promise, not a guarantee: a value can arrive
 * from an FDW, from a function declared to return numeric(18,2), or out of a
 * record cast.  Every value is therefore checked against the declaration, and
 * a broken promise raises an error rather than silently producing a wrong sum.
 *
 * Copyright (c) 2026, Andrei Lepikhov
 *
 * Released under the MIT licence; see the LICENSE file in this directory.
 *
 * IDENTIFICATION
 *	  contrib/pg_prosupport/numeric_agg.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int128.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "varatt.h"

/* see the matching check in numeric_support.c: numeric's typmod encoding */
#if PG_VERSION_NUM < 150000
#error "pg_prosupport requires PostgreSQL 15 or later (numeric typmod encoding)"
#endif

/*
 * numeric_add_opt_error() was renamed to numeric_add_safe() in PostgreSQL 19,
 * where its last argument changed from a bool * ("set this instead of raising
 * an error") to a Node *escontext, the soft-error convention the rest of the
 * tree had moved to.  NULL means "raise the error yourself" in both, which is
 * what the single caller below wants, so one macro covers every branch we
 * build against.
 */
#if PG_VERSION_NUM >= 190000
#define pps_numeric_add(n1, n2)		numeric_add_safe((n1), (n2), NULL)
#else
#define pps_numeric_add(n1, n2)		numeric_add_opt_error((n1), (n2), NULL)
#endif

/*
 * The on-disk layout of numeric.  utils/numeric.h does not expose it -- it is
 * private to numeric.c -- so the extension has to repeat it here.  Checked
 * against master as of fd2b89854d9; the format has not changed since 8.3, but
 * if it ever does we would start reading garbage without crashing, hence the
 * StaticAssertDecls below.
 */
typedef int16 NumericDigit;

#define NBASE		10000
#define DEC_DIGITS	4

struct NumericShort
{
	uint16		n_header;
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];
};

struct NumericLong
{
	uint16		n_sign_dscale;
	int16		n_weight;
	NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER];
};

union NumericChoice
{
	uint16		n_header;
	struct NumericLong n_long;
	struct NumericShort n_short;
};

StaticAssertDecl(sizeof(NumericDigit) == 2, "NumericDigit must be int16");
StaticAssertDecl(offsetof(struct NumericLong, n_data) == 4,
				 "unexpected NumericLong layout");
StaticAssertDecl(offsetof(struct NumericShort, n_data) == 2,
				 "unexpected NumericShort layout");

#define NUMERIC_SIGN_MASK		0xC000
#define NUMERIC_POS				0x0000
#define NUMERIC_NEG				0x4000
#define NUMERIC_SHORT			0x8000
#define NUMERIC_SPECIAL			0xC000

/* special values differ in the whole header, not just the two top bits */
#define NUMERIC_NAN				0xC000
#define NUMERIC_PINF			0xD000
#define NUMERIC_NINF			0xF000

#define NUMERIC_SHORT_SIGN_MASK			0x2000
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK	0x0040
#define NUMERIC_SHORT_WEIGHT_MASK		0x003F

/*
 * These macros take a pointer to a NumericChoice, that is VARDATA_ANY(datum).
 * Written that way they work the same for the one-byte header that comes out
 * of a tuple and for the four-byte one left behind by detoasting.
 */
#define PPS_FLAGBITS(c)		((c)->n_header & NUMERIC_SIGN_MASK)
#define PPS_IS_SHORT(c)		(PPS_FLAGBITS(c) == NUMERIC_SHORT)
#define PPS_IS_SPECIAL(c)	(PPS_FLAGBITS(c) == NUMERIC_SPECIAL)
#define PPS_IS_NAN(c)		((c)->n_header == NUMERIC_NAN)
#define PPS_IS_PINF(c)		((c)->n_header == NUMERIC_PINF)
#define PPS_IS_NINF(c)		((c)->n_header == NUMERIC_NINF)

#define PPS_WEIGHT(c) \
	(PPS_IS_SHORT(c) ? \
	 (((c)->n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? \
	   ~NUMERIC_SHORT_WEIGHT_MASK : 0) \
	  | ((c)->n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK)) : \
	 (c)->n_long.n_weight)

#define PPS_IS_NEG(c) \
	(PPS_IS_SHORT(c) ? \
	 (((c)->n_short.n_header & NUMERIC_SHORT_SIGN_MASK) != 0) : \
	 (((c)->n_long.n_sign_dscale & NUMERIC_SIGN_MASK) == NUMERIC_NEG))

#define PPS_CHOICE_HDR(c) \
	((int) (sizeof(uint16) + (PPS_IS_SHORT(c) ? 0 : sizeof(int16))))

#define PPS_DIGITS(c) \
	(PPS_IS_SHORT(c) ? (c)->n_short.n_data : (c)->n_long.n_data)

#define PPS_NDIGITS(c, datalen) \
	((int) (((datalen) - PPS_CHOICE_HDR(c)) / sizeof(NumericDigit)))

/*
 * The highest precision we accept.
 *
 * The ceiling is set by the accumulator's headroom, not by the width of the
 * mantissa.  At p = 28 a value is below 10^28, which is a shade over 2^93, so
 * the accumulator takes on the order of 1.7e10 addends before it reaches
 * 2^127; PPS_MAX_ADDENDS below turns that into a hard limit.  At p = 36, which
 * does occur in 1C schemas, the mantissa reaches 2^120: two addends fit in an
 * int128 and the third overflows, so that range needs a different accumulator.
 * Hence 28 rather than 38.
 *
 * There is deliberately no separate variant for p <= 18 keeping the mantissa
 * in an int64, although one existed and looked like the obvious optimisation.
 * Measured on identical numeric(18,2) data the int128 path was not slower in
 * any of four shapes (twelve aggregates with GROUP BY: 1320 ms against 1572;
 * one aggregate, no grouping: 282 against 305; three million groups: a tie;
 * parallel: a tie), and that held for the build using the INT128 emulation
 * too.  A second transition function buying nothing is not worth keeping.
 */
#define PPS_MAX_PRECISION	28

/*
 * The largest number of addends the accumulator is allowed to take.
 *
 * Every addend is a mantissa with at most PPS_MAX_PRECISION decimal digits, so
 * after N of them |sumX| <= N * (10^28 - 1).  That stays inside an int128 as
 * long as
 *
 *		N <= (2^127 - 1) / (10^28 - 1) = 17014118346
 *
 * which is the constant below.  Note that it is a little *under* 2^34 =
 * 17179869184: 10^28 is about 1.0097 * 2^93, not 2^93, and the odd percent
 * matters once the number is used as a hard limit rather than as an order of
 * magnitude.  A folded product obeys the same bound because the support
 * function only folds when p1 + p2 <= PPS_MAX_PRECISION; see
 * PPS_MAX_FACTOR_PRECISION.
 *
 * Why this is tested at all, when sum(int8) accumulates into an int128 with no
 * test whatever: there each addend is below 2^63, so the limit is 2^64 addends
 * and no table will ever reach it.  Here the addends are thirty binary orders
 * wider and the limit comes down to 1.7e10 rows in a single group -- large, but
 * no longer absurd for a fact table.  Overflow of a signed 128-bit integer is
 * undefined behaviour, not a wrapped result, so "the sum would come out wrong"
 * understates it.  The test costs one compare against a constant on a counter
 * we increment anyway, which is not worth economising on.
 */
#define PPS_MAX_ADDENDS		INT64CONST(17014118346)

/*
 * The aggregate state.  Special values are counted separately, exactly as
 * NumericAggState does: numeric_sum() looks at them before it looks at the
 * sum, and our final function has to reproduce that order.
 */
typedef struct NasAggState
{
	/*
	 * INT128, not a bare __int128.  The difference is not cosmetic: the
	 * natural alignment of __int128 is 16 bytes, palloc only promises MAXALIGN
	 * -- that is 8 -- and a compiler that sees a 16-byte-aligned type as the
	 * first member is entitled to emit an aligned 16-byte move.  That move
	 * segfaults on a state received from a parallel worker.  INT128 in
	 * common/int128.h is declared with pg_attribute_aligned(MAXIMUM_ALIGNOF)
	 * for precisely this case; see PGAC_TYPE_128BIT_INT in
	 * config/c-compiler.m4.
	 */
	INT128		sumX;			/* sum of mantissas at scale "scale" */
	int64		N;				/* count of ordinary values */
	int64		NaNcount;
	int64		pInfcount;
	int64		nInfcount;
	int32		scale;			/* the declared scale, 0..28 */
} NasAggState;

/*
 * sspace in pg_prosupport--1.0.sql is declared as 64, which is the
 * sizeof below plus an aset chunk header.  The planner takes the size of a
 * hash table entry from there (hash_agg_entry_size()); if the declared value
 * drifts away from the real one, HashAgg gets chosen where GroupAgg was the
 * right plan, and it spills without having planned to.
 *
 * The assertion is on equality rather than on "no larger" on purpose: a
 * mismatch in either direction means a wrong estimate, and a human should look
 * at it.  It will also fire on a platform with a different MAXIMUM_ALIGNOF --
 * that is not a false alarm but exactly the case where sspace needs
 * recomputing.
 */
StaticAssertDecl(sizeof(NasAggState) == 56,
				 "NasAggState changed size; recompute sspace in "
				 "pg_prosupport--1.0.sql");

#define PPS_TOTAL_COUNT(st) \
	((st)->N + (st)->NaNcount + (st)->pInfcount + (st)->nInfcount)

/* 10^i for i < DEC_DIGITS */
static const int32 pow10_int32[DEC_DIGITS] = {1, 10, 100, 1000};

/*
 * pps_int128_mul_add
 *		*i128 = *i128 * mul + add.
 *
 * The caller must guarantee *i128 >= 0, 0 < mul <= NBASE and 0 <= add < NBASE;
 * under those conditions no intermediate value overflows.  There is no check
 * on the 128-bit result itself -- the digit-count bound in
 * pps_get_bounded_int128() takes its place.
 *
 * common/int128.h has no such helper because numeric.c gets by with addition
 * and the product of two int64s.  What we need is a Horner loop over the
 * base-NBASE digits, and expressing that through int128_add_int64_mul_int64()
 * would mean splitting the digit array into pieces -- longer, and with more
 * corner cases, than these ten lines.
 */
static inline void
pps_int128_mul_add(INT128 *i128, int32 mul, int32 add)
{
#if USE_NATIVE_INT128
	*i128 = *i128 * mul + add;
#else
	uint64		lo = i128->lo;
	uint64		hi = (uint64) i128->hi;
	uint64		m = (uint64) mul;
	uint64		p0;
	uint64		p1;

	/*
	 * lo * m + add, split across 32-bit halves.  Each product is at most
	 * (2^32 - 1) * NBASE + NBASE < 2^46, so uint64 is enough.
	 */
	p0 = (lo & UINT64CONST(0xFFFFFFFF)) * m + (uint64) add;
	p1 = (lo >> 32) * m + (p0 >> 32);

	i128->lo = (p1 << 32) | (p0 & UINT64CONST(0xFFFFFFFF));
	i128->hi = (int64) (hi * m + (p1 >> 32));
#endif
}

/*
 * pps_int128_neg
 *		*i128 = -*i128.
 */
static inline void
pps_int128_neg(INT128 *i128)
{
#if USE_NATIVE_INT128
	*i128 = -*i128;
#else
	/*
	 * Ordinary two's complement: there is a carry into the high half exactly
	 * when the low half was zero.
	 */
	i128->lo = ~i128->lo + 1;
	i128->hi = ~i128->hi + (i128->lo == 0 ? 1 : 0);
#endif
}

/*
 * pps_int128_hi_int64 / pps_int128_lo_uint64
 *		The high and low 64-bit halves of an INT128, in the same layout
 *		common/int128.h uses for its emulated struct.  Needed by
 *		pps_bounded_serialize()/pps_bounded_deserialize(), which ship the two
 *		halves separately down the wire, and by pps_int128_fits_int64() below,
 *		which looks at them individually to test for sign extension.
 *
 * On PostgreSQL 18 that header hands out whole INT128 values only and exposes
 * no accessor at all.  PostgreSQL 19 added PG_INT128_HI_INT64() and
 * PG_INT128_LO_UINT64(), which do exactly this -- hence the private names
 * here.  Reusing the upstream ones would be worse than a redefinition: since
 * they are function-like macros, the preprocessor would rewrite the
 * definitions below into a call to itself, and the compiler would report a
 * missing type specifier several lines away from the actual cause.
 *
 * The same reasoning applies to the three helpers further down, which
 * PostgreSQL 19 also grew: everything this file needs from an INT128 beyond
 * what 18 provides is defined here, once, and used on every branch.
 */
static inline int64
pps_int128_hi_int64(INT128 v)
{
#if USE_NATIVE_INT128
	return (int64) (((uint128) v) >> 64);
#else
	return v.hi;
#endif
}

static inline uint64
pps_int128_lo_uint64(INT128 v)
{
#if USE_NATIVE_INT128
	return (uint64) v;
#else
	return v.lo;
#endif
}

/*
 * pps_make_int128
 *		The inverse of the two accessors above: rebuild an INT128 from its
 *		halves, the way pps_bounded_deserialize() needs after reading them
 *		back separately.
 */
static inline INT128
pps_make_int128(int64 hi, uint64 lo)
{
#if USE_NATIVE_INT128
	return (((INT128) hi) << 64) | (INT128) lo;
#else
	INT128		v;

	v.hi = hi;
	v.lo = lo;
	return v;
#endif
}

/*
 * pps_int128_add_int128
 *		*i128 += addend.
 *
 * PostgreSQL 18's common/int128.h has no such helper: every core caller there
 * only ever adds a bare int64 or an int64*int64 product into an accumulator.
 * pps_bounded_accum() and pps_bounded_combine() are the exception, merging one
 * full 128-bit accumulator into another, so it is provided here in the same
 * style as pps_int128_mul_add() and pps_int128_neg() above.
 */
static inline void
pps_int128_add_int128(INT128 *i128, INT128 addend)
{
#if USE_NATIVE_INT128
	*i128 += addend;
#else
	/*
	 * Add the unsigned low half through int128_add_uint64(), which already
	 * propagates the carry into the high half correctly, then add the high
	 * half on top.  That is exactly addend.hi * 2^64 + addend.lo added to
	 * *i128, which is what addend equals under two's complement.
	 */
	int128_add_uint64(i128, addend.lo);
	i128->hi += addend.hi;
#endif
}

/*
 * pps_int128_div_mod_int32
 *		*i128 /= divisor, truncating toward zero; *remainder is set to the
 *		truncating remainder, which takes the sign of the original *i128.
 *
 * divisor must be a positive int32 well below 2^31 -- the only caller,
 * pps_int128_to_numeric(), only ever passes 1000000000 -- so the emulated
 * path below does not have to worry about a negative or overflowing
 * divisor.  (PostgreSQL 19's int128_div_mod_int32() is the same operation
 * without that restriction.)
 */
static inline void
pps_int128_div_mod_int32(INT128 *i128, int32 divisor, int32 *remainder)
{
#if USE_NATIVE_INT128
	INT128		q;
	INT128		r;

	Assert(divisor > 0);

	q = *i128 / divisor;
	r = *i128 % divisor;

	*i128 = q;
	*remainder = (int32) r;
#else
	bool		neg;
	uint32		word[4];
	uint32		quot[4];
	uint64		rem = 0;
	int			i;

	Assert(divisor > 0);

	/*
	 * Schoolbook long division by a one-word divisor, on the magnitude: take
	 * the sign aside, negate if needed (pps_int128_neg() again, exactly as
	 * above), split the 128-bit magnitude into four 32-bit words, and bring
	 * them down into the running remainder one at a time.  "rem" never
	 * reaches "divisor", which is below 2^31, so rem << 32 | word[i] fits
	 * comfortably in a uint64 and each quotient word fits in a uint32.
	 */
	neg = (i128->hi < 0);
	if (neg)
		pps_int128_neg(i128);

	word[0] = (uint32) (((uint64) i128->hi) >> 32);
	word[1] = (uint32) (((uint64) i128->hi) & UINT64CONST(0xFFFFFFFF));
	word[2] = (uint32) (i128->lo >> 32);
	word[3] = (uint32) (i128->lo & UINT64CONST(0xFFFFFFFF));

	for (i = 0; i < 4; i++)
	{
		uint64		cur = (rem << 32) | word[i];

		quot[i] = (uint32) (cur / (uint32) divisor);
		rem = cur % (uint32) divisor;
	}

	i128->hi = (int64) (((uint64) quot[0] << 32) | quot[1]);
	i128->lo = ((uint64) quot[2] << 32) | quot[3];

	if (neg)
	{
		pps_int128_neg(i128);
		*remainder = -(int32) rem;
	}
	else
		*remainder = (int32) rem;
#endif
}

/*
 * pps_int128_fits_int64
 *		Does the value fit an int64, and if so what is it?
 *
 * The ordinary sign-extension test: the high half has to be a copy of the sign
 * bit of the low one.
 */
static inline bool
pps_int128_fits_int64(INT128 v, int64 *out)
{
	int64		hi = pps_int128_hi_int64(v);
	uint64		lo = pps_int128_lo_uint64(v);

	if ((hi == 0 && (lo >> 63) == 0) ||
		(hi == -1 && (lo >> 63) == 1))
	{
		*out = int128_to_int64(v);
		return true;
	}
	return false;
}

/*
 * pps_decimal_len
 *		Number of decimal digits in one base-NBASE digit, 1..DEC_DIGITS.
 *
 * Called once per value, before the loop over digits, and it is what lets that
 * loop run without an overflow check on every iteration.
 */
static inline int
pps_decimal_len(NumericDigit d)
{
	Assert(d > 0 && d < NBASE);

	return 1 + (d >= 10) + (d >= 100) + (d >= 1000);
}

/*
 * pps_get_bounded_int128
 *		The value's mantissa at scale target_scale.
 *
 * Returns false when the value does not fit -- that is, when the promise made
 * by the typmod was broken by the row source.  The caller must tell that apart
 * from the normal path and report an error; passing it over silently is not an
 * option.
 *
 * Why the loops carry no overflow checks.  The mantissa at target_scale has at
 * most
 *
 *		pps_decimal_len(digits[0]) + DEC_DIGITS * weight + target_scale
 *
 * decimal digits.  That bound is exact, because digits[0] is the leading
 * base-NBASE digit and weight fixes its position.  Testing that sum against 28
 * once, before the loops, guarantees that the result and every intermediate
 * value stay below 10^28 < 2^93.  The test has to be exact rather than
 * conservative: numeric(28,0) legitimately holds twenty-eight nines, and a
 * cruder bound such as DEC_DIGITS * (weight + 1) would reject them as overflow.
 *
 * Scale alignment folds into the same loop.  The stored scale is a multiple of
 * DEC_DIGITS while the value's dscale equals target_scale, so
 * stored - target_scale lies in [0, DEC_DIGITS-1] and 10^down divides NBASE
 * exactly:
 *
 *		mantissa = D_head * (NBASE / 10^down) + last_digit / 10^down
 *
 * where D_head is every base-NBASE digit but the last.  This is the common
 * case, not a corner: the stored digits almost always run past dscale inside
 * the last one.
 *
 * The other direction (stored < target_scale) happens too: make_result()
 * strips trailing zero digits, so 1.00 in a numeric(18,2) column is stored as
 * a single digit with stored = 0.
 */
static bool
pps_get_bounded_int128(union NumericChoice *c, int datalen, int target_scale,
					  INT128 *out)
{
	NumericDigit *digits;
	INT128		m;
	int			ndigits;
	int			weight;
	int			stored;
	int			i;

	Assert(target_scale >= 0 && target_scale <= PPS_MAX_PRECISION);

	ndigits = PPS_NDIGITS(c, datalen);
	weight = PPS_WEIGHT(c);
	digits = PPS_DIGITS(c);

	m = int64_to_int128(0);

	if (ndigits <= 0)
	{
		*out = m;
		return true;
	}

	if (unlikely(digits[0] <= 0 || digits[0] >= NBASE))
		return false;			/* not normalised -- the source lied */
	if (unlikely(pps_decimal_len(digits[0]) + DEC_DIGITS * weight +
				 target_scale > PPS_MAX_PRECISION))
		return false;

	stored = DEC_DIGITS * (ndigits - 1 - weight);

	if (stored >= target_scale)
	{
		int			down = stored - target_scale;
		int32		split;

		if (unlikely(down >= DEC_DIGITS))
			return false;

		split = pow10_int32[down];

		for (i = 0; i < ndigits - 1; i++)
			pps_int128_mul_add(&m, NBASE, digits[i]);

		if (unlikely(digits[ndigits - 1] % split != 0))
			return false;		/* a non-zero tail would mean rounding */

		/*
		 * The last step folds the scale alignment into the same operation:
		 * multiply by NBASE/split (which divides exactly, split being a power
		 * of ten below DEC_DIGITS) and add the truncated digit.
		 */
		pps_int128_mul_add(&m, NBASE / split, digits[ndigits - 1] / split);
	}
	else
	{
		int			up = target_scale - stored;

		if (unlikely(up > PPS_MAX_PRECISION))
			return false;

		for (i = 0; i < ndigits; i++)
			pps_int128_mul_add(&m, NBASE, digits[i]);

		/* multiply by 10^up using factors no larger than NBASE */
		while (up >= DEC_DIGITS)
		{
			pps_int128_mul_add(&m, NBASE, 0);
			up -= DEC_DIGITS;
		}
		if (up > 0)
			pps_int128_mul_add(&m, pow10_int32[up], 0);
	}

	if (PPS_IS_NEG(c))
		pps_int128_neg(&m);

	*out = m;
	return true;
}

/*
 * The highest precision either operand of a folded product may have.
 *
 * The product is accumulated with int128_add_int64_mul_int64(), which takes
 * two int64s, so each mantissa has to fit one.  The headroom of the
 * accumulator is a separate constraint and is checked in the support function:
 * the product is below 10^(p1+p2) and p1+p2 has to stay within
 * PPS_MAX_PRECISION, exactly as it does when the multiplication is left to
 * numeric_mul() and only the sum is specialised.
 */
#define PPS_MAX_FACTOR_PRECISION	18

PG_FUNCTION_INFO_V1(pps_bounded_accum);
PG_FUNCTION_INFO_V1(pps_bounded_accum_mul);
PG_FUNCTION_INFO_V1(pps_bounded_combine);
PG_FUNCTION_INFO_V1(pps_bounded_serialize);
PG_FUNCTION_INFO_V1(pps_bounded_deserialize);
PG_FUNCTION_INFO_V1(pps_bounded_sum_final);
PG_FUNCTION_INFO_V1(pps_bounded_avg_final);

/*
 * pps_make_state
 *		Allocate the state in the aggregate's memory context.
 *
 * The context is not optional: the state outlives the call, and if an error is
 * thrown part way through a group there is nobody left to free it by hand --
 * resetting the context does that reliably.
 */
static NasAggState *
pps_make_state(MemoryContext aggcontext, int32 scale)
{
	MemoryContext oldcontext;
	NasAggState *st;

	Assert(scale >= 0 && scale <= PPS_MAX_PRECISION);

	oldcontext = MemoryContextSwitchTo(aggcontext);
	st = (NasAggState *) palloc0(sizeof(NasAggState));
	MemoryContextSwitchTo(oldcontext);

	st->scale = scale;
	return st;
}

/*
 * pps_accum_overflow
 *		Complain that the accumulator has taken all the addends it can hold.
 *
 * Out of line and deliberately not inlined into its callers: what is left at
 * each call site is a single compare against a constant, which the branch
 * predictor gets right every time, and none of the ereport() machinery is in
 * the transition function's instruction footprint.
 */
static void
pps_accum_overflow(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("too many values for specialised numeric aggregate"),
			 errdetail("The specialisation accumulates at most " INT64_FORMAT
					   " values per group.",
					   PPS_MAX_ADDENDS),
			 errhint("Set pg_prosupport.bounded_numeric_agg to off to fall back "
					 "on the built-in aggregate, which has no such limit.")));
}

/*
 * pps_check_addends
 *		Raise an error if taking "adding" more addends could overflow the
 *		accumulator that already holds "have" of them.
 *
 * Both counts are known to be non-negative and no larger than PPS_MAX_ADDENDS,
 * so the subtraction below cannot itself overflow.  It is written that way
 * round rather than as (have + adding > PPS_MAX_ADDENDS) so that it stays
 * correct if that ever stops being true.
 *
 * Callers must call this *before* the addition, not after: once the addition
 * has happened the undefined behaviour has happened with it, and there is
 * nothing left to detect.
 */
static inline void
pps_check_addends(int64 have, int64 adding)
{
	if (unlikely(have > PPS_MAX_ADDENDS - adding))
		pps_accum_overflow();
}

/*
 * pps_bounded_accum
 *		Transition function: (internal, numeric, int4) -> internal.
 *
 * The third argument is the constant planted by the support function.  It is a
 * constant by construction, so the scale is read once when the state is
 * created and never looked at again.
 */
Datum
pps_bounded_accum(PG_FUNCTION_ARGS)
{
	NasAggState *st;
	MemoryContext aggcontext;
	struct varlena *arg;
	struct varlena *detoasted;
	union NumericChoice *c;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "pps_bounded_accum called in non-aggregate context");

	if (PG_ARGISNULL(0))
	{
		int32		scale;

		if (PG_ARGISNULL(2))
			elog(ERROR, "scale argument of pps_bounded_accum must not be null");

		scale = PG_GETARG_INT32(2);
		if (scale < 0 || scale > PPS_MAX_PRECISION)
			elog(ERROR, "unrecognised scale %d for pps_bounded_accum", scale);

		st = pps_make_state(aggcontext, scale);
	}
	else
		st = (NasAggState *) PG_GETARG_POINTER(0);

	if (PG_ARGISNULL(1))
		PG_RETURN_POINTER(st);

	arg = (struct varlena *) PG_GETARG_POINTER(1);
	detoasted = pg_detoast_datum_packed(arg);
	c = (union NumericChoice *) VARDATA_ANY(detoasted);

	/*
	 * Special values are counted, not rejected: 'NaN' in a numeric(18,2)
	 * column is perfectly legal, and both sum() and avg() have to return NaN
	 * rather than fail.
	 */
	if (unlikely(PPS_IS_SPECIAL(c)))
	{
		if (PPS_IS_NAN(c))
			st->NaNcount++;
		else if (PPS_IS_PINF(c))
			st->pInfcount++;
		else if (PPS_IS_NINF(c))
			st->nInfcount++;
		else
			elog(ERROR, "unrecognised numeric special value: 0x%04X",
				 c->n_header);
	}
	else
	{
		int			datalen = (int) VARSIZE_ANY_EXHDR(detoasted);
		INT128		m;

		/*
		 * Two separate things can go wrong here, and they are caught in two
		 * separate places.  A value wider than the declaration -- a broken
		 * typmod promise -- is caught when the mantissa is extracted, below,
		 * that is before it can corrupt the sum.  Overflow of the accumulator
		 * itself is caught here, before the addition rather than after it; see
		 * PPS_MAX_ADDENDS for why it is worth testing at all.
		 */
		pps_check_addends(st->N, 1);

		if (likely(pps_get_bounded_int128(c, datalen, st->scale, &m)))
			pps_int128_add_int128(&st->sumX, m);
		else
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("value does not fit the declared numeric scale %d",
							st->scale),
					 errdetail("The aggregate was specialised on the argument's "
							   "declared type; a row source supplied a value "
							   "outside that declaration.")));

		st->N++;
	}

	/*
	 * A copy appears only if the source handed us a compressed or external
	 * numeric, which cannot happen while the contract holds.  Free it anyway:
	 * the per-tuple context would clean it up, but relying on that inside the
	 * very code that handles a broken contract seems unwise.
	 */
	if (detoasted != arg)
		pfree(detoasted);

	PG_RETURN_POINTER(st);
}

/*
 * How a product of two numerics ends up, when at least one side is special.
 */
typedef enum NasMulSpecial
{
	PPS_MUL_ORDINARY,
	PPS_MUL_NAN,
	PPS_MUL_PINF,
	PPS_MUL_NINF
} NasMulSpecial;

/*
 * pps_operand_sign
 *		-1, 0 or 1 for a value that is known not to be special.
 */
static inline int
pps_operand_sign(union NumericChoice *c, int datalen)
{
	if (PPS_NDIGITS(c, datalen) <= 0)
		return 0;
	return PPS_IS_NEG(c) ? -1 : 1;
}

/*
 * pps_mul_special
 *		What numeric_mul() would have produced, when a special is involved.
 *
 * Reproduces the rules at the top of numeric_mul_safe(): NaN wins over
 * everything, an infinity times zero is NaN, and otherwise an infinity takes
 * the sign of the product.  We have to reproduce them rather than lean on
 * numeric_mul() because the whole point is that no numeric_mul() call happens.
 *
 * Reachable in practice only through a direct call: a numeric(p,s) column
 * cannot hold an infinity -- apply_typmod() refuses -- and NaN, which it can
 * hold, is the easy case.
 */
static NasMulSpecial
pps_mul_special(union NumericChoice *ca, int la,
				union NumericChoice *cb, int lb)
{
	bool		sa = PPS_IS_SPECIAL(ca);
	bool		sb = PPS_IS_SPECIAL(cb);
	int			sign;

	if (!sa && !sb)
		return PPS_MUL_ORDINARY;

	if ((sa && PPS_IS_NAN(ca)) || (sb && PPS_IS_NAN(cb)))
		return PPS_MUL_NAN;

	/* everything left has to be an infinity; anything else is a broken datum */
	if (sa && !PPS_IS_PINF(ca) && !PPS_IS_NINF(ca))
		elog(ERROR, "unrecognised numeric special value: 0x%04X", ca->n_header);
	if (sb && !PPS_IS_PINF(cb) && !PPS_IS_NINF(cb))
		elog(ERROR, "unrecognised numeric special value: 0x%04X", cb->n_header);

	if (sa && sb)
		return (PPS_IS_NINF(ca) != PPS_IS_NINF(cb)) ? PPS_MUL_NINF : PPS_MUL_PINF;

	if (sa)
	{
		sign = pps_operand_sign(cb, lb);
		if (sign == 0)
			return PPS_MUL_NAN;		/* Inf * 0 */
		if (PPS_IS_NINF(ca))
			sign = -sign;
	}
	else
	{
		sign = pps_operand_sign(ca, la);
		if (sign == 0)
			return PPS_MUL_NAN;
		if (PPS_IS_NINF(cb))
			sign = -sign;
	}

	return (sign > 0) ? PPS_MUL_PINF : PPS_MUL_NINF;
}

/*
 * pps_bounded_accum_mul
 *		Transition function for a folded product:
 *		(internal, numeric, numeric, int4, int4) -> internal.
 *
 * This is where sum(a * b) stops paying for the multiplication.  Left alone,
 * every row calls numeric_mul(), which allocates a digit buffer and a packed
 * result, and the transition function then unpacks that result again.  On the
 * machine these were written on that was about 68 ns a row -- roughly four
 * times what the accumulation itself costs.
 *
 * Here the two mantissas are extracted straight from the packed operands and
 * the product is accumulated by int128_add_int64_mul_int64(), the same helper
 * numeric.c uses for exactly this shape.  No numeric is built at any point, so
 * there is also no question of who owns it or how long it lives -- which is
 * the reason this is folded into the aggregate rather than done by
 * substituting a cheaper numeric_mul().  A substituted operator would still
 * have to return a real Datum, and the executor is entitled to keep it.
 *
 * The scales of the two operands arrive as constants planted by the support
 * function; the state's scale is their sum, which is exactly the dscale
 * numeric_mul() would have given the product.
 */
Datum
pps_bounded_accum_mul(PG_FUNCTION_ARGS)
{
	NasAggState *st;
	MemoryContext aggcontext;
	struct varlena *arga;
	struct varlena *argb;
	struct varlena *da;
	struct varlena *db;
	union NumericChoice *ca;
	union NumericChoice *cb;
	int			la;
	int			lb;
	INT128		wide;
	int64		ma;
	int64		mb;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "pps_bounded_accum_mul called in non-aggregate context");

	if (PG_ARGISNULL(0))
	{
		int32		s1;
		int32		s2;

		if (PG_ARGISNULL(3) || PG_ARGISNULL(4))
			elog(ERROR, "scale arguments of pps_bounded_accum_mul must not be null");

		s1 = PG_GETARG_INT32(3);
		s2 = PG_GETARG_INT32(4);
		if (s1 < 0 || s2 < 0 || s1 + s2 > PPS_MAX_PRECISION)
			elog(ERROR, "unrecognised scales %d and %d for pps_bounded_accum_mul",
				 s1, s2);

		st = pps_make_state(aggcontext, s1 + s2);
	}
	else
		st = (NasAggState *) PG_GETARG_POINTER(0);

	/* a NULL on either side makes the product NULL, which sum() skips */
	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		PG_RETURN_POINTER(st);

	arga = (struct varlena *) PG_GETARG_POINTER(1);
	argb = (struct varlena *) PG_GETARG_POINTER(2);
	da = pg_detoast_datum_packed(arga);
	db = pg_detoast_datum_packed(argb);
	ca = (union NumericChoice *) VARDATA_ANY(da);
	cb = (union NumericChoice *) VARDATA_ANY(db);
	la = (int) VARSIZE_ANY_EXHDR(da);
	lb = (int) VARSIZE_ANY_EXHDR(db);

	switch (pps_mul_special(ca, la, cb, lb))
	{
		case PPS_MUL_NAN:
			st->NaNcount++;
			goto done;
		case PPS_MUL_PINF:
			st->pInfcount++;
			goto done;
		case PPS_MUL_NINF:
			st->nInfcount++;
			goto done;
		case PPS_MUL_ORDINARY:
			break;
	}

	/*
	 * Each mantissa has to come out in an int64.  pps_get_bounded_int128() only
	 * promises PPS_MAX_PRECISION digits, so the narrower bound is checked here
	 * rather than assumed: the support function does guarantee it, but a
	 * direct call to this aggregate does not, and silently truncating a
	 * mantissa would produce a wrong sum with nothing to show for it.
	 */
	if (unlikely(!pps_get_bounded_int128(ca, la, PG_GETARG_INT32(3), &wide) ||
				 !pps_int128_fits_int64(wide, &ma) ||
				 !pps_get_bounded_int128(cb, lb, PG_GETARG_INT32(4), &wide) ||
				 !pps_int128_fits_int64(wide, &mb)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("value does not fit the declared numeric scale"),
				 errdetail("The aggregate was specialised on the declared types "
						   "of the factors; a row source supplied a value "
						   "outside that declaration.")));

	/*
	 * The product obeys the same bound as a plain mantissa, one step removed:
	 * the support function only fires when p1 + p2 <= PPS_MAX_PRECISION, so
	 * |ma * mb| < 10^28 and PPS_MAX_ADDENDS applies unchanged.  Tested before
	 * the addition, exactly as in pps_bounded_accum().
	 */
	pps_check_addends(st->N, 1);

	int128_add_int64_mul_int64(&st->sumX, ma, mb);
	st->N++;

done:
	if (da != arga)
		pfree(da);
	if (db != argb)
		pfree(db);

	PG_RETURN_POINTER(st);
}

/*
 * pps_bounded_combine
 *		Merge two states during parallel aggregation.
 *
 * Without it the planner does not build a Partial Aggregate at all and the
 * query loses parallelism entirely -- measured as a fourfold regression, so
 * this function is a requirement rather than a decoration.
 */
Datum
pps_bounded_combine(PG_FUNCTION_ARGS)
{
	NasAggState *st1;
	NasAggState *st2;
	MemoryContext aggcontext;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "pps_bounded_combine called in non-aggregate context");

	st1 = PG_ARGISNULL(0) ? NULL : (NasAggState *) PG_GETARG_POINTER(0);
	st2 = PG_ARGISNULL(1) ? NULL : (NasAggState *) PG_GETARG_POINTER(1);

	if (st2 == NULL)
	{
		if (st1 == NULL)
			PG_RETURN_NULL();
		PG_RETURN_POINTER(st1);
	}

	if (st1 == NULL)
	{
		st1 = pps_make_state(aggcontext, st2->scale);
		st1->sumX = st2->sumX;
		st1->N = st2->N;
		st1->NaNcount = st2->NaNcount;
		st1->pInfcount = st2->pInfcount;
		st1->nInfcount = st2->nInfcount;
		PG_RETURN_POINTER(st1);
	}

	/*
	 * Both states come from the same Aggref, so the scale is the same.  We
	 * check because a mismatch would mean the state was corrupted in transit,
	 * and adding mantissas of different scales yields a wrong sum with nothing
	 * to show for it.
	 */
	if (unlikely(st1->scale != st2->scale))
		elog(ERROR, "mismatched scales in pps_bounded_combine: %d vs %d",
			 st1->scale, st2->scale);

	/*
	 * The test in the transition functions bounds each partial state on its
	 * own; it says nothing about their sum.  Without this the parallel plan
	 * would drive straight past the limit that the serial plan stops at, and
	 * the same query would be safe or not depending on whether the planner
	 * chose to parallelise it.
	 */
	pps_check_addends(st1->N, st2->N);

	pps_int128_add_int128(&st1->sumX, st2->sumX);
	st1->N += st2->N;
	st1->NaNcount += st2->NaNcount;
	st1->pInfcount += st2->pInfcount;
	st1->nInfcount += st2->nInfcount;

	PG_RETURN_POINTER(st1);
}

/*
 * pps_bounded_serialize / pps_bounded_deserialize
 *		Ship the state from a worker to the leader.
 *
 * The format is fixed: the scale, four counters, and the sum in two halves.
 * The state only ever crosses a worker-to-leader boundary within one binary,
 * so portability of the format is not required -- but the field order is still
 * spelled out so that the two functions cannot be desynchronised by editing
 * one of them.
 */
Datum
pps_bounded_serialize(PG_FUNCTION_ARGS)
{
	NasAggState *st;
	StringInfoData buf;
	bytea	   *result;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "pps_bounded_serialize called in non-aggregate context");

	st = (NasAggState *) PG_GETARG_POINTER(0);

	pq_begintypsend(&buf);
	pq_sendint32(&buf, st->scale);
	pq_sendint64(&buf, st->N);
	pq_sendint64(&buf, st->NaNcount);
	pq_sendint64(&buf, st->pInfcount);
	pq_sendint64(&buf, st->nInfcount);
	pq_sendint64(&buf, pps_int128_hi_int64(st->sumX));
	pq_sendint64(&buf, (int64) pps_int128_lo_uint64(st->sumX));

	result = pq_endtypsend(&buf);
	PG_RETURN_BYTEA_P(result);
}

Datum
pps_bounded_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate;
	NasAggState *st;
	MemoryContext aggcontext;
	StringInfoData buf;
	int32		scale;
	int64		hi;
	uint64		lo;

	if (!AggCheckCallContext(fcinfo, &aggcontext))
		elog(ERROR, "pps_bounded_deserialize called in non-aggregate context");

	sstate = PG_GETARG_BYTEA_PP(0);

	/*
	 * initReadOnlyStringInfo() does not copy, so buf points straight into
	 * sstate.  The scale is read and validated before the state is created, so
	 * that a malformed message cannot leave a half-filled struct behind.
	 */
	initReadOnlyStringInfo(&buf, VARDATA_ANY(sstate),
						   VARSIZE_ANY_EXHDR(sstate));

	scale = pq_getmsgint(&buf, 4);
	if (scale < 0 || scale > PPS_MAX_PRECISION)
		elog(ERROR, "unrecognised scale %d in serialised aggregate state", scale);

	st = pps_make_state(aggcontext, scale);

	st->N = pq_getmsgint64(&buf);
	st->NaNcount = pq_getmsgint64(&buf);
	st->pInfcount = pq_getmsgint64(&buf);
	st->nInfcount = pq_getmsgint64(&buf);
	hi = pq_getmsgint64(&buf);
	lo = (uint64) pq_getmsgint64(&buf);
	st->sumX = pps_make_int128(hi, lo);

	/*
	 * The counts come from a worker running our own transition function, so
	 * none of these can be true -- but they arrive through a bytea that
	 * nothing else validates, and pps_bounded_combine() is about to do
	 * arithmetic on N that assumes the bound holds.  Checking here keeps that
	 * assumption honest rather than making it depend on the wire.
	 */
	if (unlikely(st->N < 0 || st->NaNcount < 0 ||
				 st->pInfcount < 0 || st->nInfcount < 0 ||
				 st->N > PPS_MAX_ADDENDS))
		elog(ERROR, "corrupted serialised aggregate state");

	pq_getmsgend(&buf);

	PG_RETURN_POINTER(st);
}

/*
 * pps_special_numeric
 *		Build NaN / Infinity / -Infinity.
 *
 * numeric.h exposes no constructor for the special values, so we go through
 * numeric_in().  The path is rare -- at most once per group, and only when the
 * group contained a special value at all -- so the cost of parsing text does
 * not matter here.
 */
static Datum
pps_special_numeric(const char *str)
{
	return DirectFunctionCall3(numeric_in, CStringGetDatum(str),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/*
 * pps_int128_to_numeric
 *		Build a numeric out of the 128-bit sum of mantissas and the scale.
 *
 * We split it into three parts of eighteen decimal digits and add them back
 * together using numeric.  No text is involved on this path, and that is not
 * for elegance: above p = 18 the sum leaves int64 on ordinary data rather than
 * in a corner, so the final function runs as many times as there are groups,
 * and parsing a string would be a visible share of a high-cardinality query.
 *
 * Dividing by 10^18 has to be done as two divisions by 10^9, because
 * common/int128.h only offers division by an int32.  There the remainder takes
 * the sign of the dividend and the quotient truncates towards zero, so all
 * three parts share one sign and their sum reproduces the original value with
 * no fixups.  Nothing is ever negated, which matters: |INT128_MIN| is not
 * representable in a signed type.
 *
 * Bounds: |v| < 2^127 ~ 1.7e38, so after dividing by 10^36 less than 170 is
 * left and part[2] certainly fits in an int64.
 */
static Numeric
pps_int128_to_numeric(INT128 v, int scale)
{
	int64		part[3];
	Numeric		res;
	int			i;

	for (i = 0; i < 2; i++)
	{
		int32		r0;
		int32		r1;

		pps_int128_div_mod_int32(&v, 1000000000, &r0);
		pps_int128_div_mod_int32(&v, 1000000000, &r1);
		part[i] = (int64) r1 * INT64CONST(1000000000) + r0;
	}
	part[2] = int128_to_int64(v);

	/*
	 * The first term sets the dscale: its log10val2 is scale, while for the
	 * others it is negative, that is they are integers.  numeric_add takes the
	 * maximum, so the result's dscale is scale no matter which parts are
	 * non-zero.
	 */
	res = int64_div_fast_to_numeric(part[0], scale);

	if (part[1] != 0)
		res = pps_numeric_add(res,
							  int64_div_fast_to_numeric(part[1], scale - 18));
	if (part[2] != 0)
		res = pps_numeric_add(res,
							  int64_div_fast_to_numeric(part[2], scale - 36));

	return res;
}

/*
 * The three ways a final function can end.  Both of ours have the same
 * preamble -- an empty state and the special values -- and differ only in how
 * they turn the accumulated mantissas into an answer.
 */
typedef enum NasFinalCase
{
	PPS_FINAL_NULL,				/* no input rows at all */
	PPS_FINAL_SPECIAL,			/* NaN or an infinity, in *result */
	PPS_FINAL_ORDINARY			/* the caller has to do the arithmetic */
} NasFinalCase;

/*
 * pps_final_preamble
 *		The part both final functions share.
 *
 * The order of the tests reproduces numeric_sum() and numeric_avg() word for
 * word, and that is not a matter of style: a column of nothing but NaNs has to
 * yield NaN rather than NULL, which is why the special-value counters are
 * consulted before the count of ordinary values.
 *
 * *result is only set for PPS_FINAL_SPECIAL.
 */
static NasFinalCase
pps_final_preamble(NasAggState *st, Datum *result)
{
	if (st == NULL || PPS_TOTAL_COUNT(st) == 0)
		return PPS_FINAL_NULL;

	if (st->NaNcount > 0)
	{
		*result = pps_special_numeric("NaN");
		return PPS_FINAL_SPECIAL;
	}

	/* adding plus and minus infinity together gives NaN */
	if (st->pInfcount > 0 && st->nInfcount > 0)
	{
		*result = pps_special_numeric("NaN");
		return PPS_FINAL_SPECIAL;
	}
	if (st->pInfcount > 0)
	{
		*result = pps_special_numeric("Infinity");
		return PPS_FINAL_SPECIAL;
	}
	if (st->nInfcount > 0)
	{
		*result = pps_special_numeric("-Infinity");
		return PPS_FINAL_SPECIAL;
	}

	return PPS_FINAL_ORDINARY;
}

/*
 * pps_sum_numeric
 *		Turn the accumulated mantissas into the numeric sum.
 *
 * The result's dscale has to equal scale.  For sum() that is plain enough; for
 * avg() it matters through select_div_scale(), which derives the scale of a
 * quotient from the dscale of its operands, so getting it wrong here silently
 * changes how many digits show up in the output.
 */
static Numeric
pps_sum_numeric(NasAggState *st)
{
	int64		narrow;

	/*
	 * Fast path: the sum fits in an int64 and the result is built by a single
	 * call, with no additions.  For p <= 18 this is the usual case; above that
	 * it is not, which is why the general path avoids text as well.
	 */
	if (pps_int128_fits_int64(st->sumX, &narrow))
		return int64_div_fast_to_numeric(narrow, st->scale);

	return pps_int128_to_numeric(st->sumX, st->scale);
}

/*
 * pps_bounded_sum_final
 *		Final function for the sum: (internal) -> numeric.
 */
Datum
pps_bounded_sum_final(PG_FUNCTION_ARGS)
{
	NasAggState *st;
	Datum		result = (Datum) 0;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "pps_bounded_sum_final called in non-aggregate context");

	st = PG_ARGISNULL(0) ? NULL : (NasAggState *) PG_GETARG_POINTER(0);

	switch (pps_final_preamble(st, &result))
	{
		case PPS_FINAL_NULL:
			PG_RETURN_NULL();
		case PPS_FINAL_SPECIAL:
			PG_RETURN_DATUM(result);
		case PPS_FINAL_ORDINARY:
			PG_RETURN_NUMERIC(pps_sum_numeric(st));
	}

	pg_unreachable();
}

/*
 * pps_bounded_avg_final
 *		Final function for the average: (internal) -> numeric.
 *
 * This is numeric_avg() with our sum in place of theirs.  Everything that
 * decides the shape of the answer -- the special-value ordering, the count
 * being of ordinary values only, and the scale of the quotient, which
 * select_div_scale() derives from the operands -- is therefore reproduced
 * exactly, provided our sum has the same value and the same dscale as core's.
 * It does: both are the sum of the inputs at the inputs' common scale.
 *
 * So avg() costs one extra final function and nothing else.  The transition,
 * combine, serialize and deserialize functions are shared with sum(), just as
 * core shares numeric_avg_accum between the two.
 */
Datum
pps_bounded_avg_final(PG_FUNCTION_ARGS)
{
	NasAggState *st;
	Datum		result = (Datum) 0;
	Datum		sum_datum;
	Datum		n_datum;

	if (!AggCheckCallContext(fcinfo, NULL))
		elog(ERROR, "pps_bounded_avg_final called in non-aggregate context");

	st = PG_ARGISNULL(0) ? NULL : (NasAggState *) PG_GETARG_POINTER(0);

	switch (pps_final_preamble(st, &result))
	{
		case PPS_FINAL_NULL:
			PG_RETURN_NULL();
		case PPS_FINAL_SPECIAL:
			PG_RETURN_DATUM(result);
		case PPS_FINAL_ORDINARY:
			break;
	}

	/*
	 * N cannot be zero here: pps_final_preamble() has already returned for an
	 * empty state, and a group that had only special values was handled there
	 * too, so numeric_div() cannot see a zero divisor.
	 */
	Assert(st->N > 0);

	sum_datum = NumericGetDatum(pps_sum_numeric(st));
	n_datum = NumericGetDatum(int64_to_numeric(st->N));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sum_datum, n_datum));
}
