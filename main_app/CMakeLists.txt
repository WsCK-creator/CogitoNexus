cmake_minimum_required(VERSION 3.16)
project(CogitoBrain)

# Standard C++20 dla nowoczesnego AI i asynchroniczności
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Wskazujemy ścieżkę do pliku w podfolderze brain
add_executable(brain brain/main.cpp)
