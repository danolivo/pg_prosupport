# pg_prosupport

Extra PostgreSQL query tree optimisations that might be done with the prosupport
machinery.

An extension, and — on one branch only — a PostgreSQL patch underneath it.
Which of the two entry points below the planner offers depends on the server,
and that is the only thing that differs between the branches; the rewrites
themselves are the same code either way.

**On PostgreSQL 19 and later there is nothing to patch.** Core calls an
aggregate's own `pg_proc.prosupport` function with a
`SupportRequestSimplifyAggref` while it simplifies the query tree (commit
`42473b3b31`, the one that turns `COUNT(1)` into `COUNT(*)`), and that is the
entry point used there: `pps_agg_support()` is attached to
`pg_catalog.sum(numeric)` and `pg_catalog.avg(numeric)` by
`pps_attach_support()`, which `CREATE EXTENSION` runs for you. Nothing has to
be preloaded — the library is `dlopen()`ed the first time the planner
resolves the support function. The attachment is a write to `pg_proc`, since
`ALTER FUNCTION ... SUPPORT` refuses an aggregate outright, so it comes with
`pps_detach_support()` — **which has to be run before `DROP EXTENSION`**; see
Removal below.

**On PostgreSQL 18 there is no such call, hence the patch.**
`patches/0001-Introduce-agg_simplify_hook-to-postgres-18.patch` adds
`agg_simplify_hook`, a single planner hook: the planner calls it, if set, for
every `Aggref` it meets while simplifying the query tree, and whatever
non-`NULL` node the hook returns takes the `Aggref`'s place — the same point
in planning, and the same shape of rewrite that 19 reaches through the
catalog. It is a plain global hook, in the same family as `planner_hook`,
`set_rel_pathlist_hook` and `join_search_hook`: no `pg_proc.prosupport`
write, no `pg_depend` bookkeeping, no per-aggregate attachment to get wrong —
and, on the other hand, a library that has to be preloaded, because a hook,
unlike a catalog entry, is not resolved on demand.

**This extension is where every rewrite reached through either entry point
lives** -- all of them, with nothing left for core to do unconditionally.
`pps_simplify_aggref()` in `pg_prosupport.c` decides, from `aggref->aggfnoid`
and its arguments alone, which aggregates it wants and leaves everything else
— the overwhelming majority of calls — alone. That includes the case that
would be the obvious candidate for a core-level shortcut, a plain
`sum(column)` whose typmod is known outright: it arrives here exactly the same
way `sum(a + b)` or `avg(column)` does, and is recognised the same way,
through `pps_derive_bounds()`.

`numeric_agg`/`const_agg` (`make check`) pass on master through the catalog
entry point, with no patch involved at all, and against a PG18 built with the
patch. PG18's own `make check` (231/231) is unaffected by the patch: the hook
is `NULL` — one predicted-not-taken branch per `Aggref` — on a server where
nothing has installed it.

Rewrites, where they live, and what governs them:

| rewrite | where | governed by |
|---|---|---|
| `sum(a)`/`avg(a)` — `a` typmod-derivable, or derivable only through arithmetic (`a+b`, `round(a*b,2)`, …) → `bounded_numeric_sum`/`bounded_numeric_avg` | `numeric_support.c`, `numeric_agg.c` | `pg_prosupport.bounded_numeric_agg` — **on** by default |
| `sum(c)` for a constant `c` → `c * NULLIF(count(*), 0)::numeric`; the aggregation disappears | `constagg.c` | `pg_prosupport.fold_const_sum` — **off** by default |

The two rewrites are independent: `pg_prosupport.c`'s
`pps_simplify_aggref()` is a fixed sequence of two calls, one per module,
each recognising its own aggregate shape and consulting only its own GUC --
not one nested inside the other, and not a single switch governing both.
`pg_prosupport.bounded_numeric_agg` only narrows which function accumulates
an aggregate that was going to run anyway -- a plan-shape-preserving change --
so it defaults to on. `pg_prosupport.fold_const_sum` removes the aggregate
from the plan outright, a bigger change of plan shape, so it defaults to off
and is opt-in. When both are on and a query is `sum()` over a numeric
constant -- the one shape either could take -- the constant fold is tried
first and wins, since eliminating the aggregation is strictly better than
merely narrowing it; with `fold_const_sum` off (the default), that same
query falls through to `bounded_numeric_agg` instead, one step less thorough
but still on.

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

