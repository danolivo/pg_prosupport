# contrib/pg_prosupport/Makefile
#
# Copyright (c) 2026, Andrei Lepikhov
# Released under the MIT licence; see the LICENSE file in this directory.

MODULE_big = pg_prosupport
OBJS = \
	$(WIN32RES) \
	constagg.o \
	numeric_agg.o \
	numeric_support.o \
	pg_prosupport.o

EXTENSION = pg_prosupport
DATA = pg_prosupport--1.0.sql
PGFILEDESC = "pg_prosupport - plan-time aggregate rewrites through the prosupport machinery"

REGRESS = install numeric_agg const_agg

# The one thing sql/ cannot reach: whether a detach in one backend invalidates
# a generic plan held by another.  See specs/detach_propagates.spec.
ISOLATION = detach_propagates

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_prosupport
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
