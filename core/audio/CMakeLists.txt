target_sources(${PROJECT_NAME} PRIVATE
        audiostream.h)

if(NOT LIBRETRO)
    target_sources(${PROJECT_NAME} PRIVATE
            audiobackend_alsa.cpp
            audiobackend_coreaudio.cpp
            audiobackend_libao.cpp
            audiobackend_null.cpp
            audiobackend_oboe.cpp
            audiobackend_omx.cpp
            audiobackend_oss.cpp
            audiobackend_pulseaudio.cpp
            audiobackend_sdl2.cpp
            audiostream.cpp)
    # DirectSound depends on getNativeHwnd() which is provided by sdl.cpp / winmain.cpp.
    # Headless Windows builds have neither, so skip the backend (audio is stubbed
    # at runtime to audiobackend_null in headless mode anyway).
    if(NOT MAPLECAST_HEADLESS)
        target_sources(${PROJECT_NAME} PRIVATE
                audiobackend_directsound.cpp)
    endif()
endif()
