// MAPLECAST_AUTOSELECT — see header. Deterministic MvC2 char-select driver.
#include "maplecast_autoselect.h"
#include "hw/sh4/sh4_mem.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <array>

namespace {

// ---- guest RAM helpers (same access as the king.html state reader) ----
static inline uint8_t rd8(uint32_t guest) { return ::mem_b[guest & 0x1FFFFFF]; }

// ---- char-select layout (retail DC / mvsc2), from the disasm spec ----
constexpr uint32_t GRID_TBL   = 0x8C161FEC;   // char_id = tbl[row*8+col], 0xFF=empty
constexpr uint32_t CUR_COL[2] = {0x8C28C414, 0x8C28C415}; // P1, P2
constexpr uint32_t CUR_ROW[2] = {0x8C28C416, 0x8C28C417};
constexpr uint32_t LOCK[2]    = {0x8C28C412, 0x8C28C413}; // 0x77 = that side fully done
constexpr uint32_t SLOT0[2]   = {0x8C268340, 0x8C2688E4}; // per-player state struct; +0x05 = grid/color state
constexpr uint32_t IN_MATCH   = 0x8C289624;               // nonzero once the round is live

// ---- DC_BTN bits (active-high), matching the movie feeder's kcode layout ----
constexpr uint16_t B_UP=0x0010, B_DOWN=0x0020, B_LEFT=0x0040, B_RIGHT=0x0080;
constexpr uint16_t B_CONFIRM=0x0004;   // LP — inside the game's 0x03F0 "any attack" confirm mask
constexpr uint16_t B_START=0x0008;     // START — title/menu advance + free-play 2P join
constexpr uint16_t NEUTRAL_LOW = 0xFFFF; // active-low: nothing pressed

// ---- parsed target teams ----
bool  g_parsed=false, g_active=false, g_observe=false;
int   g_team[2][3] = {{-1,-1,-1},{-1,-1,-1}};
uint16_t g_input[2] = {NEUTRAL_LOW, NEUTRAL_LOW};
uint64_t g_frame=0;                    // our own vblank counter (holdMoviePace ticks it)

// ---- diagnostics (MAPLECAST_AUTOSELECT_DEBUG=1) ----
struct DbgP { int lock, st, col, row, cellAtCur, pick, target, tc, tr, dir; };
DbgP g_dbgP[2] = {};
int  g_dbgOn = -1;          // -1 = unchecked, 0/1 = env value
bool g_gridDumped = false;
inline bool dbgOn(){ if(g_dbgOn<0){ const char* e=std::getenv("MAPLECAST_AUTOSELECT_DEBUG"); g_dbgOn=(e&&*e&&*e!='0')?1:0; } return g_dbgOn==1; }
// MENU-NAV: drive from a COLD BOOT (title) into the 2P-versus char-select by pulsing
// START on BOTH players (free-play join), so the whole reconstruction runs in one pass
// with NO savestate (SAVE_AT_FRAME anchors captured in the vblank ISR don't reload).
int  g_menuNavOn = -1;
bool g_everCS = false;      // latched true once char-select was reached; stops menu-nav after locking
inline bool menuNav(){ if(g_menuNavOn<0){ const char* e=std::getenv("MAPLECAST_AUTOSELECT_MENUNAV"); g_menuNavOn=(e&&*e&&*e!='0')?1:0; } return g_menuNavOn==1; }
constexpr uint64_t MENU_BOOTWAIT = 200;   // vblanks of neutral before the title accepts START

void parseEnv() {
    g_parsed=true;
    // OBSERVE mode: trace guest RAM while the HUMAN picks manually (never inject).
    // Calibrates the grid/cursor/lock offsets + reveals the real lock-byte progression.
    { const char* o=std::getenv("MAPLECAST_AUTOSELECT_OBSERVE"); g_observe=(o&&*o&&*o!='0'); }
    const char* e=std::getenv("MAPLECAST_AUTOSELECT");
    if((!e||!*e)){
        if(g_observe) printf("[autoselect] OBSERVE mode — tracing char-select RAM (no input injected)\n");
        return;
    }
    // format: "p1a,p1b,p1c;p2a,p2b,p2c"  (char_ids, dec or 0xHEX)
    int side=0, idx=0; const char* p=e;
    while(*p && side<2){
        while(*p==' ') ++p;
        char* end=nullptr; long v=std::strtol(p,&end,0);
        if(end==p){ ++p; continue; }
        if(idx<3) g_team[side][idx++]=(int)v;
        p=end;
        while(*p==' ') ++p;
        if(*p==';'){ side++; idx=0; ++p; }
        else if(*p==','){ ++p; }
    }
    if(g_team[0][0]>=0 && g_team[1][0]>=0){
        g_active=true;
        printf("[autoselect] target P1=%d,%d,%d  P2=%d,%d,%d\n",
               g_team[0][0],g_team[0][1],g_team[0][2],g_team[1][0],g_team[1][1],g_team[1][2]);
    }
}

// grid geometry: read the live table (no DC/NAOMI ambiguity — read what's really there)
inline uint8_t cell(int col,int row){ return rd8(GRID_TBL + (uint32_t)(row*8+col)); }
bool findCell(int charid,int&oc,int&orow){
    for(int r=0;r<8;r++)for(int c=0;c<8;c++) if(cell(c,r)==(uint8_t)charid){oc=c;orow=r;return true;}
    return false;
}
// model ONE d-pad edge: apply dir with wrap, skipping empty(0xFF)/taken cells, until valid.
// dir: 0=up 1=down 2=left 3=right. `taken` = this player's already-locked chars.
void step(int&col,int&row,int dir,const bool taken[0x3B]){
    for(int guard=0; guard<16; ++guard){
        if(dir==0) row=(row+7)&7; else if(dir==1) row=(row+1)&7;
        else if(dir==2) col=(col+7)&7; else col=(col+1)&7;
        uint8_t id=cell(col,row);
        if(id!=0xFF && !(id<0x3B && taken[id])) return; // landed on a valid, untaken cell
    }
}
// BFS over the 4 edges -> first d-pad direction toward the target cell (-1 if already there)
int firstMove(int col,int row,int tcol,int trow,const bool taken[0x3B]){
    if(col==tcol&&row==trow) return -1;
    bool seen[8][8]={{false}}; seen[row][col]=true;
    struct N{int c,r,first;};
    std::vector<N> q; q.push_back({col,row,-1});
    for(size_t h=0; h<q.size() && h<256; ++h){
        N cur=q[h];
        for(int d=0; d<4; ++d){
            int nc=cur.c, nr=cur.r; step(nc,nr,d,taken);
            if(seen[nr][nc]) continue; seen[nr][nc]=true;
            int f = cur.first<0 ? d : cur.first;
            if(nc==tcol&&nr==trow) return f;
            q.push_back({nc,nr,f});
        }
    }
    return -1;
}
uint16_t dirBtn(int d){ return d==0?B_UP : d==1?B_DOWN : d==2?B_LEFT : B_RIGHT; }
uint16_t press(uint16_t hi){ return (uint16_t)(~hi & 0xFFFF); } // active-high -> active-low kcode

} // namespace

