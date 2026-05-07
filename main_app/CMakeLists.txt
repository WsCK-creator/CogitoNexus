cmake_minimum_required(VERSION 3.16)
project(CogitoBrain)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(vad/rnnoise)

# Konfiguracja whisper.cpp - wylaczamy budowanie zbednych dodatkow
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(whisper)

# Dodajemy VAD jako bibliotekę współdzieloną (DLL)
add_library(vad SHARED vad/vad.cpp)
target_include_directories(vad PUBLIC vad/rnnoise/include)
target_link_libraries(vad PUBLIC rnnoise)
target_compile_definitions(vad PRIVATE VAD_EXPORTS)

add_executable(brain 
brain/main.cpp
brain/brain.cpp)
target_include_directories(brain PRIVATE vad/rnnoise/include whisper vad)
target_link_libraries(brain vad whisper)