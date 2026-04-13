#!/bin/bash
# ============================================================================
# MAPLECAST QUICKSTART — Get running in 5 minutes
#
# Usage:
#   ./scripts/quickstart.sh server    # Build + run headless server
#   ./scripts/quickstart.sh relay     # Build + run the relay
#   ./scripts/quickstart.sh webgpu    # Serve WebGPU renderer (no build!)
#   ./scripts/quickstart.sh wasm      # Build + serve WASM renderer
#   ./scripts/quickstart.sh all       # Everything at once
#
# Prerequisites:
#   - Linux (Ubuntu 22.04+ recommended)
#   - A Dreamcast ROM (MVC2 GDI)
#   - For server: cmake, g++, libcurl-dev, zlib1g-dev, libzstd-dev
#   - For relay: Rust toolchain (curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh)
#   - For WASM: Emscripten SDK
#   - For WebGPU: just Python3 (for local HTTP server) or any static file server
# ============================================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'

banner() {
    echo -e "${CYAN}"
    echo "╔══════════════════════════════════════════╗"
    echo "║   MAPLECAST QUICKSTART                    ║"
    echo "║   OVERKILL IS NECESSARY                   ║"
    echo "╚══════════════════════════════════════════╝"
    echo -e "${NC}"
}

check_deps() {
    local missing=()
    for cmd in "$@"; do
        if ! command -v "$cmd" &>/dev/null; then missing+=("$cmd"); fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        echo -e "${RED}Missing dependencies: ${missing[*]}${NC}"
        echo "Install with: sudo apt-get install -y ${missing[*]}"
        return 1
    fi
    echo -e "${GREEN}All dependencies found.${NC}"
}

# ─── SERVER ──────────────────────────────────────────
build_server() {
    echo -e "\n${YELLOW}[1/3] Checking dependencies...${NC}"
    check_deps cmake g++ pkg-config

    echo -e "\n${YELLOW}[2/3] Building headless flycast server...${NC}"
    cmake -B build-headless \
        -DMAPLECAST_HEADLESS=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build build-headless -j"$(nproc)"
    strip build-headless/flycast

    echo -e "\n${YELLOW}[3/3] Verifying no GPU deps...${NC}"
    if ldd build-headless/flycast | grep -qiE 'libGL|libSDL|libX11'; then
        echo -e "${RED}ERROR: Binary has GPU dependencies. MAPLECAST_HEADLESS gate broken.${NC}"
        exit 1
    fi
    echo -e "${GREEN}Server built: build-headless/flycast ($(du -h build-headless/flycast | cut -f1))${NC}"
}

run_server() {
    local rom="${1:-}"
    if [ -z "$rom" ]; then
        echo -e "${YELLOW}Usage: $0 server <path-to-rom.gdi>${NC}"
        echo "  Example: $0 server ~/roms/mvc2/mvc2.gdi"
        exit 1
    fi
    if [ ! -f "$rom" ]; then
        echo -e "${RED}ROM not found: $rom${NC}"
        exit 1
    fi
    echo -e "\n${GREEN}Starting flycast headless server...${NC}"
    echo "  ROM: $rom"
    echo "  Stream: ws://localhost:7210"
    echo "  Input:  udp://localhost:7100"
    echo "  Control: ws://localhost:7211"
    echo ""
    echo -e "${CYAN}Press Ctrl+C to stop.${NC}"
    build-headless/flycast "$rom"
}

# ─── RELAY ───────────────────────────────────────────
build_relay() {
    echo -e "\n${YELLOW}[1/2] Checking Rust toolchain...${NC}"
    check_deps cargo

    echo -e "\n${YELLOW}[2/2] Building relay...${NC}"
    cd relay
    cargo build --release
    cd "$ROOT"
    echo -e "${GREEN}Relay built: relay/target/release/maplecast-relay${NC}"
}

