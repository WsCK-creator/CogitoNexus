cmake_minimum_required(VERSION 3.16)
project(CogitoBrain)

#set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- VAD ---
add_subdirectory(vad/rnnoise)

# --- LLAMA.CPP ---
set(GGML_CUDA ON CACHE BOOL "Wsparcie dla NVIDIA GPU" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(LLM/llama.cpp)

# --- WHISPER ---
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(whisper/whisper.cpp)

add_executable(brain 
    brain/main.cpp
    brain/brain.cpp
    vad/vad.cpp
    whisper/whisper-wrapper.cpp
    LLM/LLM.cpp
)

target_compile_features(brain PRIVATE cxx_std_20)

target_include_directories(brain PRIVATE 
    vad/rnnoise/include 
    whisper 
    vad 
    whisper/whisper.cpp/include
    LLM                        
    LLM/llama.cpp/include      
)
target_link_directories(brain PRIVATE ${CMAKE_BINARY_DIR}/vad/rnnoise/$<CONFIG>)
target_link_libraries(brain rnnoise whisper llama)