cmake_minimum_required(VERSION 3.20)
project(BeatShoreTestClient)

add_executable(BeatShoreTestClient
    Source/main.cpp
)

target_compile_features(BeatShoreTestClient PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(BeatShoreTestClient PRIVATE /W4)
endif()
