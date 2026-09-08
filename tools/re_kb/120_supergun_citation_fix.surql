-- 120: SUPERGUN citation repair. The SUPERGUN netcode seed was applied as 118 and then RENUMBERED TO 119 by the
-- coordinator, because the ENGINE lane had independently written and applied its own 118 (118_receipt_runner_gate3.surql)
-- about fifteen minutes earlier. The file, its header and the references in docs/SUPERGUN-NETCODE.md were corrected at
-- rename time, but two note strings had ALREADY been written into the live graph and still cited seed 118 -- which now
-- resolves to the Gate 3 seed. That is a dangling citation of exactly the kind this graph exists to prevent, so it is
-- repaired here rather than left to a doc.
-- Also hardens finding:supergun_steam_send_buffering_cost_unknown with an explicit note, so that nobody reading the graph
-- later can mistake the CONFIRMED flag value for a CONFIRMED millisecond cost.
-- Lane numbering agreed 2026-09-04: SUPERGUN NETCODE takes EVEN numbers from 120 up (120, 122, 124...), the RECEIPT
-- RUNNER / ENGINE lane takes ODD (121, 123...).
-- Apply with tools/re_kb/apply_seed.py after the README export backup
-- (re_kb_data/_exports/re_kb_20260904-151946_pre120.surql). Idempotent. No apostrophes inside string literals.
-- Do NOT re-run 07_dedup_edges.surql afterwards.
USE NS re DB kb;

UPDATE finding:supergun_steam_p2p_send_flag_unknown SET note='RESOLVED 2026-09-04 by seed 119 (tools/re_kb/119_supergun_p2p_send_and_clock.surql; applied as 118 and renumbered to 119 the same day because the engine lane had already taken 118 for receipt_runner_gate3): eP2PSendType = 0 = k_EP2PSendUnreliable at all five ISteamNetworking SendP2PPacket call sites in the image. See finding:supergun_steam_p2p_send_flag_resolved. NOTE that this resolves only the FLAG VALUE; the millisecond cost of the buffering it selects is still open, see finding:supergun_steam_send_buffering_cost_unknown.';

UPDATE finding:supergun_ggpo_clock_resolution_unknown SET note='RESOLVED 2026-09-04 by seed 119 (tools/re_kb/119_supergun_p2p_send_and_clock.surql; applied as 118 and renumbered to 119 the same day because the engine lane had already taken 118 for receipt_runner_gate3): the clock is latched from QueryPerformanceCounter once per frame by FUN_1400388F0, so GGPO sees ~16.67 ms granularity. See finding:supergun_ggpo_clock_frame_quantised.';

UPDATE finding:supergun_steam_send_buffering_cost_unknown SET status='open', note='OPEN ON PURPOSE -- do not read this as settled. What is CONFIRMED (seed 119) is the FLAG: the shipped build passes eP2PSendType = 0 = k_EP2PSendUnreliable, the buffering variant, at every one of the five SendP2PPacket sites. What is NOT known is HOW MANY MILLISECONDS that buffering costs: the window lives inside steamclient, not in the game image, and Valve does not document it for the legacy ISteamNetworking path, so it cannot be read statically at any effort. Anyone quoting a number here is guessing. The commercially load-bearing claim -- that a SUPERGUN raw-UDP transport beats the shipped one -- rests on this quantity and is therefore still UNPROVEN; the flag finding makes it structurally plausible, not measured. Gate N4 in docs/SUPERGUN-NETCODE.md is the only thing that settles it, and its confounder must be policed: the GGPO endpoint _round_trip_time read (endpoint+0x2068) and the raw sg_probe UDP RTT must be taken SIMULTANEOUSLY and between the SAME machine pair, never a Steam match on one day compared with a probe on another.';
