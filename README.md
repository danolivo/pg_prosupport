# pg_prosupport

Extra PostgreSQL query tree optimisations that might be done with the prosupport
machinery.

Two PostgreSQL patches, plus an extension that builds on top of the first of
them.

**Patch 2, `patches/pg18-numeric-scaled-sum-catalog.patch`, moves the single
most valuable case into core outright.** `sum(numeric)` over an argument
whose precision and scale are known from a plain typmod — a column, a cast,
a `CASE`, a domain — is substituted for `pg_catalog.numeric_scaled_sum(numeric,
int4)`, a real catalog aggregate, by a direct, unconditional function call —
`simplify_sum_numeric_aggref()` in `numeric.c`, called from
`eval_const_expressions_mutator()` — not by a hook a module has to install.
This needs **no extension and no `CREATE EXTENSION`** to work: it is core
behaviour on any database, the moment the patch is applied and the server
rebuilt. See that patch file's own header for the design and the numbers.

**Patch 1, `patches/pg18-agg-simplify-hook.patch`, adds `agg_simplify_hook`**,
a single planner hook: the planner calls it, if set, for every `Aggref` it
meets while simplifying the query tree, and whatever non-`NULL` node the hook
returns takes the `Aggref`'s place — the same point in planning, and the same
shape of rewrite, that PostgreSQL 19's `SupportRequestSimplifyAggref` reaches
through `pg_proc.prosupport` (commit `42473b3b31`, which turns `COUNT(1)`
into `COUNT(*)`). Unlike that catalog-driven dispatch, `agg_simplify_hook` is
a plain global hook — in the same family as `planner_hook`,
`set_rel_pathlist_hook` and `join_search_hook` — added to the planner on
purpose in favour of the catalog machinery: no `pg_proc.prosupport` write, no
`pg_depend` bookkeeping, no per-aggregate attachment to get wrong. It runs
*after* patch 2's direct call, only for whatever that one declined.

**This extension is a home for rewrites reached through that hook** — the
ones that are not (yet, or ever) unconditional enough to bake into core the
way patch 2's case was. Its own hook function decides, from
`aggref->aggfnoid` alone, which aggregates it wants and leaves everything
else — the overwhelming majority of calls — alone.

`numeric_agg`/`const_agg` (`make installcheck`) pass against a PG18 built
with both patches, and PG18's own `make check` (231/231) is unaffected by
either: patch 2's substitution is silent in `EXPLAIN` except for the
aggregate's name, and patch 1's hook is `NULL` — one predicted-not-taken
branch per `Aggref` — on a server where nothing has installed it.

Rewrites, where they live, and what governs them:

| rewrite | where | governed by |
|---|---|---|
| `sum(a)` — `a` typmod-derivable → `pg_catalog.numeric_scaled_sum(a, s)` | core, `numeric.c` (**patch 2**) | nothing; always on, no extension needed |
| `sum(c)` for a constant `c` → `c * NULLIF(count(*), 0)::numeric`; the aggregation disappears | `constagg.c` | `pg_prosupport.fold_const_sum` |
| `avg(a)` typmod-derivable, or `sum(a)`/`avg(a)` derivable only through arithmetic (`a+b`, `round(a*b,2)`, …) → `numeric_scaled_avg`/`numeric_scaled_sum_expr` | `numeric_support.c`, `numeric_agg.c` | `pg_prosupport.enabled` |

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

Load the extension first.

## Loading the extension

`sum(amount)` over a plain typmod-derivable argument needs none of this: it
is patch 2's doing, in core, whether or not `pg_prosupport` is even
installed in the database. Everything below is about the *rest* of the
rewrites — `avg()`, the arithmetic-expression and product-folded forms of
`sum()`/`avg()`, and the constant fold — which do go through
`agg_simplify_hook` and do need the extension loaded.

There is no catalog attachment step, unlike a `SupportRequestSimplify`-based
rewrite reached through `pg_proc.prosupport`: `agg_simplify_hook` is
installed once, by `_PG_init()`, when the shared library is loaded — and
that is the one thing `CREATE EXTENSION` on its own does *not* do. It
creates `numeric_scaled_sum_expr` and the rest as ordinary catalog objects,
but the library itself is only `dlopen()`ed when one of its C functions is
actually called, which none of these are until an `Aggref` has already been
rewritten to name one. For the hook to ever run, load the library through
one of the ways PostgreSQL offers for that:

```
# postgresql.conf, before the server starts — the normal way to run this
shared_preload_libraries = 'pg_prosupport'
```

```sql
-- or, mid-session, if the server was not started that way
LOAD 'pg_prosupport';
```

`session_preload_libraries` works the same way as `shared_preload_libraries`
but per-session rather than per-cluster. Either way, `CREATE EXTENSION
pg_prosupport;` is still needed once per database, to create
`numeric_scaled_sum_expr` and its siblings — loading the library alone
installs the hook but creates no aggregates for it to substitute.

Check:

```sql
EXPLAIN (verbose, costs off) SELECT sum(amount), avg(amount) FROM docs;
--   Output: numeric_scaled_sum(amount, 2), avg(amount)
```

`sum(amount)` is rewritten by core regardless of whether `pg_prosupport` is
loaded (patch 2); `avg(amount)`, here, was not, because the extension is not
loaded — with it loaded, this would read `numeric_scaled_avg(amount, 2)`
instead. Why an *extension* rewrite (never core's) was not applied:

```sql
SET client_min_messages = debug1;
EXPLAIN SELECT avg(amount) FROM docs;
-- DEBUG:  pg_prosupport: leaving avg() alone: could not derive a precision and
--         scale for the argument within the supported range
```

(`sum(amount)` would print no such message either way: patch 2's substitution
is unconditional and does not decline out loud — there is nothing to log
about a rewrite that is never wrong.)

There is no equivalent of writing `pg_proc.prosupport` back to zero to detach
the extension in one session while it stays installed elsewhere — the hook,
once loaded, applies to every database in the cluster (or every session that
loads it, with `session_preload_libraries`/`LOAD`). `SET pg_prosupport.enabled
= off` (see Disabling, below) is the per-session and per-database way to stop
the rewrites without touching how the library is loaded, and `DROP EXTENSION
pg_prosupport;` can be run at any time, in any order, without special care:
nothing outside `pg_depend`'s own bookkeeping points at the extension's
objects, since none of this is reached through a catalog field.

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

`numeric_agg` covers the scale specialisation, `const_agg` the constant
rewrite. Both `LOAD 'pg_prosupport';` the way this README does, so they
exercise `agg_simplify_hook` end to end -- which means the server under test
needs both `patches/pg18-agg-simplify-hook.patch` and
`patches/pg18-numeric-scaled-sum-catalog.patch` (or an equivalent for
whichever version you are building) applied, not just PostgreSQL 15 or later
for numeric's typmod encoding. Applying patch 2 adds new `pg_proc`/
`pg_aggregate` rows, so a data directory built before it cannot be reused --
`initdb` again after applying it.

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

Which of the two mechanisms catches a given call is not a free choice, and it
is worth being precise about the boundary. **Patch 2, in core**, handles
`sum(numeric)` alone, only when the sole argument's typmod can be read off
directly — a column, a cast, a `CASE`, a domain — and only when that argument
is not a bare constant (a constant is left for `constagg.c`'s better rewrite,
below, to have first refusal on). **The extension's hook** handles
everything patch 2 does not: `avg(numeric)` in every shape, `sum(numeric)`
whose argument is an *arithmetic expression* rather than a plain typmod
(`a + b`, `a * b`, `round(a * b, 2)`, …, via `pps_derive_bounds()`'s
recursion through `OpExpr`/`FuncExpr`), and the product-folded forms of both.
The two never compete for the same `Aggref`: core's call runs first and,
when it substitutes, the `Aggref` is already gone by the time
`agg_simplify_hook` would have seen it.

Those come either from a declared typmod or from arithmetic over declared
operands:

| argument | derived | rewritten by |
|---|---|---|
| `a` — `numeric(10,2)` | `(10,2)` | core (patch 2) |
| `b` — `avg(b)`, `b` is `numeric(10,2)` | `(10,2)` | extension |
| `a * b` — `(10,2)`, `(12,4)` | `(22,6)` | extension |
| `a + b`, `a - b` | `(13,4)` | extension |
| `-a` | `(10,2)` | extension |
| `a * b + a` | `(23,6)` | extension |
| `a * 1.2` | `(13,3)` | extension |
| `a * b::numeric(14,4)` | `(24,6)` | extension |
| `round(a * b, 2)`, `trunc(a * b, 2)` | `(19,2)` | extension |
| `a * c` — `c` is `numeric(20,10)` | `(30,12)` | no, `p > 28` |
| `a / b`, `a / 100` | — | no, the quotient's scale depends on the values |
| `round(a, n)` — `n` is not constant | — | no |
| `a * v` — `v` has no typmod | — | no |

A product sitting at the top of the argument gets one step more: instead of
being computed per row and then accumulated, it is taken apart and both factors
are handed to the aggregate, which multiplies them as integers. That needs each
factor to derive at 18 digits or fewer, since the two mantissas have to fit
int64s; the sum of the two precisions still has to stay within 28. This is
the extension's own rewrite in every row below — a product is never at the
top of a *plain* typmod argument, so core's direct call never enters into it.

| argument | aggregate | why |
|---|---|---|
| `a * b` | `numeric_scaled_sum_mul(a, b, 2, 4)` | folded |
| `(a + 1) * b` | `numeric_scaled_sum_mul((a+1), b, 2, 4)` | factors may be expressions |
| `a * b * a` | `numeric_scaled_sum_mul((a*b), a, 6, 2)` | nests; the inner product is still `numeric_mul` |
| `a * b + a` | `numeric_scaled_sum_expr((a*b)+a, 6)` | the product is not at the top |
| `d * e` — `d` is `numeric(20,2)` | `numeric_scaled_sum_expr(d*e, 4)` | a factor wider than 18 digits |

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

For `avg()`, or for `sum()` over an arithmetic expression -- the extension's
own rewrites -- `SET pg_prosupport.enabled = off` is the immediate cure,
before going after the row source.

For a plain `sum(column)`, there is no session-level switch: patch 2 put that
substitution in core, unconditionally, on the premise that it is never wrong
to do -- so this error means the premise was actually violated (a typmod that
does not hold), which is a data problem worth fixing at the source rather
than a rewrite worth turning off. The only way to stop core's substitution
itself is to not apply `pg_18-numeric-scaled-sum-catalog.patch` (or to revert
and rebuild without it), which needs a restart either way.

## Disabling

Three levels, from soft to hard -- all of them scoped to the extension's own
rewrites (`avg()`, `sum()`/`avg()` over an arithmetic expression, the product
fold, and the constant fold). `sum(column)` over a plain typmod is core's
doing (patch 2) and none of these switches reach it; see above.

**Per session or per database** — GUCs, no superuser needed:

```sql
SET pg_prosupport.enabled = off;                -- the extension's rewrites, this session
SET pg_prosupport.fold_const_sum = off;         -- only sum() over a constant
ALTER DATABASE mydb SET pg_prosupport.enabled = off;
```

Either of them flushes the plan cache in the session that runs it, so it takes
effect immediately there, including for already-saved prepared statements. Other
backends pick it up when they next set it themselves.

They are separate switches because the two rewrites fail in different ways: the
specialised aggregates can be wrong about a scale, whereas the constant rewrite
changes the expression tree of a query.

**Entirely** — remove `pg_prosupport` from `shared_preload_libraries` (or
`session_preload_libraries`) and restart/reconnect; a session that loaded it
with a bare `LOAD` stops having it the moment that session ends. Sessions
that already hold a generic plan built with a rewrite in it keep using that
plan until they replan; `DISCARD PLANS` or a reconnect settles that.

**Removal.** In any order, and with no special care:

```sql
DROP EXTENSION pg_prosupport;
```

There is nothing else pointing at its objects for a stale reference to break:
unlike a hand-written `pg_proc.prosupport`, everything here is ordinary
`pg_depend`-tracked catalog state, and the hook itself only ever runs while
the library is loaded in the first place.

## Licence

MIT. Copyright (c) 2026 Andrei Lepikhov; see `LICENSE`.