Every rewrite this extension makes -- the plain-column and
arithmetic-expression forms of `sum()`/`avg()`, the product fold, and the
constant fold -- arrives through the same entry point, so what it takes to
get any of them is what it takes to get all of them. That differs by branch.

**PostgreSQL 19 and later:**

```sql
CREATE EXTENSION pg_prosupport;
```

That is the whole of it. The install script creates the aggregates and then
calls `pps_attach_support()`, which points the `pg_proc.prosupport` entry of
`pg_catalog.sum(numeric)` and `pg_catalog.avg(numeric)` at
`pps_agg_support()`; from then on the planner resolves that function on
demand and `dlopen()`s the library itself. Superuser only, because the
attachment writes to `pg_proc` — and because it does, `DROP EXTENSION` is no
longer unconditional; see Removal.

The extension is **not relocatable** and installs into a schema of its own,
`prosupport`, which `CREATE EXTENSION` creates for you;
`CREATE EXTENSION ... SCHEMA something_else` and
`ALTER EXTENSION ... SET SCHEMA` are both errors. That is deliberate.
`pps_attach_support()` and `pps_detach_support()` are plpgsql, and a plpgsql
body has to name `pps_agg_support()` in full: `@extschema@` is substituted
once, when the install script runs, so a later move of the extension would
leave those two naming a schema that no longer holds the function — or, worse,
one that holds a different function of the same name. Forbidding the move is
what makes the qualified name true for the life of the installation. Nothing
else needs the schema on `search_path`: the rewrites reach the aggregates by
OID, not by name. Add it if you want to call `pps_detach_support()` without
qualifying it.

One wrinkle worth knowing: `CREATE EXTENSION` creates that schema but records
no ownership of it, so `DROP EXTENSION` leaves it behind, empty. A later
`CREATE EXTENSION` reuses it. `install.sql` asserts this rather than wishing it
away — if a future release starts dropping the schema, that test is where it
shows up.

The install script ends with `GRANT USAGE ON SCHEMA prosupport TO PUBLIC` and
then revokes `EXECUTE` from `PUBLIC` on every function it created. Both halves
are load-bearing, and the line between them is not where you would guess:

- The **schema** must be usable by everyone. `LookupFuncName()`, which is how
  the rewrite finds the substitute aggregate, goes through
  `LookupExplicitNamespace()`, and that checks `ACL_USAGE` against the *calling*
  user. Without the grant an ordinary user's `sum(v)` fails in the planner with
  `permission denied for schema prosupport` — not a lost optimisation, a broken
  query.
- The **aggregates** must stay executable by everyone, for the same shape of
  reason one level down: `ExecInitAgg()` checks `ACL_EXECUTE` on
  `aggref->aggfnoid` against the calling user, and the planner put
  `bounded_numeric_sum` there behind that user's back. Revoke it and the same
  query fails with `permission denied for aggregate bounded_numeric_sum` — a
  refusal naming an object the user never wrote.
- Everything else is revoked. The transition, combine, serialize and final
  functions are checked against the aggregate's *owner*, not the caller, so
  taking `EXECUTE` away from `PUBLIC` costs a rewritten query nothing; and they
  all take `internal`, so nobody could have called them by hand regardless.
  `pps_set_support()` and its two wrappers are the ones where the revoke
  matters.

`install.sql` runs a plain, unprivileged role through a rewritten `sum()` to
keep that line where it is.

The attachment is per database, like the extension, and it is not carried by
`pg_dump` (catalog rows of `pg_catalog` objects never are). After a dump and
restore the extension is there and the attachment is not, which is what
`pps_attach_support()` is for by hand:

```sql
SELECT pps_attach_support();     -- idempotent; superuser
```

