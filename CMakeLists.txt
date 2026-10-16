cmake_minimum_required(VERSION 3.20)
project(DivaNativeDLSS VERSION 0.3.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MSVC on Windows x64 is required")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# The NVIDIA NGX (DLSS) backend needs the SDK import library from FetchSDK.ps1.
# It is optional: without it the build still produces a working DLL that offers
# the AMD FidelityFX (FSR4) backend, which is all an AMD GPU needs.
option(DIVA_ENABLE_NGX "Build the NVIDIA DLSS (NGX) backend" ON)
if(DIVA_ENABLE_NGX AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib/nvsdk_ngx_s.lib")
    message(STATUS "NVIDIA NGX SDK not found - building without the DLSS backend.")
    message(STATUS "Run FetchSDK.ps1 to enable DLSS alongside FSR4.")
    set(DIVA_ENABLE_NGX OFF)
endif()

add_subdirectory(third_party/minhook)

add_library(DivaNativeDLSS SHARED src/main.cpp)
target_compile_options(DivaNativeDLSS PRIVATE /W4 /O2 /EHsc /utf-8)
target_compile_definitions(DivaNativeDLSS PRIVATE DIVA_ENABLE_NGX=$<BOOL:${DIVA_ENABLE_NGX}>)
target_include_directories(DivaNativeDLSS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DivaNativeDLSS PRIVATE minhook d3d11 dxgi d3d12 d3dcompiler user32 gdi32 advapi32 version)
if(DIVA_ENABLE_NGX)
    target_link_libraries(DivaNativeDLSS PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib/nvsdk_ngx_s.lib")
endif()

option(BUILD_GPU_TEST "Build the manual D3D11 GPU test harness" ON)
if(BUILD_GPU_TEST)
    add_executable(gpu_test gpu_test.cpp)
    target_compile_options(gpu_test PRIVATE /EHsc /utf-8)
    target_link_libraries(gpu_test PRIVATE d3d11 dxgi d3dcompiler user32)
endif()

# Host-side diagnostics for the D3D11 <-> D3D12 interop the FSR4 backend uses.
# They need a real GPU; see docs/fsr4.md.
option(BUILD_TOOLS "Build the interop/FSR4 diagnostic tools" ON)
if(BUILD_TOOLS)
    foreach(tool interop_probe share_probe bridge_final fsr_test)
        add_executable(${tool} tools/${tool}.cpp)
        target_compile_options(${tool} PRIVATE /EHsc /utf-8)
        target_include_directories(${tool} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(${tool} PRIVATE d3d11 d3d12 dxgi d3dcompiler advapi32 version)
    endforeach()
endif()

# ReShade add-on that provides the in-game configuration panel. ReShade loads
# add-ons from the game root (*.addon / *.addon64), completely separately from
# DivaModLoader, and the two communicate only through settings.ini.
option(BUILD_RESHADE_ADDON "Build the ReShade add-on with the configuration panel" ON)
if(BUILD_RESHADE_ADDON)
    add_library(DivaNativeDLSSAddon SHARED src_reshade/addon.cpp)
    target_compile_options(DivaNativeDLSSAddon PRIVATE /W4 /O2 /EHsc /utf-8)
    target_include_directories(DivaNativeDLSSAddon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src_reshade")
    set_target_properties(DivaNativeDLSSAddon PROPERTIES
        OUTPUT_NAME "DivaNativeDLSS"
        SUFFIX ".addon64")
endif()

# MinHook's own CMakeLists installs its headers and .lib unconditionally, so
# `cmake --install` cannot produce a clean mod folder. This target builds the
# release payload explicitly: a mod folder is just the DLL plus its config,
# plus the ReShade add-on that sits in the game root.
set(DIVA_PACKAGE_NAME "DivaNativeDLSS-${PROJECT_VERSION}")
set(DIVA_RELEASE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/release")
set(DIVA_PACKAGE_DIR "${DIVA_RELEASE_ROOT}/${DIVA_PACKAGE_NAME}")
# The release root has to exist at configure time: it is the working directory
# of the packaging commands, and MSBuild refuses to start a command whose working
# directory is missing.
file(MAKE_DIRECTORY "${DIVA_RELEASE_ROOT}")
add_custom_target(package
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${DIVA_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIVA_PACKAGE_DIR}/mods/Diva Native DLSS"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist/mods/Diva Native DLSS" "${DIVA_PACKAGE_DIR}/mods/Diva Native DLSS"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DivaNativeDLSS>" "${DIVA_PACKAGE_DIR}/mods/Diva Native DLSS/DivaNativeDLSS.dll"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:DivaNativeDLSSAddon>" "${DIVA_PACKAGE_DIR}/DivaNativeDLSS.addon64"
    COMMAND ${CMAKE_COMMAND} -E tar "cf" "${DIVA_PACKAGE_NAME}.zip" --format=zip -- "${DIVA_PACKAGE_NAME}/mods" "${DIVA_PACKAGE_NAME}/DivaNativeDLSS.addon64"
    WORKING_DIRECTORY "${DIVA_RELEASE_ROOT}"
    DEPENDS DivaNativeDLSS DivaNativeDLSSAddon
    COMMENT "Packaging mods/Diva Native DLSS into ${DIVA_PACKAGE_DIR}"
)

# Kept for compatibility with the upstream workflow. Prefer the `package`
# target above; `cmake --install` also emits MinHook's headers and import lib.
install(TARGETS DivaNativeDLSS RUNTIME DESTINATION "mods/Diva Native DLSS")
install(DIRECTORY dist/ DESTINATION .)
