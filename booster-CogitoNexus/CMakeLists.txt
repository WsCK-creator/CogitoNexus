cmake_minimum_required(VERSION 3.14)
project(cogito_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDK path
set(SDK_DIR "/home/booster/Workspace/sdk_release-main" CACHE PATH "Booster Robotics SDK directory")

# Piper1-gpl install path (kept only as a CACHE default so the piper_exe path
# can be passed to cogito_server as a runtime argv default in documentation /
# scripts; the libpiper headers/libs are NOT linked into cogito_server -- see
# the architecture note at the top of src/booster-CogitoNexus.cpp for why:
# linking libpiper directly into this binary together with the Booster SDK's
# fastrtps/fastcdr/openssl stack silently breaks espeak-ng phonemization.
# TTS is instead run out-of-process via the `piper_exe` CLI.
set(PIPER_DIR "/home/booster/projects/booster-CogitoNexus/piper1-gpl/libpiper/install" CACHE PATH "libpiper install directory")

# Include headers
include_directories(${SDK_DIR}/include)
include_directories(${SDK_DIR}/third_party/include)
include_directories(${SDK_DIR}/third_party)

# Link directories
link_directories(${SDK_DIR}/lib/aarch64)

# Common libraries
set(COMMON_LIBS
    ${SDK_DIR}/lib/aarch64/libbooster_robotics_sdk.a
    fastrtps
    fastcdr
    pthread
)

# ============================================================
# Target: Full Server (with ChannelFactory)
# TTS is NOT linked in-process (see note above) -- cogito_server shells out
# to piper_exe at runtime for each utterance.
# ============================================================
add_executable(cogito_server src/booster-CogitoNexus.cpp)
target_link_libraries(cogito_server PRIVATE ${COMMON_LIBS})
target_compile_options(cogito_server PRIVATE -Wall -Wextra -pedantic)

# Install
install(TARGETS cogito_server DESTINATION bin)

# NOTE: booster_tts_alsa and booster_audio_only targets were removed because
# their source files (src/booster_tts_alsa.cpp, src/booster_audio_only.cpp)
# no longer exist in this project. Re-add them here if those files come back.