**PostgreSQL 18:** `CREATE EXTENSION` is not enough on its own, and neither
is loading the library: both steps are required. The hook is installed once,
by `_PG_init()`, when the shared library is loaded — and loading is the one
thing `CREATE EXTENSION` does *not* do. It creates `bounded_numeric_sum` and
the rest as ordinary catalog objects, but the library itself is only
`dlopen()`ed when one of its C functions is actually called, which none of
these are until an `Aggref` has already been rewritten to name one. For the
hook to ever run, load the library through one of the ways PostgreSQL offers
for that:

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
`bounded_numeric_sum` and its siblings — loading the library alone
installs the hook but creates no aggregates for it to substitute.

Check:

```sql
EXPLAIN (verbose, costs off) SELECT sum(amount), avg(amount) FROM docs;
--   Output: bounded_numeric_sum(amount, 2), bounded_numeric_avg(amount, 2)
```

Both are rewritten once the extension is loaded and its aggregates created;
with the library not loaded, or `pg_prosupport.bounded_numeric_agg` off,
`EXPLAIN` would show plain `sum(amount)`/`avg(amount)` instead. If one of the
two is left alone while the other is rewritten, that means its precision and
scale could not be derived -- `avg()` and `sum()` are declined independently,
by the same function, from the same typmod check:

```sql
SET client_min_messages = debug1;
EXPLAIN SELECT avg(some_untyped_expr) FROM docs;
-- DEBUG:  pg_prosupport: leaving avg() alone: could not derive a precision and
--         scale for the argument within the supported range
```

