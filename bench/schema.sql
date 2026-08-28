--
-- pg_prosupport: benchmark schema
--
-- Usage:
--     psql -f bench/schema.sql -f bench/data.sql
--     psql -f bench/queries.sql
--
-- Nothing here depends on the extension; the same tables are used to measure
-- the stock aggregates and the substituted ones, switched with
-- pg_prosupport.enabled.
--

DROP TABLE IF EXISTS pps_bench;
DROP TABLE IF EXISTS pps_bench12;

--
-- One row per generated value, with several numeric shapes side by side so
-- that a single scan cost applies to all of them.
--
--   grp_lo   1 000 groups     -- the hash table fits anywhere
--   grp_hi   one row each     -- the hash table is the whole table
--   v        numeric(18,2)    -- mantissa fits int64, the common money shape
--   w        numeric(25,2)    -- mantissa needs int128
--   a, b                      -- operands for sum(a * b)
--
CREATE TABLE pps_bench (
	grp_lo	int,
	grp_hi	int,
	v		numeric(18,2),
	w		numeric(25,2),
	a		numeric(10,2),
	b		numeric(12,4)
);

--
-- Twelve numeric columns, for the "a report with a dozen sum()s" shape.  Both
-- widths are present so the narrow and wide mantissa paths can be compared on
-- the same number of aggregates.
--
CREATE TABLE pps_bench12 (
	grp		int,
	n01 numeric(18,2), n02 numeric(18,2), n03 numeric(18,2),
	n04 numeric(18,2), n05 numeric(18,2), n06 numeric(18,2),
	n07 numeric(18,2), n08 numeric(18,2), n09 numeric(18,2),
	n10 numeric(18,2), n11 numeric(18,2), n12 numeric(18,2),
	w01 numeric(25,2), w02 numeric(25,2), w03 numeric(25,2),
	w04 numeric(25,2), w05 numeric(25,2), w06 numeric(25,2),
	w07 numeric(25,2), w08 numeric(25,2), w09 numeric(25,2),
	w10 numeric(25,2), w11 numeric(25,2), w12 numeric(25,2)
);