namespace maplecast_autoselect {

bool active(){ if(!g_parsed) parseEnv(); return g_active || g_observe; }

// one-time 8x8 grid-table dump — confirms GRID_TBL holds real char_ids (not 0xFF/garbage).
void dumpGrid(){
    g_gridDumped=true;
    printf("[autoselect] grid @0x%08X (8x8 char_ids, FF=empty):\n", GRID_TBL);
    for(int r=0;r<8;r++){ printf("[autoselect]  r%d:",r);
        for(int c=0;c<8;c++) printf(" %02X", cell(c,r)); printf("\n"); }
    fflush(stdout);
}

// True only while a player is actively on the char-select GRID (st 1/2/3) and the
// round hasn't started. Gates the driver so the HUMAN can freely navigate menus —
// the picker only takes over once you're on the character grid.
bool atCharSelect(){
    if(rd8(IN_MATCH)!=0) return false;
    uint8_t s0=rd8(SLOT0[0]+0x05), s1=rd8(SLOT0[1]+0x05);
    return (s0>=1 && s0<=3) || (s1>=1 && s1<=3);
}

// called once per vblank (emulator.cpp). Runs the state machine off guest RAM,
// computes each player's input for this frame, and reports whether to hold the movie pace.
bool holdMoviePace(){
    if(!active()) return false;
    g_frame++;
    const bool trace = dbgOn() || g_observe;
    if(trace && !g_gridDumped) dumpGrid();

    // OBSERVE mode: trace only, inject nothing, let the human drive char-select.
    if(g_observe){
        if(trace && (g_frame%15)==0){
            for(int pl=0;pl<2;++pl){
                int rcol=rd8(CUR_COL[pl]), rrow=rd8(CUR_ROW[pl]);
                printf("[autoselect] OBS P%d lock=%02X st=%02X cur=(%d,%d) cell=%02X in_match=%u\n",
                       pl+1, rd8(LOCK[pl]), rd8(SLOT0[pl]+0x05), rcol, rrow,
                       cell(rcol&7,rrow&7), rd8(IN_MATCH));
            }
            // EXTRA probe: distinguish menu vs char-select. grid_valid = #cells with a
            // real char_id (0..0x3A) in the 8x8 table; at true char-select ~55. Dump the
            // per-player state-struct heads (which byte is the 'active' flag may differ by
            // mode) + a few known globals to see what the movie is actually navigating.
            if(trace && (g_frame%30)==0){
                int gv=0; for(int i=0;i<64;i++){ uint8_t id=rd8(GRID_TBL+i); if(id<0x3B) gv++; }
                printf("[autoselect] PROBE gridValid=%d msub=%02X inmatch=%02X rc=%02X | P1h=%02X %02X %02X %02X %02X %02X %02X %02X | P2h=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                       gv, rd8(0x8C289621), rd8(0x8C289624), rd8(0x8C28962B),
                       rd8(SLOT0[0]+0),rd8(SLOT0[0]+1),rd8(SLOT0[0]+2),rd8(SLOT0[0]+3),rd8(SLOT0[0]+4),rd8(SLOT0[0]+5),rd8(SLOT0[0]+6),rd8(SLOT0[0]+7),
                       rd8(SLOT0[1]+0),rd8(SLOT0[1]+1),rd8(SLOT0[1]+2),rd8(SLOT0[1]+3),rd8(SLOT0[1]+4),rd8(SLOT0[1]+5),rd8(SLOT0[1]+6),rd8(SLOT0[1]+7));
            }
            fflush(stdout);
        }
        return false;                                  // don't hold the movie pace while observing
    }

    // Hand off to the movie the instant the round goes live (however we got here).
    if(rd8(IN_MATCH)!=0){
        g_input[0]=g_input[1]=NEUTRAL_LOW;
        if(g_active){ g_active=false; printf("[autoselect] in_match -> hand off to movie @ frame 0\n"); fflush(stdout); }
        return false;
    }
    // Only DRIVE while on the char-select grid. In menus/title/VS-intro stay passive
    // (getInput returns false -> the HUMAN drives) but keep the match movie paused.
    if(!atCharSelect()){
        // Once we've reached char-select at least once, NEVER pulse START again:
        // after both teams lock, st drops to 0 (off-grid) and we'd otherwise mash
        // START through the VS intro (could pause the match). Just wait for in_match.
        if(menuNav() && !g_everCS){
            // Cold-boot menu nav: pulse START on BOTH players (title -> 2P versus
            // char-select via free-play join). press ~4 / rest ~20 vblanks = clean edges.
            if(g_frame < MENU_BOOTWAIT){ g_input[0]=g_input[1]=NEUTRAL_LOW; }
            else {
                bool pressF = (((g_frame - MENU_BOOTWAIT) % 24) < 4);
                g_input[0]=g_input[1]= pressF ? press(B_START) : NEUTRAL_LOW;
            }
            if(trace && (g_frame%60)==0){
                printf("[autoselect] menu-nav both-START f=%llu st1=%02X st2=%02X msub=%02X\n",
                       (unsigned long long)g_frame, rd8(SLOT0[0]+0x05), rd8(SLOT0[1]+0x05), rd8(0x8C289621)); fflush(stdout); }
            return true;
        }
        g_input[0]=g_input[1]=NEUTRAL_LOW;
        if(trace && (g_frame%60)==0){ printf("[autoselect] waiting for char-select (P1 st=%02X P2 st=%02X)\n", rd8(SLOT0[0]+0x05), rd8(SLOT0[1]+0x05)); fflush(stdout); }
        return true;
    }
    g_everCS = true;   // reached the char-select grid — latch so menu-nav never mashes START again

    // press 1 vblank, rest 2 vblanks -> clean edges (~20 actions/s), avoids DAS auto-repeat
    const bool pressFrame = (g_frame % 3)==0;

    uint8_t l0=rd8(LOCK[0]), l1=rd8(LOCK[1]);
    if(l0==0x77 && l1==0x77){
        // both teams locked. Neutral through load/VS/intro; release the pace at round start.
        g_input[0]=g_input[1]=NEUTRAL_LOW;
        if(rd8(IN_MATCH)!=0){ g_active=false; printf("[autoselect] both teams locked, in_match -> hand off to movie @ frame 0\n"); fflush(stdout); return false; }
        if(trace && (g_frame%30)==0){ printf("[autoselect] both locked, waiting in_match (=%u)\n", rd8(IN_MATCH)); fflush(stdout); }
        return true;                                   // still in intro; keep the match movie paused
    }

    for(int pl=0; pl<2; ++pl){
        uint8_t lock=rd8(LOCK[pl]);
        uint8_t st=rd8(SLOT0[pl]+0x05);                // 1=grid nav, 2=color, 3=advance
        int col=rd8(CUR_COL[pl])&7, row=rd8(CUR_ROW[pl])&7;  // mask -> in-bounds for the 8x8 arrays
        DbgP& D=g_dbgP[pl];
        D.lock=lock; D.st=st; D.col=col; D.row=row; D.cellAtCur=cell(col,row);
        D.pick=-1; D.target=-1; D.tc=-1; D.tr=-1; D.dir=-9;

        if(lock==0x77){ g_input[pl]=NEUTRAL_LOW; }
        else if(!pressFrame){ g_input[pl]=NEUTRAL_LOW; }        // rest frame -> release the edge
        else if(st==2){ g_input[pl]=press(B_CONFIRM); D.dir=-2; } // color select -> accept palette
        else if(st!=1){ g_input[pl]=NEUTRAL_LOW; }             // transient
        else {
            // grid nav: which pick? = number of chars already locked (bits 0..2)
            int i = (lock&1)+((lock>>1)&1)+((lock>>2)&1); D.pick=i;
            if(i>2){ g_input[pl]=NEUTRAL_LOW; }
            else {
                int target=g_team[pl][i]; D.target=target;
                bool taken[0x3B]={false};
                for(int k=0;k<i;k++) if(g_team[pl][k]>=0 && g_team[pl][k]<0x3B) taken[g_team[pl][k]]=true;
                int tc,tr;
                if(!findCell(target,tc,tr)){ g_input[pl]=NEUTRAL_LOW; D.dir=-8; } // unknown char
                else{
                    D.tc=tc; D.tr=tr;
                    if(col==tc && row==tr){ g_input[pl]=press(B_CONFIRM); D.dir=-1; } // on target -> lock
                    else{ int d=firstMove(col,row,tc,tr,taken); D.dir=d;
                          g_input[pl]= d<0 ? press(B_CONFIRM) : press(dirBtn(d)); }
                }
            }
        }
    }
    if(trace && (g_frame%30)==0){
        for(int pl=0;pl<2;++pl){ DbgP&D=g_dbgP[pl];
            printf("[autoselect] P%d lock=%02X st=%02X cur=(%d,%d) cell=%02X pick=%d target=%02X tgt=(%d,%d) dir=%d\n",
                   pl+1, D.lock&0xFF, D.st&0xFF, D.col, D.row, D.cellAtCur&0xFF, D.pick, D.target&0xFF, D.tc, D.tr, D.dir); }
        fflush(stdout);
    }
    return true;
}

bool getInput(int player, uint16_t& kcodeLow){
    if(!active() || g_observe || player<0 || player>1) return false;  // observe -> human input passes through
    if(rd8(IN_MATCH)!=0) return false;                                // in match -> movie drives
    // char-select -> picker drives; menus -> picker drives ONLY in menu-nav mode (else human)
    if(!atCharSelect() && !menuNav()) return false;
    kcodeLow = g_input[player];
    return true;
}

} // namespace maplecast_autoselect
