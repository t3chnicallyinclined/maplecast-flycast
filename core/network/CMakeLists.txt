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
        maplecast_xdp_input.cpp
        maplecast_xdp_input.h
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
        maplecast_wasm_bridge.cpp)

# maplecast_stream (H.264 / NVENC / CUDA GL interop) requires CUDA+NVENC
# SDKs. Only include when those were detected (CMakeLists.txt:1714 sets
# MAPLECAST_NVENC). Without NVENC the file won't even compile — it
# unconditionally #include <cuda.h>.
#
# maplecast_webrtc (libdatachannel) is the P2P H.264 data-channel path;
# only include when libdatachannel submodule is present + MAPLECAST_WEBRTC
# compile define was set by the main CMakeLists.
#
# Both are GPU-dependent legacy streaming paths. Headless + most client
# builds without an NVIDIA GPU skip them; the TA mirror on port 7200
# (always compiled in) carries the render stream instead.
if(NOT MAPLECAST_HEADLESS)
    if(CUDA_INCLUDE AND CUDA_LIB AND NVENC_INCLUDE)
        target_sources(${PROJECT_NAME} PRIVATE
            maplecast_stream.cpp
            maplecast_stream.h)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/core/deps/libdatachannel/CMakeLists.txt")
        target_sources(${PROJECT_NAME} PRIVATE
            maplecast_webrtc.cpp
            maplecast_webrtc.h)
    endif()
endif()
