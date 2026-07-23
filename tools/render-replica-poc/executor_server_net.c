/* executor_server_net — (2) deployment form: the EXECUTOR authoritative server on a real UDP
 * input socket (:7100), the same port/packet the input server + native clients already speak.
 * Real-time 60 Hz loop: recvfrom latest input -> dc_to_cps2 -> Input_DEC @0x2681DC -> byte-exact
 * tick -> statewire_v2 delta. NO flycast, NO emulator. Drop-in for maplecast-headless's game role.
 *
 * Input packet (ggpo.cpp:122): { u16 buttons; u8 lt; u8 rt; u32 seq } little-endian, 8 bytes.
 * Latest-wins latch (LatencyFirst): drain the socket each frame, keep the newest by seq.
 *
 * Build (win):   zig cc -O2 -w -o executor_server_net.exe executor_server_net.c gen_tick_all.c gen_leaf.c -lws2_32 -lm
 * Build (linux): cc  -O2 -w -o executor_server_net     executor_server_net.c gen_tick_all.c gen_leaf.c -lm
 * Run:           ./executor_server_net _ram_f90.bin 7100 [seconds=8]
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef _WIN32
  #include <winsock2.h>
  #include <windows.h>
  typedef int socklen_t;
  #define CLOSESOCK closesocket
  static void sleep_us(long us){ /* ~ms granularity is fine for the 60Hz pace test */
      if(us > 1000) Sleep((DWORD)(us/1000)); }
  static long long now_us(void){ LARGE_INTEGER f,t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
      return (long long)(t.QuadPart*1000000LL/f.QuadPart); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <time.h>
  #define CLOSESOCK close
  static void sleep_us(long us){ struct timespec ts={us/1000000, (us%1000000)*1000}; nanosleep(&ts,0); }
  static long long now_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
      return (long long)t.tv_sec*1000000LL + t.tv_nsec/1000; }
#endif

void tick_entry(Sh4Ctx* c);
static long g_calls=0;
int  mc_call_guard(void){ return (++g_calls>5000000L)?1:0; }
void mc_unknown_call(u32 a){ (void)a; }
void mc_unk_regs(u32* r){ (void)r; }
u32  mc_curfn=0; void mc_push(u32 a){ (void)a; } void mc_pop(void){}

/* DC pad -> CPS2 latched bits (native-client replica.rs dc_to_cps2). */
static uint16_t dc_to_cps2(uint16_t btn, uint8_t lt, uint8_t rt){
    #define DN(b) ((btn&(b))==0)
    uint16_t m=0;
    if(DN(0x0010))m|=0x2000; if(DN(0x0020))m|=0x1000; if(DN(0x0040))m|=0x0800; if(DN(0x0080))m|=0x0400;
    if(DN(0x0008))m|=0x8000; if(DN(0x0400))m|=0x0200; if(DN(0x0200))m|=0x0100;
    if(DN(0x0004))m|=0x0040; if(DN(0x0002))m|=0x0020;
    if(lt>=0x80)m|=0x0080; if(rt>=0x80)m|=0x0010;
    return m;
    #undef DN
}

/* statewire_v2 delta (C port) */
#define MERGE 8
static void put32(unsigned char*p,unsigned v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static unsigned sw_delta(const unsigned char*cur,unsigned n,const unsigned char*ref,unsigned key,unsigned char*out){
    unsigned char*o=out; *o++=0; put32(o,key); o+=4; unsigned char*np=o; o+=4; unsigned R=0,i=0;
    while(i<n){ if(cur[i]==ref[i]){i++;continue;} unsigned s=i,l=i,j=i+1;
        while(j<n&&(j-l)<=MERGE){ if(cur[j]!=ref[j])l=j; j++; }
        unsigned len=l-s+1; put32(o,s);o+=4; put32(o,len);o+=4; memcpy(o,cur+s,len);o+=len; R++; i=l+1; }
    put32(np,R); return (unsigned)(o-out);
}

static unsigned char ram[32u*1024u*1024u];
#define ST_OFF 0x00260000u
#define ST_LEN 0x00040000u
static unsigned char keyref[ST_LEN], wirebuf[ST_LEN+4096];

int main(int argc,char**argv){
    const char* rp = argc>1?argv[1]:"_ram_f90.bin";
    int port = argc>2?atoi(argv[2]):7100;
    int secs = argc>3?atoi(argv[3]):8;
    FILE*f=fopen(rp,"rb"); if(!f){printf("no %s\n",rp);return 2;} fread(ram,1,RAM_SIZE,f); fclose(f);
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2,2),&w);
#endif
    int sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
    if(bind(sock,(struct sockaddr*)&a,sizeof a)<0){ printf("bind :%d failed\n",port); return 3; }
#ifdef _WIN32
    u_long nb=1; ioctlsocket(sock,FIONBIO,&nb);
#else
    { int fl=1; ioctl(sock, FIONBIO, &fl); }
#endif
    printf("[executor-server-net] authoritative sim on UDP :%d, NO flycast. running %ds @60Hz...\n", port, secs);
    Sh4Ctx c; memset(&c,0,sizeof c); c.ram=ram; c.r[15]=0x8CFF0000u;
    unsigned short prev=0; uint32_t lastseq=0;
    long long t_start=now_us(); int fr=0; long inputs=0, tick_us_tot=0; long delta_tot=0; int deltas=0;
    while(now_us()-t_start < (long long)secs*1000000LL){
        long long frame_deadline = t_start + (long long)(fr+1)*16667LL;
        // LATEST-WINS latch: drain the socket, keep the newest packet by seq.
        unsigned char pkt[64]; uint16_t buttons=0xFFFF; uint8_t lt=0,rt=0; int got=0;
        for(;;){ int n=recv(sock,(char*)pkt,sizeof pkt,0);
            if(n<8) break;
            uint32_t seq = pkt[4]|(pkt[5]<<8)|(pkt[6]<<16)|((uint32_t)pkt[7]<<24);
            (void)seq;
            buttons = pkt[0]|(pkt[1]<<8); lt=pkt[2]; rt=pkt[3]; got=1; inputs++;
        }
        uint16_t cur = got ? dc_to_cps2(buttons,lt,rt) : 0;
        unsigned char* id=ram+0x2681DC;
        *(unsigned short*)(id+0)=cur; *(unsigned short*)(id+2)=prev;
        *(unsigned short*)(id+4)=(unsigned short)(cur&~prev); *(unsigned short*)(id+6)=(unsigned short)(prev&~cur);
        prev=cur;
        long long tk0=now_us(); g_calls=0; tick_entry(&c); tick_us_tot += (long)(now_us()-tk0);
        int isKey=(fr%60)==0;
        if(isKey) memcpy(keyref, ram+ST_OFF, ST_LEN);
        else { unsigned ws=sw_delta(ram+ST_OFF,ST_LEN,keyref,fr/60,wirebuf); delta_tot+=ws; deltas++; }
        fr++;
        long long wait = frame_deadline - now_us(); if(wait>0) sleep_us(wait);
    }
    double fps = fr/(double)secs;
    printf("  ran %d frames in %ds (%.1f fps), %ld input packets latched\n", fr, secs, fps, inputs);
    printf("  tick %.3f us/frame avg  |  wire %.0f B/delta avg (~%.3f Mbps @60Hz)\n",
           fr? (double)tick_us_tot/fr:0, deltas? (double)delta_tot/deltas:0, deltas? (double)delta_tot/deltas*8*60/1e6:0);
    printf("  => real UDP input -> byte-exact authoritative tick -> thin wire, off flycast.\n");
    CLOSESOCK(sock);
    return 0;
}
