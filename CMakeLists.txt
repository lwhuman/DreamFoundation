cmake_minimum_required(VERSION 3.14)

project(DreamFoundation
        VERSION 1.0.0
        LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

file(GLOB_RECURSE DREAM_FOUNDATION_SOURCES ${PROJECT_SOURCE_DIR}/src/*.c)

# add_executable(
#     DreamTest
#     ${DREAM_FOUNDATION_SOURCES}
# )

add_library(
    dlogger-lite
    ${PROJECT_SOURCE_DIR}/src/Dream/LoggerLite.c
)

target_include_directories(dlogger-lite PUBLIC ${PROJECT_SOURCE_DIR}/include)

# target_link_libraries(DreamTest PRIVATE xcb xcb-keysyms xcb-xinput xcb-icccm EGL)
