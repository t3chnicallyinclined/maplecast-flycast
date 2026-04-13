#!/bin/bash
# ============================================================================
# MAPLECAST — Install all build dependencies
#
# Usage:
#   ./scripts/install-deps.sh          # Install everything
#   ./scripts/install-deps.sh server   # Just server deps
#   ./scripts/install-deps.sh relay    # Just relay deps
#   ./scripts/install-deps.sh all      # Everything (same as no args)
#
# Tested on Ubuntu 22.04+ / Debian 12+. Other distros: adapt the package
# names, the tools are the same.
# ============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'

usage() {
    echo "Usage: $0 [server|relay|all]"
    echo ""
    echo "  server  — C++ build deps (cmake, g++, libs)"
    echo "  relay   — Rust toolchain"
    echo "  all     — Everything (default)"
}

install_server_deps() {
    echo -e "${CYAN}[server] Installing C++ build dependencies...${NC}"
    sudo apt-get update -qq
    sudo apt-get install -y \
        build-essential \
        cmake \
        pkg-config \
        git \
        libcurl4-openssl-dev \
        zlib1g-dev \
        libzstd-dev \
        libudev-dev
    echo -e "${GREEN}[server] Done. You can now build the headless server.${NC}"
}

install_relay_deps() {
    echo -e "${CYAN}[relay] Installing Rust toolchain...${NC}"
    if command -v cargo &>/dev/null; then
        echo -e "${GREEN}[relay] Rust already installed: $(rustc --version)${NC}"
    else
        echo "Installing Rust via rustup..."
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
        # shellcheck source=/dev/null
        source "$HOME/.cargo/env"
        echo -e "${GREEN}[relay] Rust installed: $(rustc --version)${NC}"
    fi
}

check_result() {
    echo ""
    echo -e "${GREEN}════════════════════════════════════════${NC}"
    echo -e "${GREEN}  Dependencies installed!${NC}"
    echo -e "${GREEN}════════════════════════════════════════${NC}"
    echo ""
    echo "Next steps:"
    echo "  ./scripts/quickstart.sh server ~/roms/mvc2.gdi"
    echo "  ./scripts/quickstart.sh relay"
    echo "  ./scripts/quickstart.sh webgpu"
}

case "${1:-all}" in
    server)
        install_server_deps
        ;;
    relay)
        install_relay_deps
        ;;
    all)
        install_server_deps
        install_relay_deps
        ;;
    -h|--help|help)
        usage
        exit 0
        ;;
    *)
        echo -e "${RED}Unknown target: $1${NC}"
        usage
        exit 1
        ;;
esac

check_result
