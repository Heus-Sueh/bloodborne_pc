cmake_minimum_required(VERSION 3.20)
project(bbport-launcher LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL3 QUIET sdl3)
    pkg_check_modules(Freetype QUIET freetype2)
endif()

if(NOT SDL3_FOUND)
    find_package(SDL3 CONFIG QUIET)
endif()

if(NOT Freetype_FOUND)
    find_package(Freetype QUIET)
endif()

# RmlUi Dependency (Use local third_party if present, otherwise FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/RmlUi/Include/RmlUi/Core.h")
    set(RMLUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/RmlUi")
    file(GLOB_RECURSE RMLUI_CORE_SOURCES "${RMLUI_DIR}/Source/Core/*.cpp")
    list(APPEND RMLUI_SOURCES
        ${RMLUI_CORE_SOURCES}
        "${RMLUI_DIR}/Backends/RmlUi_Renderer_GL3.cpp"
        "${RMLUI_DIR}/Backends/RmlUi_Platform_SDL.cpp"
    )
    add_library(rmlui STATIC ${RMLUI_SOURCES})
    set_target_properties(rmlui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(rmlui PUBLIC
        RMLUI_SDL_VERSION_MAJOR=3
        RMLUI_FONT_ENGINE_FREETYPE
        RMLUI_NUM_MSAA_SAMPLES=0
    )
    target_include_directories(rmlui PUBLIC
        "${RMLUI_DIR}/Include"
        "${RMLUI_DIR}/Source/Core"
        "${RMLUI_DIR}/Backends"
    )
    if(SDL3_FOUND)
        target_link_libraries(rmlui PUBLIC ${SDL3_LIBRARIES})
        target_include_directories(rmlui PUBLIC ${SDL3_INCLUDE_DIRS})
    else()
        target_link_libraries(rmlui PUBLIC SDL3::SDL3)
    endif()
    if(Freetype_FOUND)
        target_link_libraries(rmlui PUBLIC ${Freetype_LIBRARIES})
        target_include_directories(rmlui PUBLIC ${Freetype_INCLUDE_DIRS})
    endif()
    target_link_libraries(rmlui PUBLIC OpenGL::GL)
else()
    include(FetchContent)
    FetchContent_Declare(
        rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG 6.0
        GIT_SHALLOW TRUE
    )
    set(RMLUI_FONT_ENGINE_FREETYPE ON CACHE BOOL "" FORCE)
    set(RMLUI_BACKENDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/backends" CACHE PATH "" FORCE)
    FetchContent_MakeAvailable(rmlui)
endif()

add_executable(bb-launcher
    src/main.cpp
    src/launcher_model.cpp
    src/config_manager.cpp
    src/mods_manager.cpp
    src/trophy_manager.cpp
    src/i18n.cpp
)

set_target_properties(bb-launcher PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "bb-launcher"
)

target_include_directories(bb-launcher PRIVATE
    src
)

target_link_libraries(bb-launcher PRIVATE
    rmlui
    OpenGL::GL
    Threads::Threads
)

if(SDL3_FOUND)
    target_link_libraries(bb-launcher PRIVATE ${SDL3_LIBRARIES})
    target_include_directories(bb-launcher PRIVATE ${SDL3_INCLUDE_DIRS})
elseif(TARGET SDL3::SDL3)
    target_link_libraries(bb-launcher PRIVATE SDL3::SDL3)
endif()

if(Freetype_FOUND)
    target_link_libraries(bb-launcher PRIVATE ${Freetype_LIBRARIES})
    target_include_directories(bb-launcher PRIVATE ${Freetype_INCLUDE_DIRS})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(bb-launcher PRIVATE m)
endif()

# Copy assets to build directory on build
add_custom_command(TARGET bb-launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:bb-launcher>/assets"
)