There is no equivalent of writing `pg_proc.prosupport` back to zero to detach
the extension in one session while it stays installed elsewhere — the hook,
once loaded, applies to every database in the cluster (or every session that
loads it, with `session_preload_libraries`/`LOAD`). `SET pg_prosupport.bounded_numeric_agg
= off` (see Disabling, below) is the per-session and per-database way to stop
that rewrite without touching how the library is loaded, and `DROP EXTENSION
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
rewrite. Both exercise the whole path end to end, so on PostgreSQL 18 the
server under test needs
`patches/0001-Introduce-agg_simplify_hook-to-postgres-18.patch` applied, not
just PostgreSQL 15 or later for numeric's typmod encoding; on 19 and later no
patch is involved and a stock server is enough. The `LOAD 'pg_prosupport';`
the two scripts start with is what 18 needs and what 19 ignores -- there the
extension is already reachable through `pg_proc` by then -- and the
`pps_detach_support()` they end with is the other way round.

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

`sum(numeric)` and `avg(numeric)` are both handled the same way, by the same
function, `pps_simplify_bounded_numeric_agg()`: whether the argument's
precision and scale can be read off directly — a column, a cast, a `CASE`, a
domain — or only by walking an *arithmetic expression* (`a + b`, `a * b`,
`round(a * b, 2)`, …, via `pps_derive_bounds()`'s recursion through
`OpExpr`/`FuncExpr`), there is one code path for both, and one GUC governing
both. A bare numeric constant is left alone here for `constagg.c`'s better
rewrite to have first refusal on (see above); everything else reaches this
function the same way regardless of whether the argument is a plain column
or an expression built out of several.

Those come either from a declared typmod or from arithmetic over declared
operands:

| argument | derived | rewritten? |
|---|---|---|
| `a` — `numeric(10,2)` | `(10,2)` | yes |
| `b` — `avg(b)`, `b` is `numeric(10,2)` | `(10,2)` | yes |
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
| `a * b` | `bounded_numeric_sum_mul(a, b, 2, 4)` | folded |
| `(a + 1) * b` | `bounded_numeric_sum_mul((a+1), b, 2, 4)` | factors may be expressions |
| `a * b * a` | `bounded_numeric_sum_mul((a*b), a, 6, 2)` | nests; the inner product is still `numeric_mul` |
| `a * b + a` | `bounded_numeric_sum((a*b)+a, 6)` | the product is not at the top |
| `d * e` — `d` is `numeric(20,2)` | `bounded_numeric_sum(d*e, 4)` | a factor wider than 18 digits |

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

For any of `sum()`/`avg()` over a plain column or over an arithmetic
expression -- all reached through `pg_prosupport.bounded_numeric_agg` --
`SET pg_prosupport.bounded_numeric_agg = off` is the immediate cure, before
going after the row source. If the error came from the constant fold instead
(`sum()` over a numeric literal, with `pg_prosupport.fold_const_sum` on),
`SET pg_prosupport.fold_const_sum = off` is the one to reach for.

## Disabling

Three levels, from soft to hard -- all of them scoped to the extension's own
rewrites (`sum()`/`avg()` over a plain column or an arithmetic expression,
the product fold, and the constant fold).

**Per session or per database** — GUCs, no superuser needed. Two switches,
each governing exactly one of `pps_simplify_aggref()`'s two calls (see the
table above) and neither one reaching into the other:

```sql
SET pg_prosupport.bounded_numeric_agg = off;    -- sum()/avg() over a column or an expression; on by default
SET pg_prosupport.fold_const_sum = off;         -- only sum() over a constant; off by default
ALTER DATABASE mydb SET pg_prosupport.bounded_numeric_agg = off;
```

Either of them flushes the plan cache in the session that runs it, so it takes
effect immediately there, including for already-saved prepared statements. Other
backends pick it up when they next set it themselves.

They are separate switches because the two rewrites fail in different ways --
the specialised aggregates can be wrong about a scale, whereas the constant
rewrite changes the expression tree of a query -- and because they default
differently: `bounded_numeric_agg` only narrows an accumulator, a
plan-shape-preserving change, so it is on by default; `fold_const_sum`
removes the aggregate from the plan outright, which is worth opting into
rather than discovering after the fact, so it defaults to off.

**Entirely, on 19 and later** — give the two aggregates back:

```sql
SELECT pps_detach_support();
```

The next plan uses the stock `sum()`/`avg()` again, in **every** backend, not
just the one that ran the detach — which took some doing, and is worth knowing
about if you ever write something similar.

Writing `sum(numeric)`'s `prosupport` does send a cluster-wide invalidation for
that `pg_proc` row, but no cached plan is subscribed to it:
`record_plan_function_dependency()` in `setrefs.c` skips every OID below
`FirstUnpinnedObjectId`, so a plan never records a dependency on a built-in
aggregate. A plan that carries the rewrite names `bounded_numeric_sum`
instead — an ordinary user-space OID, and *that* one is in the plan's
`invalItems`. So `pps_detach_support()` finishes with a no-op
`UPDATE pg_proc SET proname = proname` over the four `bounded_numeric_*`
aggregates, purely for the invalidation it emits. Without it a pooled
connection goes on using the rewrite indefinitely after a detach;
`specs/detach_propagates.spec` is the two-session test that says so.

The reverse does not work and is not attempted: a plan holding the stock
`sum(numeric)` has no dependency to invalidate, so `pps_attach_support()` only
reaches the session that runs it. That is what it needs to be — it runs from
the install script, where nothing has been planned yet, and later sessions plan
with the attachment already in place.

**Entirely, on 18** — remove `pg_prosupport` from `shared_preload_libraries`
(or `session_preload_libraries`) and restart/reconnect; a session that loaded
it with a bare `LOAD` stops having it the moment that session ends. Sessions
that already hold a generic plan built with a rewrite in it keep using that
plan until they replan; `DISCARD PLANS` or a reconnect settles that.

**Removal.** On 18, in any order and with no special care:

```sql
DROP EXTENSION pg_prosupport;
```

On 19 and later, detach first:

```sql
SELECT pps_detach_support();
DROP EXTENSION pg_prosupport;
```

(Qualified, `SELECT prosupport.pps_detach_support();`, unless the
extension's schema is on your `search_path`.)

The order is not advice, it is enforced. `pps_attach_support()` records a
`pg_depend` entry from `sum(numeric)` to `pps_agg_support()`, so a `DROP
EXTENSION` that skips the detach fails -- with
`cannot drop function sum(numeric) because it is required by the database
system`, since `sum(numeric)` is pinned and cannot be dropped along with us.
That message is unhelpful but the behaviour behind it is the point: without
the dependency the drop would succeed and leave `sum(numeric)` pointing at an
OID that no longer exists, and every query using `sum(numeric)` in that
database would then fail in the planner with `cache lookup failed for
function NNNN`. A refused `DROP EXTENSION` is a much better failure than a
database that can no longer add up a column.

## Licence

MIT. Copyright (c) 2026 Andrei Lepikhov; see `LICENSE`.
