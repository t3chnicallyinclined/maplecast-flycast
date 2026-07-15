/*
	MAPLECAST_GOLD build profile (docs/TDW-GOLD-STANDARD.md §2c).

	Purpose: an enforceable boundary between the PRODUCT wire (TDW gold set +
	legacy-client legs) and the RE/diagnostic LAB instrumentation (~150 env
	flags: CHARQ, Oracle probes, TILEDESC, WALKSNAP, SUBHASH, dumps, watches,
	HITDIFF, …). All lab code is env-gated OFF at runtime already; GOLD removes
	it from the binary entirely — smaller product, zero accidental-enable risk.

	Conversion pattern (adopt incrementally, family by family):

	    #include "maplecast_gold.h"
	    MC_LAB_ONLY(
	        // an entire diagnostic block: statics, env gate, probe, printf
	    )

	or for larger regions:

	    #if MC_LAB
	    ...diagnostic family...
	    #endif

	Rules:
	- NEVER wrap product wire paths (TDW encoder, legacy legs, SYNC, input,
	  audio) — only instrumentation whose absence cannot change wire bytes.
	- A converted family must leave behavior in the default (lab) build
	  byte-for-byte identical: guards compile to the existing code when
	  MAPLECAST_GOLD is OFF.
	- When a family is converted, list it in TDW-GOLD-STANDARD.md §2c.
*/
#pragma once

#ifndef MAPLECAST_GOLD_BUILD
#define MAPLECAST_GOLD_BUILD 0
#endif

#define MC_LAB (!MAPLECAST_GOLD_BUILD)

#if MAPLECAST_GOLD_BUILD
#define MC_LAB_ONLY(...)
#else
#define MC_LAB_ONLY(...) __VA_ARGS__
#endif
