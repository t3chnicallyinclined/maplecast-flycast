/*
	MapleCast Lookup Test — record 253B game state + TA commands,
	then replay using ONLY game state to look up TA templates.

	Phase 1 (RECORD): 600 frames, save (game_state_hash → TA commands) pairs
	Phase 2 (REPLAY): use game state hash to look up recorded TA commands
	                   render from lookup, not from live stream

	MAPLECAST_LOOKUP_TEST=1 to enable.
*/
#pragma once

struct rend_context;
struct TA_context;

namespace maplecast_lookup_test
{
void init();
bool active();

// Called from render path — records state+TA during record phase,
// looks up TA during replay phase
void serverRecord(TA_context* ctx);
void addToCache(TA_context* ctx);  // mirror client adds templates to cache
bool clientLookup(rend_context& rc);
bool isReplaying();
int cacheSize();
}
