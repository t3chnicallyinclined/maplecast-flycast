target_sources(${PROJECT_NAME} PRIVATE
        mc_readtrace.cpp
        mc_readtrace.h
        gsta_charpass.cpp
        gsta_charpass.h
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
        maplecast_oracle_hook.cpp
        maplecast_oracle_hook.h
        maplecast_replica_live.cpp
        maplecast_replica_live.h
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
        maplecast_state_replica.cpp
        maplecast_state_replica.h
        maplecast_rollback.cpp
        maplecast_rollback.h
        maplecast_lockstep.cpp
        maplecast_lockstep.h
        maplecast_predict.cpp
        maplecast_predict.h
        maplecast_predictor.cpp
        maplecast_predictor.h
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
        maplecast_shadow_exec.cpp
        maplecast_shadow_exec.h
        maplecast_wasm_bridge.cpp)

# === Shadow executor (live game-tick validation) ==========================
# maplecast_shadow_exec.cpp is ALWAYS compiled (no-op unless the executor is linked), so the
# unconditional maplecast_shadow_exec::onFrame() call in serverPublish always resolves. Turning
# on -DMAPLECAST_SHADOW_EXEC=ON links the standalone transpiled game-tick executor into the
# server so each frame is validated vs mem_b (runtime-gated by the MAPLECAST_SHADOW_EXEC env).
# gen_tick_all.c is DERIVED game code (gitignored): regenerate it before building via
#   python tools/render-replica-poc/gen_tick.py
if(MAPLECAST_SHADOW_EXEC)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tools/render-replica-poc/gen_tick_all.c")
        message(FATAL_ERROR "MAPLECAST_SHADOW_EXEC=ON but gen_tick_all.c is missing — run: python tools/render-replica-poc/gen_tick.py")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/render-replica-poc/shadow_exec_runner.c"
        "${CMAKE_SOURCE_DIR}/tools/render-replica-poc/gen_tick_all.c")
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/render-replica-poc")
    target_compile_definitions(${PROJECT_NAME} PRIVATE MAPLECAST_SHADOW_EXEC_BUILD)
endif()

# === Native GSTA client (feat/render-replica-live) =========================
# The transpiled MVC2 render path (tools/render-replica-poc, byte-exact) is
# amalgamated into gsta_render_frame.c and consumed by the GSTA client section
# of maplecast_mirror.cpp. Only the mirror-client build needs it (the client
# renders the ~12KB/frame GSTA wire through flycast's OWN renderer). The POC dir
# is added as an include dir so "sh4ctx.h" + the #include'd .c paths resolve.
if(MAPLECAST_CLIENT_ONLY)
    target_sources(${PROJECT_NAME} PRIVATE
        gsta_render_frame.c
        gsta_render_frame.h
        gsta_stage.cpp
        gsta_stage.h
        gsta_render_debug.cpp
        gsta_render_debug.h)
    # Put the POC dir on the flycast target's include path so gsta_render_frame.c's
    # #include'd .c files + their "sh4ctx.h" resolve. A source-file property does NOT
    # propagate here because the target is defined in the top-level CMakeLists, not
    # this subdir (CMake source-prop scope) — target_include_directories is reliable.
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/render-replica-poc")
    # MUST be target_compile_definitions (not add_compile_definitions): the flycast
    # target is defined in the PARENT CMakeLists, so a directory-scoped define here
    # never reaches maplecast_mirror.cpp / mainui.cpp (the GSTA section #ifdef'd it out).
    target_compile_definitions(${PROJECT_NAME} PRIVATE MAPLECAST_GSTA_CLIENT_BUILD)
endif()

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
