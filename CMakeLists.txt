cmake_minimum_required(VERSION 3.20)
project(ProjectM_Visualizer_SDL3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL3 paths (downloaded by setup_project.ps1)
set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SDL3")
include_directories("${SDL3_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
link_directories("${SDL3_DIR}/lib/x64")

# ProjectM and CURL find packages (via vcpkg)
find_package(CURL REQUIRED)
find_package(projectM REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE
    SDL3::SDL3
    CURL::libcurl
    projectM::projectM
    opengl32
)

# Auto-copy DLLs to the output directory upon build
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDL3_DIR}/lib/x64/SDL3.dll"
    $<TARGET_FILE_DIR:${PROJECT_NAME}>
)
