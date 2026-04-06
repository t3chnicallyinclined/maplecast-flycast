// ============================================================================
// UI-COMMON.MJS — Shared utilities and constants
// ============================================================================

export const MVC2_CHARS = [
  'Ryu','Zangief','Guile','Morrigan','Anakaris','Strider','Cyclops','Wolverine',
  'Psylocke','Iceman','Rogue','Captain America','Spider-Man','Hulk','Venom','Dr. Doom',
  'Tron Bonne','Jill','Hayato','Ruby Heart','SonSon','Amingo','Marrow','Cable',
  'Abyss I','Abyss II','Abyss III','Chun-Li','Megaman','Roll','Akuma','B.B. Hood',
  'Felicia','Charlie','Sakura','Dan','Cammy','Dhalsim','M. Bison','Ken',
  'Gambit','Juggernaut','Storm','Sabretooth','Magneto','Shuma-Gorath','War Machine','Silver Samurai',
  'Omega Red','Spiral','Colossus','Iron Man','Sentinel','Blackheart','Thanos','Jin'
];

export const AVATARS = [
  '&#x1F94A;','&#x26A1;','&#x1F525;','&#x1F480;','&#x1F451;','&#x2694;',
  '&#x1F47E;','&#x1F3AF;','&#x1F4A5;','&#x1F30A;','&#x2B50;','&#x1F40D;'
];

export const ANON_NAMES = [
  'WANDERER','DRIFTER','NOMAD','RONIN','GHOST','SHADOW','VAGRANT',
  'STRANGER','OUTLAW','ROGUE','EXILE','PHANTOM','UNKNOWN','NAMELESS','FACELESS'
];

export function charName(id) {
  return MVC2_CHARS[id] || '???';
}

export function escapeHtml(str) {
  return str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

export function avg(arr) {
  return arr.length > 0 ? arr.reduce((a, b) => a + b, 0) / arr.length : 0;
}