run_relay() {
    echo -e "\n${GREEN}Starting relay...${NC}"
    echo "  Upstream: ws://127.0.0.1:7210 (flycast)"
    echo "  Clients:  ws://0.0.0.0:7201"
    echo ""
    relay/target/release/maplecast-relay \
        --ws-upstream ws://127.0.0.1:7210 \
        --ws-listen 0.0.0.0:7201 \
        --http-listen 127.0.0.1:7202 \
        --no-webtransport
}

# ─── WEBGPU ──────────────────────────────────────────
serve_webgpu() {
    echo -e "\n${GREEN}Serving WebGPU renderer...${NC}"
    echo "  URL: http://localhost:8080/webgpu-test.html"
    echo ""
    echo -e "${CYAN}No build needed — pure JavaScript!${NC}"
    echo "Make sure flycast server + relay are running first."
    echo ""
    cd web
    if command -v python3 &>/dev/null; then
        python3 -m http.server 8080
    elif command -v npx &>/dev/null; then
        npx serve -l 8080
    else
        echo -e "${RED}Need python3 or npx to serve files${NC}"
        exit 1
    fi
}

# ─── WASM ────────────────────────────────────────────
build_wasm() {
    echo -e "\n${YELLOW}Building WASM renderer...${NC}"
    if [ ! -d "packages/renderer" ]; then
        echo -e "${RED}packages/renderer not found${NC}"
        exit 1
    fi
    cd packages/renderer
    ./build.sh
    cd "$ROOT"
    echo -e "${GREEN}WASM renderer built.${NC}"
}

# ─── ALL ─────────────────────────────────────────────
run_all() {
    local rom="${1:-}"
    if [ -z "$rom" ]; then
        echo -e "${YELLOW}Usage: $0 all <path-to-rom.gdi>${NC}"
        exit 1
    fi
    build_server
    build_relay

    echo -e "\n${GREEN}Starting all services...${NC}"
    # Start server in background
    build-headless/flycast "$rom" &
    SERVER_PID=$!
    sleep 3

    # Start relay in background
    relay/target/release/maplecast-relay \
        --ws-upstream ws://127.0.0.1:7210 \
        --ws-listen 0.0.0.0:7201 \
        --http-listen 127.0.0.1:7202 \
        --no-webtransport &
    RELAY_PID=$!
    sleep 1

    echo -e "\n${GREEN}════════════════════════════════════════${NC}"
    echo -e "${GREEN}  MAPLECAST IS RUNNING!${NC}"
    echo -e "${GREEN}════════════════════════════════════════${NC}"
    echo ""
    echo "  WebGPU: Open web/webgpu-test.html in Chrome"
    echo "          (edit WS URL to ws://localhost:7201)"
    echo ""
    echo "  Server PID: $SERVER_PID"
    echo "  Relay PID:  $RELAY_PID"
    echo ""
    echo -e "${CYAN}Press Ctrl+C to stop all.${NC}"

    trap "kill $SERVER_PID $RELAY_PID 2>/dev/null; exit" INT TERM
    wait
}

# ─── MAIN ────────────────────────────────────────────
banner

case "${1:-help}" in
    server)
        build_server
        run_server "${2:-}"
        ;;
    relay)
        build_relay
        run_relay
        ;;
    webgpu)
        serve_webgpu
        ;;
    wasm)
        build_wasm
        ;;
    all)
        run_all "${2:-}"
        ;;
    *)
        echo "Usage: $0 {server|relay|webgpu|wasm|all} [rom-path]"
        echo ""
        echo "  server <rom>  — Build + run headless flycast server"
        echo "  relay         — Build + run the stream relay"
        echo "  webgpu        — Serve WebGPU renderer (no build!)"
        echo "  wasm          — Build WASM renderer"
        echo "  all <rom>     — Build + run everything"
        echo ""
        echo "Quick start:"
        echo "  Terminal 1: $0 server ~/roms/mvc2/mvc2.gdi"
        echo "  Terminal 2: $0 relay"
        echo "  Terminal 3: $0 webgpu"
        echo "  Browser:    http://localhost:8080/webgpu-test.html"
        ;;
esac
