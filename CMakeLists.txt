cmake_minimum_required(VERSION 3.24)
project(bloodborne_pc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Global build directory outputs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/core")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(Vulkan REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(Freetype REQUIRED IMPORTED_TARGET freetype2)

# Submodules & GPU library
add_subdirectory(core)

# Third-party decoders: ATRAC9
add_library(atrac9 STATIC
    third_party/LibAtrac9/C/src/libatrac9.c
)
set_target_properties(atrac9 PROPERTIES C_STANDARD 99)
target_include_directories(atrac9 PUBLIC third_party/LibAtrac9/C/src)

# Third-party UI: RmlUi
file(GLOB_RECURSE RMLUI_CORE_SOURCES
    "third_party/RmlUi/Source/Core/*.cpp"
)
list(APPEND RMLUI_SOURCES
    ${RMLUI_CORE_SOURCES}
    third_party/RmlUi/Backends/RmlUi_Renderer_GL3.cpp
    third_party/RmlUi/Backends/RmlUi_Platform_SDL.cpp
)
add_library(rmlui STATIC ${RMLUI_SOURCES})
set_target_properties(rmlui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(rmlui PUBLIC
    RMLUI_SDL_VERSION_MAJOR=3
    RMLUI_FONT_ENGINE_FREETYPE
    RMLUI_NUM_MSAA_SAMPLES=0
)
target_include_directories(rmlui PUBLIC
    third_party/RmlUi/Include
    third_party/RmlUi/Source/Core
    third_party/RmlUi/Backends
)
target_link_libraries(rmlui PUBLIC
    PkgConfig::SDL3
    PkgConfig::Freetype
    OpenGL::GL
)

# Runtime Subsystems & Services
set(RUNTIME_C_SOURCES
    src/runtime_ajm.c
    src/runtime_file.c
    src/runtime_host.c
    src/runtime_memory.c
    src/runtime_ngs2.c
    src/runtime_pad.c
    src/runtime_thread.c
    src/runtime_toypad.c
    src/runtime.c
)

set(RUNTIME_CPP_SOURCES
    src/config_manager.cpp
    src/loader.cpp
    src/runtime_audio.cpp
    src/runtime_compat.cpp
    src/runtime_content.cpp
    src/runtime_error.cpp
    src/runtime_kernel.cpp
    src/runtime_mutex.cpp
    src/runtime_net.cpp
    src/runtime_platform.cpp
    src/runtime_registry.cpp
    src/runtime_rewatch.cpp
    src/runtime_rtc.cpp
    src/runtime_rwlock.cpp
    src/runtime_savedata.cpp
    src/runtime_sema.cpp
    src/runtime_services.cpp
    src/runtime_subsystems.cpp
    src/runtime_vfs.cpp
)

set_source_files_properties(${RUNTIME_C_SOURCES} PROPERTIES
    LANGUAGE C
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
set_source_files_properties(${RUNTIME_CPP_SOURCES} PROPERTIES
    LANGUAGE CXX
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

add_library(bbruntime STATIC
    ${RUNTIME_C_SOURCES}
    ${RUNTIME_CPP_SOURCES}
)
target_include_directories(bbruntime PUBLIC . src third_party/LibAtrac9/C/src)
target_link_libraries(bbruntime PUBLIC
    PkgConfig::SDL3
    PkgConfig::Vulkan
    atrac9
    Threads::Threads
    m
    ${CMAKE_DL_LIBS}
)

# Core Executables
# 1. bb-probe
add_executable(bb-probe
    src/probe.c
    src/vulkan_smoke.c
)
set_target_properties(bb-probe PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    BUILD_RPATH "\$ORIGIN/core;\${CMAKE_BINARY_DIR}/core"
    INSTALL_RPATH "\$ORIGIN/core"
)
target_include_directories(bb-probe PRIVATE . src)
target_link_libraries(bb-probe PRIVATE
    bbruntime
    bbgpu
    atrac9
    PkgConfig::SDL3
    PkgConfig::Vulkan
    Threads::Threads
    m
    ${CMAKE_DL_LIBS}
)
target_link_options(bb-probe PRIVATE -no-pie -rdynamic)

# 2. bb-runner (Native C++23 game runner)
add_executable(bb-runner
    src/config_manager.cpp
    src/loader.cpp
    src/runner.cpp
)
set_target_properties(bb-runner PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
target_include_directories(bb-runner PRIVATE . src)
target_link_libraries(bb-runner PRIVATE Threads::Threads)

# 3. bb-launcher (Native C++20 UI launcher)
add_executable(bb-launcher
    src/config_manager.cpp
    launcher/src/launcher_model.cpp
    launcher/src/main.cpp
)
set_target_properties(bb-launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(bb-launcher PRIVATE . src launcher/src)
target_link_libraries(bb-launcher PRIVATE
    rmlui
    PkgConfig::SDL3
    PkgConfig::Freetype
    OpenGL::GL
    Threads::Threads
    m
)

# 4. bb-gpu-capabilities
add_executable(bb-gpu-capabilities tools/gpu_capabilities.c)
set_target_properties(bb-gpu-capabilities PROPERTIES C_STANDARD 11)
target_link_libraries(bb-gpu-capabilities PRIVATE PkgConfig::Vulkan PkgConfig::SDL3 m)

# Comprehensive Test Suites
enable_testing()
add_compile_options(-UNDEBUG)

add_executable(pad-test tests/test_pad.c)
set_target_properties(pad-test PROPERTIES C_STANDARD 11)
target_include_directories(pad-test PRIVATE . src)
target_link_libraries(pad-test PRIVATE PkgConfig::SDL3 PkgConfig::Vulkan m)
add_test(NAME pad COMMAND pad-test)

add_executable(platform-test tests/test_platform.cpp src/runtime_platform.cpp)
set_target_properties(platform-test PROPERTIES CXX_STANDARD 23)
target_include_directories(platform-test PRIVATE . src)
target_link_libraries(platform-test PRIVATE Threads::Threads)
add_test(NAME platform COMMAND platform-test)

add_executable(subsystems-test
    tests/test_subsystems.cpp
    src/runtime_subsystems.cpp
    src/runtime_savedata.cpp
    src/runtime_content.cpp
    src/runtime_rtc.cpp
)
set_target_properties(subsystems-test PROPERTIES CXX_STANDARD 23)
target_include_directories(subsystems-test PRIVATE . src)
target_link_libraries(subsystems-test PRIVATE Threads::Threads)
add_test(NAME subsystems COMMAND subsystems-test)

add_executable(vfs-test tests/test_vfs.cpp src/runtime_vfs.cpp)
set_target_properties(vfs-test PROPERTIES CXX_STANDARD 23)
target_include_directories(vfs-test PRIVATE . src)
target_link_libraries(vfs-test PRIVATE Threads::Threads)
add_test(NAME vfs COMMAND vfs-test)

add_executable(loader-test tests/test_loader.cpp src/loader.cpp)
set_target_properties(loader-test PROPERTIES CXX_STANDARD 23)
target_include_directories(loader-test PRIVATE . src)
target_link_libraries(loader-test PRIVATE Threads::Threads)
add_test(NAME loader COMMAND loader-test)

add_executable(savedata-test tests/test_savedata.cpp src/runtime_savedata.cpp)
set_target_properties(savedata-test PROPERTIES CXX_STANDARD 23)
target_include_directories(savedata-test PRIVATE . src)
target_link_libraries(savedata-test PRIVATE Threads::Threads)
add_test(NAME savedata COMMAND savedata-test)

add_executable(rtc-test tests/test_rtc.cpp src/runtime_rtc.cpp)
set_target_properties(rtc-test PROPERTIES CXX_STANDARD 23)
target_include_directories(rtc-test PRIVATE . src)
target_link_libraries(rtc-test PRIVATE Threads::Threads)
add_test(NAME rtc COMMAND rtc-test)

add_executable(runtime-test tests/test_runtime.c)
set_target_properties(runtime-test PROPERTIES C_STANDARD 11)
target_include_directories(runtime-test PRIVATE . src)
target_link_libraries(runtime-test PRIVATE bbruntime bbgpu atrac9 PkgConfig::SDL3 PkgConfig::Vulkan Threads::Threads m ${CMAKE_DL_LIBS})
add_test(NAME runtime COMMAND runtime-test)

add_executable(file-mods-test tests/test_file_mods.c src/runtime_vfs.cpp)
set_target_properties(file-mods-test PROPERTIES C_STANDARD 11)
set_source_files_properties(src/runtime_vfs.cpp PROPERTIES LANGUAGE CXX CXX_STANDARD 23)
target_include_directories(file-mods-test PRIVATE . src)
target_link_libraries(file-mods-test PRIVATE Threads::Threads)
add_test(NAME file-mods COMMAND file-mods-test)

add_executable(sema-test tests/test_sema.c)
set_target_properties(sema-test PROPERTIES C_STANDARD 11)
target_include_directories(sema-test PRIVATE . src)
target_link_libraries(sema-test PRIVATE bbruntime bbgpu atrac9 PkgConfig::SDL3 PkgConfig::Vulkan Threads::Threads m ${CMAKE_DL_LIBS})
add_test(NAME sema COMMAND sema-test)

add_executable(online-test tests/test_online.c)
set_target_properties(online-test PROPERTIES C_STANDARD 11)
target_include_directories(online-test PRIVATE . src)
target_link_libraries(online-test PRIVATE bbruntime bbgpu atrac9 PkgConfig::SDL3 PkgConfig::Vulkan Threads::Threads m ${CMAKE_DL_LIBS})
add_test(NAME online COMMAND online-test)

add_executable(content-test tests/test_content.c src/runtime_content.cpp)
set_target_properties(content-test PROPERTIES C_STANDARD 11)
set_source_files_properties(src/runtime_content.cpp PROPERTIES LANGUAGE CXX CXX_STANDARD 23)
target_include_directories(content-test PRIVATE . src)
target_link_libraries(content-test PRIVATE Threads::Threads)
add_test(NAME content COMMAND content-test)

add_executable(launcher-test tests/test_launcher.cpp src/config_manager.cpp)
set_target_properties(launcher-test PROPERTIES CXX_STANDARD 20)
target_include_directories(launcher-test PRIVATE . src launcher/src)
target_link_libraries(launcher-test PRIVATE Threads::Threads)
add_test(NAME launcher COMMAND launcher-test)
