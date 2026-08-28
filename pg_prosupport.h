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

/* pg_prosupport.c */
extern bool pps_enabled;
extern void pps_decline(Oid aggfnoid, const char *reason);

/* numeric_support.c */
extern void pps_syscache_reset(Datum arg, int cacheid, uint32 hashvalue);

/* constagg.c */
extern bool pps_fold_const_sum;
extern Node *pps_simplify_const_sum(Aggref *agg);

#endif							/* PG_PROSUPPORT_H */
