# pg_prosupport

Extra PostgreSQL query tree optimisations that might be done with the prosupport
machinery.

An aggregate call can be rewritten at plan time from an extension, on a stock
server. `pg_proc.prosupport` of an aggregate is an ordinary catalog field, and
`simplify_aggref()` hands whatever it points at a
`SupportRequestSimplifyAggref` — the same mechanism that turns `COUNT(1)` into
`COUNT(*)`. This extension is a home for rewrites reached that way. **No patched
server is required — from PostgreSQL 19.**

`simplify_aggref()`'s call to `SupportRequestSimplifyAggref` is itself new in
PostgreSQL 19 (commit `42473b3b31`). On PostgreSQL 18 the aggregate's
`prosupport` field exists and can be set exactly as described below, but the
planner never consults it, so nothing fires. `patches/` carries a small,
tested backport of just that hook — no COUNT(1)/COUNT(*) support function,
no other core behaviour change — for building a PG18 that pg_prosupport can
attach to; see `patches/pg18-support-request-simplify-aggref.patch` for how
to apply it. `numeric_agg`/`const_agg` (`make installcheck`) pass against a
PG18 built with that patch, and PG18's own `make check` (231/231) is
unaffected by it.

Two of them so far, both aimed at what generated SQL — 1C, in the workload this
came from — does to `sum(numeric)`:

| rewrite | where | switch |
|---|---|---|
| `sum(c)` for a constant `c` → `c * NULLIF(count(*), 0)::numeric`; the aggregation disappears | `constagg.c` | `pg_prosupport.fold_const_sum` |
| `sum(a)`, `avg(a)` whose argument has a provable precision and scale → an aggregate specialised on that scale | `numeric_support.c`, `numeric_agg.c` | `pg_prosupport.enabled` |

Grouped aggregation over a plain column: 42–52% off the time, 68–70% off the
hash-table memory, and the HashAgg spill point three times further out.

When the argument is a product, the multiplication is folded into the transition
function as well: `sum(a * b)` becomes a four-argument aggregate that takes both
mantissas out of the packed operands and accumulates the product directly, so no
intermediate `numeric` is built at all. Measured on my laptop, 2M rows, 50k
groups, one aggregate:

| | total | minus the scan |
|---|---|---|
| `sum(a)` core | 613 ms | 254 ms |
| `sum(a)` rewritten | 427 ms | **68 ms** |
| `sum(a*b)` core | 767 ms | 408 ms |
| `sum(a*b)` rewritten | 521 ms | **162 ms** (−60%) |
| `avg(a*b)` core | 776 ms | 417 ms |
| `avg(a*b)` rewritten | 549 ms | **190 ms** (−54%) |

Folding the multiplication in is what makes the difference on that shape: before
it, `sum(a * b)` gained about 8% end-to-end; it now gains about 32%.

Planning costs something per rewritten aggregate. So be careful on highload OLTP.

These are one machine's numbers and will not be maintained; `bench/` has the
schema, the data generator and the exact queries, so measure your own.

```sh
psql -f bench/schema.sql -f bench/data.sql          # or -v rows=500000
psql -f bench/queries.sql                            # rewritten
psql -v enabled=off -f bench/queries.sql             # stock
```

Attach the support function first.

## Attaching the support function

Everything hangs on `pg_proc.prosupport` of the built-in aggregate. There is no
DDL that writes it for an aggregate — `ALTER FUNCTION ... SUPPORT` refuses
aggregates outright, and `ALTER AGGREGATE` has no `SUPPORT` clause — so the
field is written directly, as superuser, once per database:

```sql
CREATE EXTENSION pg_prosupport;

UPDATE pg_proc SET prosupport = 'pps_agg_support'::regproc
 WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure,
               'pg_catalog.avg(numeric)'::regprocedure);
```

Detaching is the same statement with a zero:

```sql
UPDATE pg_proc SET prosupport = 0
 WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure,
               'pg_catalog.avg(numeric)'::regprocedure);
```

Check:

```sql
EXPLAIN (verbose, costs off) SELECT sum(amount), avg(amount) FROM docs;
--   Output: numeric_scaled_sum(amount, 2), numeric_scaled_avg(amount, 2)
--   Output: sum(amount), avg(amount)          <- not rewritten
```

Why it was not rewritten:

```sql
SET client_min_messages = debug1;
EXPLAIN SELECT sum(amount) FROM docs;
-- DEBUG:  pg_prosupport: leaving sum() alone: could not derive a precision and
--         scale for the argument within the supported range
```

**Read the next section before you run any of this.** Writing the catalog by
hand skips the bookkeeping that a DDL command would have done, and the
consequences are not theoretical.

### Caveats of writing prosupport by hand

**No dependency is recorded.** `pg_depend` gets nothing, so nothing stops you
from dropping the extension while `prosupport` still points at
`pps_agg_support`. Do that and every query using `sum(numeric)` in that database
stops planning:

```
ERROR:  cache lookup failed for function 26396
```

That is not a degraded plan, it is a hard failure on a very ordinary query, and
it lasts until someone sets `prosupport` back to zero with the statement above.
**Always detach before `DROP EXTENSION`.**

