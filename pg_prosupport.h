/*-------------------------------------------------------------------------
 *
 * pg_prosupport.h
 *	  Declarations shared between the extension's modules.
 *
 * Copyright (c) 2026, Andrei Lepikhov
 *
 * Released under the MIT licence; see the LICENSE file in this directory.
 *
 * IDENTIFICATION
 *	  contrib/pg_prosupport/pg_prosupport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROSUPPORT_H
#define PG_PROSUPPORT_H

#include "nodes/primnodes.h"

struct PlannerInfo;				/* avoid pulling in pathnodes.h here */

/* pg_prosupport.c */
extern void pps_decline(Oid aggfnoid, const char *reason);

/*
 * numeric_support.c
 *
 * pps_numeric_agg is this rewrite's own switch (GUC pg_prosupport.numeric_agg,
 * default on): the specialised sum()/avg() over a bounded numeric, including
 * the product fold.  pps_get_sum_numeric_oid() is the one piece of plumbing
 * this module shares with constagg.c -- which aggregate is pg_catalog.
 * sum(numeric) -- so that constagg.c does not have to keep a second,
 * independently invalidated cache of the same fact just to recognise its own
 * Aggref.
 */
extern bool pps_numeric_agg;
extern void pps_syscache_reset(Datum arg, int cacheid, uint32 hashvalue);
extern Node *pps_simplify_scaled_numeric_agg(struct PlannerInfo *root, Aggref *agg);
extern Oid	pps_get_sum_numeric_oid(void);

/*
 * constagg.c
 *
 * pps_fold_const_sum is this rewrite's own switch (GUC
 * pg_prosupport.fold_const_sum, default off -- see the comment above the
 * DefineCustomBoolVariable() call in pg_prosupport.c for why the default
 * differs from pps_numeric_agg's).
 */
extern bool pps_fold_const_sum;
extern Node *pps_simplify_const_sum(Aggref *agg);

#endif							/* PG_PROSUPPORT_H */
