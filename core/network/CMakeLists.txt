target_sources(${PROJECT_NAME} PRIVATE
        alienfnt_modem.cpp
        alienfnt_modem.h
        dns.cpp
        ggpo.cpp
        ggpo.h
        maplecast.cpp
        maplecast.h
        maplecast_telemetry.cpp
        maplecast_telemetry.h
        miniupnp.cpp
        miniupnp.h
        naomi_network.cpp
        naomi_network.h
        net_handshake.cpp
        net_handshake.h
        net_platform.h
        output.cpp
        output.h
        picoppp.cpp
        picoppp.h
        netservice.cpp
        netservice.h
        dcnet.cpp
        dcnet.h
        ice.cpp
        ice.h
        ifaddrs.c
        maplecast_input_server.cpp
        maplecast_input_server.h
        maplecast_audio.cpp
        maplecast_audio.h
        maplecast_audio_ws.cpp
        maplecast_audio_ws.h
        maplecast_audio_client.cpp
        maplecast_audio_client.h
        maplecast_gamestate.cpp
        maplecast_gamestate.h
        maplecast_mirror.cpp
        maplecast_mirror.h
        maplecast_ws_server.cpp
        maplecast_ws_server.h
        maplecast_control_ws.cpp
        maplecast_control_ws.h
        maplecast_player.cpp
        maplecast_player.h
        maplecast_state_sync.cpp
        maplecast_state_sync.h
        maplecast_replica.cpp
        maplecast_replica.h
        maplecast_input_sink.cpp
        maplecast_input_sink.h
        maplecast_palette.cpp
        maplecast_palette.h
        hub_discovery.cpp
        hub_discovery.h
        replay_writer.cpp
        replay_writer.h
        replay_reader.cpp
        replay_reader.h
        maplecast_wasm_bridge.cpp)

# maplecast_stream (H.264 / NVENC / CUDA GL interop) and maplecast_webrtc
# (libdatachannel) are the GPU-dependent legacy streaming paths. Exclude
# them from the headless build — the TA mirror on port 7200 replaces them.
if(NOT MAPLECAST_HEADLESS AND NOT MAPLECAST_CLIENT_ONLY)
    target_sources(${PROJECT_NAME} PRIVATE
        maplecast_stream.cpp
        maplecast_stream.h
        maplecast_webrtc.cpp
        maplecast_webrtc.h)
endif()

# Linux-only optimizations: AF_XDP zero-copy gamepad receive (Linux kernel BPF)
# and direct evdev gamepad polling (Linux input subsystem). Both have non-Linux
# fallback paths at runtime — SDL gamepad on Windows/macOS does the equivalent
# job. Keep these out of non-Linux builds so they can't break the compile.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE
        maplecast_xdp_input.cpp
        maplecast_xdp_input.h
        maplecast_evdev_input.cpp
        maplecast_evdev_input.h)
endif()

# Option 6: lookup-table renderer infrastructure.
# Visual cache (records game-state → TA-display-list mappings), brute-force
# scanner (drives the emulator through every char/anim state), and lookup-test
# rig (record-and-replay validator). All three use POSIX file APIs (mkdir,
# popen, ls) so we gate to Linux for now. Default OFF so production headless
# binaries don't carry the disk-writing code paths even on Linux unless the
# operator explicitly opts in. Hooks in emulator.cpp / Renderer_if.cpp /
# TexCache.cpp are guarded by MAPLECAST_LOOKUP so they vanish from the binary
# when this flag is off — zero hot-path cost. See docs/STREAMING-OPTIONS.md
# Option 6 and the c0621cbe7 strip commit for context.
option(MAPLECAST_LOOKUP "Build the Option 6 lookup-renderer cache+scanner machinery (Linux only)" OFF)
if(MAPLECAST_LOOKUP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE
        maplecast_visual_cache.cpp
        maplecast_visual_cache.h
        maplecast_scanner.cpp
        maplecast_scanner.h
        maplecast_lookup_test.cpp
        maplecast_lookup_test.h)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MAPLECAST_LOOKUP)
elseif(MAPLECAST_LOOKUP)
    message(WARNING "MAPLECAST_LOOKUP=ON ignored on non-Linux: visual_cache/scanner/lookup_test use POSIX file APIs")
endif()
