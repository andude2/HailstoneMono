cmake_minimum_required(VERSION 3.20)
project(HSMono LANGUAGES CXX)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS hailstone-addon.txt)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/hailstone-addon.txt" version_line REGEX "^version ")
string(REGEX REPLACE "^version " "" plugin_version "${version_line}")
add_compile_definitions(HAILSTONE_MONO_VERSION="${plugin_version}")

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$" OR
        CMAKE_GENERATOR_PLATFORM STREQUAL "x64"))
    message(FATAL_ERROR "The checked-in Mono runtime requires a Windows x64 build")
endif()

set(HAILSTONE_SDK "" CACHE PATH "Directory containing hailstone_plugin.h")
if(NOT EXISTS "${HAILSTONE_SDK}/hailstone_plugin.h")
    message(FATAL_ERROR "Set HAILSTONE_SDK to Hailstone's sdk directory")
endif()

# The managed UI draws through the client's Dear ImGui context. The plugin
# must use the same Dear ImGui version the client builds (the SDK's
# imgui_context contract), so point this at that source tree. A checkout that
# already has it is preferred; FetchContent is the fallback for a fresh one.
set(HAILSTONE_IMGUI_DIR "" CACHE PATH "Dear ImGui source directory the client builds with")
if(NOT EXISTS "${HAILSTONE_IMGUI_DIR}/imgui.h")
    include(FetchContent)
    FetchContent_Declare(hailstone_imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 97428e8ac99e339ce05eee531cf55b77b29ea709 # v1.91.9
        SYSTEM)
    FetchContent_MakeAvailable(hailstone_imgui_src)
    set(HAILSTONE_IMGUI_DIR "${hailstone_imgui_src_SOURCE_DIR}")
endif()

add_library(hailstone_imgui STATIC
    "${HAILSTONE_IMGUI_DIR}/imgui.cpp"
    "${HAILSTONE_IMGUI_DIR}/imgui_draw.cpp"
    "${HAILSTONE_IMGUI_DIR}/imgui_tables.cpp"
    "${HAILSTONE_IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(hailstone_imgui SYSTEM PUBLIC "${HAILSTONE_IMGUI_DIR}")
# The client compiles its copy with this definition; the version and every
# structure the plugin and client share must match.
target_compile_definitions(hailstone_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_features(hailstone_imgui PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(hailstone_imgui PRIVATE /W0)
endif()

add_library(HSMono MODULE Plugin.cpp platform/MonoLibrary.cpp ImGuiBridge.cpp)
target_compile_features(HSMono PRIVATE cxx_std_20)
target_include_directories(HSMono PRIVATE "${HAILSTONE_SDK}")
target_include_directories(HSMono SYSTEM PRIVATE ../Mono/include/mono-2.0)
target_link_libraries(HSMono PRIVATE hailstone_imgui)
if(MSVC)
    target_compile_options(HSMono PRIVATE /W4 /utf-8)
endif()

find_program(CSHARP_COMPILER NAMES csc
    HINTS "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319" REQUIRED)
configure_file(Smoke.cs Smoke.cs COPYONLY)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Smoke.dll"
    COMMAND "${CSHARP_COMPILER}" /nologo /target:library
        /out:Smoke.dll Smoke.cs
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Smoke.cs" VERBATIM)
add_custom_target(mono_smoke ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Smoke.dll")

# Managed caller for the ImGui bridge; ui_bridge_test draws two frames through
# it against a real ImGui context.
configure_file(tests/UiSmoke.cs UiSmoke.cs COPYONLY)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/UiSmoke.dll"
    COMMAND "${CSHARP_COMPILER}" /nologo /target:library
        /out:UiSmoke.dll UiSmoke.cs
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/UiSmoke.cs" VERBATIM)
add_custom_target(ui_smoke ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/UiSmoke.dll")

add_executable(ui_bridge_test EXCLUDE_FROM_ALL
    tests/UiBridgeTest.cpp ImGuiBridge.cpp platform/MonoLibrary.cpp)
target_compile_features(ui_bridge_test PRIVATE cxx_std_20)
target_include_directories(ui_bridge_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(ui_bridge_test SYSTEM PRIVATE ../Mono/include/mono-2.0)
target_link_libraries(ui_bridge_test PRIVATE hailstone_imgui)
add_dependencies(ui_bridge_test ui_smoke)

# Runs the bridge against the real runtime and a synthetic Hailstone SDK host.
add_executable(embedded_warrior_test EXCLUDE_FROM_ALL
    tests/EmbeddedWarrior.cpp Plugin.cpp platform/MonoLibrary.cpp ImGuiBridge.cpp)
target_compile_features(embedded_warrior_test PRIVATE cxx_std_20)
target_include_directories(embedded_warrior_test PRIVATE "${HAILSTONE_SDK}")
target_include_directories(embedded_warrior_test SYSTEM PRIVATE ../Mono/include/mono-2.0)
target_link_libraries(embedded_warrior_test PRIVATE hailstone_imgui)
