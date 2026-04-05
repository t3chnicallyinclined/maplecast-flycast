// ============================================================================
// STUBS.CPP — Minimal stubs for standalone WASM mirror renderer
//
// Every symbol here is referenced by the flycast renderer files but NOT needed
// for mirror rendering. These are no-ops / sensible defaults.
//
// OVERKILL IS NECESSARY. Zero dead code in the hot path.
// ============================================================================

#include "types.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "emulator.h"
#include "cfg/option.h"
#include "oslib/storage.h"
#include "wsi/context.h"
#include "input/mouse.h"

#include <string>

// ============================================================================
// Data Globals
// ============================================================================

// Frame counter (used by texture cache cleanup)
u32 FrameCount = 1;

// Renderer pointer — set by wasm_bridge.cpp after creating OpenGLRenderer
Renderer* renderer = nullptr;

// Framebuffer write detection (not needed — server handles rendering)
u32 fb_watch_addr_start = 0;
u32 fb_watch_addr_end = 0;
bool fb_dirty = false;

// SPG frame skip (not needed — we render every frame from the server)
u32 fskip = 0;
bool SH4FastEnough = true;

// Holly system bus registers (SB_LMMODE0/1 read by pvr_mem.cpp)
u32 sb_regs[0x540] = {};

// Mouse state (for OSD crosshair — unused in mirror mode)
s32 mo_x_abs[4] = {};
s32 mo_y_abs[4] = {};

// GraphicsContext::instance is provided by wsi/libretro.cpp
// Helper to set protected static member from wasm_gl_context.cpp
class GraphicsContextAccessor : public GraphicsContext {
public:
    static void setInstance(GraphicsContext* ctx) { instance = ctx; }
};
void setGraphicsContext(GraphicsContext* ctx) {
    GraphicsContextAccessor::setInstance(ctx);
}

// ============================================================================
// Settings — Dreamcast defaults for MVC2
// ============================================================================

settings_t settings;

void initSettings() {
    settings.platform.system = DC_PLATFORM_DREAMCAST;
    settings.platform.ram_size = 16 * 1024 * 1024;     // 16MB
    settings.platform.ram_mask = 0x00FFFFFF;
    settings.platform.vram_size = 8 * 1024 * 1024;     // 8MB
    settings.platform.vram_mask = 0x007FFFFF;
    settings.platform.aram_size = 2 * 1024 * 1024;     // 2MB
    settings.platform.aram_mask = 0x001FFFFF;
    settings.display.width = 640;
    settings.display.height = 480;
    settings.display.uiScale = 1.0f;
}

// ============================================================================
// Renderer_if.cpp stubs (we don't include the full render orchestration)
// ============================================================================

void rend_start_render() {}
void rend_swap_frame(u32) {}
void rend_set_fb_write_addr(u32) {}
void check_framebuffer_write() {}
void rend_disable_rollback() {}
bool rend_is_enabled() { return true; }
void rend_vblank() {}
void rend_cancel_emu_wait() {}
void rend_start_rollback() {}
void rend_allow_rollback() {}
void rend_enable_renderer(bool) {}
void rend_reset() {}
void rend_term_renderer() {}
int rend_end_render(int, int, int, void*) { return 0; }

// ============================================================================
// SPG stubs (scan pulse generator — hardware timing, not needed)
// ============================================================================

void CalculateSync() {}
void rescheduleSPG() {}

// ============================================================================
// Holly interrupt controller stubs
// ============================================================================

void asic_RaiseInterrupt(HollyInterruptID) {}
void asic_RaiseInterruptBothCLX(HollyInterruptID) {}
void asic_CancelInterrupt(HollyInterruptID) {}

// ============================================================================
// Address space / memory protection stubs (not available in WASM)
// ============================================================================

namespace addrspace {
    void protectVram(u32, u32) {}
    void unprotectVram(u32, u32) {}
    u32 getVramOffset(void*) { return (u32)-1; }
}

// ============================================================================
// Block manager stubs (GGPO memory tracking — not needed)
// ============================================================================

// These may be referenced via mem_watch.h templates
void bm_LockPage(u32, u32) {}
void bm_UnlockPage(u32, u32) {}
u32 bm_getRamOffset(void*) { return (u32)-1; }
bool bm_RamWriteAccess(void*) { return false; }

// ============================================================================
// EventManager stubs (renderer constructor/destructor uses these)
// ============================================================================

void EventManager::registerEvent(Event, Callback, void*) {}
void EventManager::unregisterEvent(Event, Callback, void*) {}
void EventManager::broadcastEvent(Event) {}

// The global Emulator instance is referenced transitively
Emulator emu;

// ============================================================================
// GUI stubs
// ============================================================================

void gui_display_osd() {}
void os_SetThreadName(const char*) {}

// OSD crosshair/VMU stubs (from vmu_xhair.h, used by gldraw.cpp)
#include "vmu_xhair.h"

std::pair<float, float> getCrosshairPosition(int) { return {0.f, 0.f}; }
float lightgun_crosshair_size = 0.f;
vmu_screen_params_t vmu_screen_params[4] = {};

// ============================================================================
// Host filesystem stubs (custom textures — disabled in mirror mode)
// ============================================================================

