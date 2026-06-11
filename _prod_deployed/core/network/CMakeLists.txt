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
        maplecast_oracle_hook.cpp
        maplecast_oracle_hook.h
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
