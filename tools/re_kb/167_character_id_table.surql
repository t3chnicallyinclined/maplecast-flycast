-- 167_character_id_table.surql -- the MvC2 character id -> name table, read from the Steam
-- binary's OWN strings. 2026-09-05. Doc: supergun/docs/RECEIPT-RUNNER-LIVE-DEBUG.md
USE NS re DB kb;

UPSERT source:steam_roster_strings SET kind='binary', strength='code',
  ref='mvc-live-skins-quarters/d3dcap/replay/re_map/cache/steam_strings.json, addresses 0x14097D020..0x14097D3D8',
  note='The game ships its roster as literal strings of the form "<hex cid> <NAME>", one per character, in id order.';

UPSERT finding:character_id_table SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='MvC2 character id -> name, read from the game binary rather than inferred',
  statement='55 entries at 0x14097D020..0x14097D3D8, each literally "<hex cid> <NAME>". Full table: 0=RYU; 1=ZANGIEF; 2=GUILE; 3=MORRIGAN; 4=ANAKARIS; 5=STRIDER HIRYU; 6=CYCLOPS; 7=WOLVERINE; 8=PSYLOCKE; 9=ICE-MAN; 10=ROGUE; 11=CAPTAIN AMERICA; 12=SPIDER-MAN; 13=HULK; 14=VENOM; 15=Dr.DOOM; 17=JILL; 18=HAYATO; 22=MARROW; 23=CABLE; 24=LAST BOSS 1; 25=LAST BOSS 2; 26=LAST BOSS 3; 27=CHUN-LI; 28=ROCK-MAN; 29=ROLL; 30=GOUKI; 31=BULETTA; 32=FELICIA; 33=NASH; 34=SAKURA; 35=DAN; 36=CAMMY; 37=DHALSIM; 38=VEGA; 39=KEN; 40=GAMBIT; 41=JUGGERNAUT; 42=STORM; 43=SABRETOOTH; 44=MAGNETO; 45=SHUMAGORATH; 46=WAR-MACHINE; 47=SILVER-SAMURAI; 48=OMEGA-RED; 49=SPIRAL; 50=COLOSSUS; 51=IRON-MAN; 52=SENTINEL; 53=BLACK HEART; 54=THANOTH; 55=GIN-SAOTOME; 56=CAPTAIN COMMANDO; 57=HONE WOLVERINE; 58=KOBUN.',
  self_validating='FOUR of these were independently MEASURED on the live process earlier the same day by watching the bytes at fighter+0x6C0 change as a human picked characters: 0x2A STORM = 42, 0x2C MAGNETO = 44, 0x32 COLOSSUS = 50, 0x34 SENTINEL = 52. All four agree with the table, so the mapping is confirmed by two independent routes rather than assumed from a published roster order.',
  use='This is what turns "--roster 53,23,15,12,50,46" into "Blackheart/Doom/Colossus vs Cable/Spider-Man/War-Machine". Slot order is P1C1,P2C1,P1C2,P2C2,P1C3,P2C3 -- EVEN slots are P1 -- also confirmed live.',
  caveat='Ids 16, 19, 20, 21 are absent from the table; 24-26 are LAST BOSS 1/2/3. Do NOT assume the id space is dense.';

RELATE finding:character_id_table -> cites -> source:steam_roster_strings SET id='e_roster_src';