namespace hostfs {

std::string getTextureLoadPath(const std::string&) { return ""; }
std::string getTextureDumpPath() { return ""; }

// Minimal concrete AllStorage implementation (custom textures disabled, never called at runtime)
class NullAllStorage : public AllStorage {
public:
    bool isKnownPath(const std::string&) override { return false; }
    std::vector<FileInfo> listContent(const std::string&) override { return {}; }
    FILE* openFile(const std::string&, const std::string&) override { return nullptr; }
    std::string getParentPath(const std::string& p) override { return p; }
    std::string getSubPath(const std::string& r, const std::string&) override { return r; }
    FileInfo getFileInfo(const std::string&) override { return {}; }
    bool exists(const std::string&) override { return false; }
};

static NullAllStorage nullStorage;
AllStorage& storage() { return nullStorage; }

bool addStorage(bool, bool, const std::string&,
    void (*)(bool, std::string), const std::string&) { return false; }

// AllStorage base class methods (defined in storage.cpp which we don't include)
std::vector<FileInfo> AllStorage::listContent(const std::string&) { return {}; }
FILE* AllStorage::openFile(const std::string&, const std::string&) { return nullptr; }
std::string AllStorage::getParentPath(const std::string& p) { return p; }
std::string AllStorage::getSubPath(const std::string& r, const std::string&) { return r; }
FileInfo AllStorage::getFileInfo(const std::string&) { return {}; }
bool AllStorage::exists(const std::string&) { return false; }
std::string AllStorage::getDefaultDirectory() { return ""; }

} // namespace hostfs

// ============================================================================
// Serialization stubs (save state — not needed in mirror mode)
// ============================================================================

// If serialize.cpp is not included, we need these:
// Serializer and Deserializer are defined in serialize.h — check if they
// need stub implementations. The ta_ctx.cpp serialize/deserialize functions
// reference them but are never called in mirror mode.

// ============================================================================
// TA hardware stubs (we feed TA data directly, not through DMA)
// ============================================================================

void ta_vtx_ListInit(bool) {}
void ta_vtx_SoftReset() {}

// TaTypeLut — lookup table for TA command types. Pure computation, from ta.cpp.
#include "hw/pvr/ta.h"

u32 TaTypeLut::poly_data_type_id(PCW pcw) {
    if (pcw.Texture) {
        if (pcw.Volume == 0) {
            if (pcw.Col_Type == 0) return pcw.UV_16bit == 0 ? 3 : 4;
            else if (pcw.Col_Type == 1) return pcw.UV_16bit == 0 ? 5 : 6;
            else return pcw.UV_16bit == 0 ? 7 : 8;
        } else {
            if (pcw.Col_Type == 0) return pcw.UV_16bit == 0 ? 11 : 12;
            else if (pcw.Col_Type == 1) return INVALID_TYPE;
            else return pcw.UV_16bit == 0 ? 13 : 14;
        }
    } else {
        if (pcw.Volume == 0) {
            if (pcw.Col_Type == 0) return 0;
            else if (pcw.Col_Type == 1) return 1;
            else return 2;
        } else {
            if (pcw.Col_Type == 0) return 9;
            else if (pcw.Col_Type == 1) return INVALID_TYPE;
            else return 10;
        }
    }
}

u32 TaTypeLut::poly_header_type_size(PCW pcw) {
    if (pcw.Volume == 0) {
        if (pcw.Col_Type < 2) return 0;
        else if (pcw.Col_Type == 2) {
            if (pcw.Texture && pcw.Offset) return 2 | 0x80;
            else return 1;
        } else return 0;
    } else {
        if (pcw.Col_Type == 0) return 3;
        else if (pcw.Col_Type == 2) return 4 | 0x80;
        else if (pcw.Col_Type == 3) return 3;
        else return INVALID_TYPE;
    }
}

TaTypeLut::TaTypeLut() {
    for (int i = 0; i < 256; i++) {
        PCW pcw;
        pcw.obj_ctrl = i;
        u32 rv = poly_data_type_id(pcw);
        u32 type = poly_header_type_size(pcw);
        if (type & 0x80) rv |= SZ64 << 30;
        else rv |= SZ32 << 30;
        rv |= (type & 0x7F) << 8;
        table[i] = rv;
    }
}

// ta_vtx_data stubs (DMA write path — never called in mirror mode)
void DYNACALL ta_vtx_data32(const SQBuffer*) {}
void ta_vtx_data(const SQBuffer*, u32) {}

// ============================================================================
// PVR hardware stubs
// ============================================================================

// YUV_init() is already defined in pvr_mem.cpp

// ============================================================================
// i18n stubs
// ============================================================================

namespace i18n {
void init() {}
const char* T(const char* msg) { return msg; }
std::string getCurrentLocale() { return "en"; }
}

// ============================================================================
// Logging / debug stubs
// ============================================================================

#include "log/LogManager.h"

void GenericLog(LogTypes::LOG_LEVELS level, LogTypes::LOG_TYPE type,
    const char* file, int line, const char* fmt, ...) {
    // Silent in production WASM build — zero overhead
}

void os_DebugBreak() {
    // In WASM, trigger a trap for debugging
    __builtin_trap();
}

void fatal_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    __builtin_trap();
}