**An OID can be reused.** If the support function is dropped and its OID is
later handed to an unrelated function, the planner will call that function with
a `SupportRequestSimplifyAggref` pointer. Same rule, same reason: detach first.

**Saved plans do not notice.** A backend holding a generic plan built with a
rewrite in it keeps using it after the catalog write, because the plan no longer
mentions `sum(numeric)` for the invalidation machinery to match on. Existing
sessions need `DISCARD PLANS` or a reconnect. The GUCs below do flush the plan
cache and are the better switch when you need one in a hurry.

All three are properties of writing the catalog directly, not of the rewrites.
A core patch that teaches `ALTER FUNCTION` to accept `SUPPORT` on an aggregate —
which records the dependency — and makes `simplify_aggref()` record the plan's
dependency on the original aggregate would remove all three. The extension does
not need it, and does not assume it.

## Building

In-tree, as a contrib module:

```sh
cd contrib/pg_prosupport
make && make install
```

Or standalone against an installed server:

```sh
make USE_PGXS=1 PG_CONFIG=/path/to/bin/pg_config
make USE_PGXS=1 PG_CONFIG=/path/to/bin/pg_config install
```

Tests:

```sh
make check
make USE_PGXS=1 PG_CONFIG=/path/to/bin/pg_config installcheck PGPORT=5432
```

`numeric_agg` covers the scale specialisation, `const_agg` the constant rewrite.
Both attach the support function the way this README does, so they run on any
server from PostgreSQL 15 up.

## sum() over a constant

When the argument of `sum(numeric)` is a constant the aggregation is not
specialised, it is removed:

```
sum(c)  ->  c * NULLIF(count(*), 0)::numeric
```

`sum(c)` over `n > 0` rows is exactly `c * n`, and NULL over an empty input.
The `NULLIF` is what keeps that second case right: `count(*)` returns 0 there
and `c * 0` is 0, a plausible-looking wrong answer in the one case nobody looks
at. With it the rewrite needs to know nothing about the query around it — a
plain scalar aggregate, a `GROUP BY`, and a grouping set that produces an empty
group are all the same to it.

This is not a corner case in generated SQL. In twelve hours of two 1C workloads
there are 596 `sum()` calls over a literal, carrying about 1.35 billion
transition-function calls whose result was settled by the SQL text. The
statements holding them are 2.8% and 1.4% of all statements and 5.1% and 5.4% of
the execution time. They come from padding the measure columns of a `UNION ALL`
branch that has no value for them.

Where they end up is worse than the count suggests. One `HAVING` in that
workload is `sum((sum('0'))::numeric(27,2)) <> '0'` repeated 26 times in one
disjunction: 26 provably false disjuncts, each estimated as an independent
condition.

Four things about the shape of the replacement:

- it keeps an aggregate rather than folding to a bare constant, so a query whose
  only aggregate was `sum('0')` does not end up with none while
  `Query.hasAggs` still says it has one;
- `count(*)` is `int8inc`, against `numeric_avg_accum`, which allocates;
- all the rewritten aggregates in one query collapse into one. `ExecInitAgg()`
  matches `Aggref`s with `equal()`, so those 26 copies become 26 references to a
  single `count(*)` transition — and to the query's existing `count(*)` if it has
  one;
- the dscale comes out on its own. Numeric multiplication yields `s1 + s2` and
  the int8 conversion yields 0, so the product carries `dscale(c)`, which is what
  `sum()` produced, every addend having had the same dscale. `NaN`, `±Infinity`
  and a NULL constant need no special case either.

`FILTER` is carried over to the `count(*)`: it counts exactly the rows `sum()`
would have added, and a filter matching nothing leaves `count(*)` at 0, which the
`NULLIF` turns back into NULL. `DISTINCT` and `ORDER BY` are refused —
`sum(DISTINCT c)` is `c`, and the sort expressions live in the argument list that
the replacement does not have.

A `PREPARE` parameter counts as a constant whenever the plan is a custom one: it
is bound before constant folding runs, so `sum($1)` becomes that execution's
value times `count(*)`. A generic plan is built with no bound parameters, the
`Param` survives folding, and nothing fires. Both are right — a custom plan is
only ever used for the execution it was built for.

`avg(c)` is deliberately not handled. It is not `c`: `avg()` divides through
`numeric_div()`, and `select_div_scale()` gives the quotient more fractional
digits than `c` has, so the rewrite would have to reproduce the division rather
than fold it away. It falls through to the scale specialisation instead.

## When the scale specialisation applies

The aggregate must be `sum(numeric)` or `avg(numeric)` with no `DISTINCT`,
`ORDER BY`, `FILTER` or `VARIADIC`, and the argument's precision and scale must
come out at `1 <= p <= 28`, `0 <= s <= p`.

Those come either from a declared typmod or from arithmetic over declared
operands:

| argument | derived | rewritten |
|---|---|---|
| `a` — `numeric(10,2)` | `(10,2)` | yes |
| `a * b` — `(10,2)`, `(12,4)` | `(22,6)` | yes |
| `a + b`, `a - b` | `(13,4)` | yes |
| `-a` | `(10,2)` | yes |
| `a * b + a` | `(23,6)` | yes |
| `a * 1.2` | `(13,3)` | yes |
| `a * b::numeric(14,4)` | `(24,6)` | yes |
| `round(a * b, 2)`, `trunc(a * b, 2)` | `(19,2)` | yes |
| `a * c` — `c` is `numeric(20,10)` | `(30,12)` | no, `p > 28` |
| `a / b`, `a / 100` | — | no, the quotient's scale depends on the values |
| `round(a, n)` — `n` is not constant | — | no |
| `a * v` — `v` has no typmod | — | no |

A product sitting at the top of the argument gets one step more: instead of
being computed per row and then accumulated, it is taken apart and both factors
are handed to the aggregate, which multiplies them as integers. That needs each
factor to derive at 18 digits or fewer, since the two mantissas have to fit
int64s; the sum of the two precisions still has to stay within 28.

| argument | aggregate | why |
|---|---|---|
| `a * b` | `numeric_scaled_sum_mul(a, b, 2, 4)` | folded |
| `(a + 1) * b` | `numeric_scaled_sum_mul((a+1), b, 2, 4)` | factors may be expressions |
| `a * b * a` | `numeric_scaled_sum_mul((a*b), a, 6, 2)` | nests; the inner product is still `numeric_mul` |
| `a * b + a` | `numeric_scaled_sum((a*b)+a, 6)` | the product is not at the top |
| `d * e` — `d` is `numeric(20,2)` | `numeric_scaled_sum(d*e, 4)` | a factor wider than 18 digits |

The rules are the ones numeric itself guarantees: a product has scale `s1 + s2`
and at most `p1 + p2` digits, a sum or difference has scale `max(s1, s2)` and at
most `max(p1-s1, p2-s2) + 1 + max(s1, s2)`, and `round(x, n)` / `trunc(x, n)`
with a constant `n` have scale exactly `max(n, 0)` plus room for one carried
digit, since `round(9.99, 1)` is `10.0`. Constants carry no typmod, so their
digits are counted from the value.

Division is excluded on purpose, including division by a constant:
`select_div_scale()` derives the quotient's scale from the operand *values*, so
even `a / 100` has a scale that varies from row to row.

Recursion into the expression stops at depth 8, so a long left-deep chain such as
`a+b+c+d+e+f+g+h+i` is declined rather than walked.

`exprTypmod()` also yields a typmod for `CASE` and `COALESCE` when it is the same
in every arm, and for domains over `numeric(p,s)`; both are supported. It does
not apply to `PREPARE` parameters — their `paramtypmod` is -1 even when the type
was declared.

A derivation bug cannot produce a wrong answer: the transition function checks
every value against the scale it was specialised on and raises an error if it
does not fit.

Window usage (`sum(x) OVER (...)`) is untouched: that is a `WindowFunc`, not an
`Aggref`, and the support function is not consulted for it.

`p > 28` is left to the built-in aggregate: at `p = 36` the mantissa reaches
2^120 and only two addends fit in a 128-bit accumulator. Across the erp_v and
cherkizovo schemas the rewrite covers 97.8% and 88.1% of row-columns
respectively.

## When the typmod lies

A value can arrive from an FDW or out of a cast without honouring the declared
scale. The aggregate then raises an error rather than silently producing a wrong
sum:

```
ERROR:  value does not fit the declared numeric scale 2
DETAIL:  The aggregate was specialised on the argument's declared type;
         a row source supplied a value outside that declaration.
```

The cure is `SET pg_prosupport.enabled = off`, and that is the first thing to do
before going after the row source.

## Disabling

Three levels, from soft to hard.

**Per session or per database** — GUCs, no superuser needed:

```sql
SET pg_prosupport.enabled = off;                -- both rewrites, this session
SET pg_prosupport.fold_const_sum = off;         -- only sum() over a constant
ALTER DATABASE mydb SET pg_prosupport.enabled = off;
```

Either of them flushes the plan cache in the session that runs it, so it takes
effect immediately there, including for already-saved prepared statements. Other
backends pick it up when they next set it themselves.

They are separate switches because the two rewrites fail in different ways: the
specialised aggregates can be wrong about a scale, whereas the constant rewrite
changes the expression tree of a query.

**Entirely, leaving the extension installed** — detach, as in section 2.
Sessions that already hold a generic plan keep the rewrite until they replan;
`DISCARD PLANS` or a reconnect settles that.

**Removal.** The order matters, and nothing enforces it:

```sql
UPDATE pg_proc SET prosupport = 0
 WHERE oid IN ('pg_catalog.sum(numeric)'::regprocedure,
               'pg_catalog.avg(numeric)'::regprocedure);
DROP EXTENSION pg_prosupport;
```

Dropping the extension first leaves `prosupport` pointing at a function that no
longer exists, and every `sum(numeric)` in the database then fails to plan with
`cache lookup failed for function NNNNN`. The way out is the same `UPDATE`.

## Licence

MIT. Copyright (c) 2026 Andrei Lepikhov; see `LICENSE`.
