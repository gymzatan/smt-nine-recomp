cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Shin Megami Tensei NINE - Static Recompilation
#
# Resource preparation and code generation are driven by tools/build.py.
# ============================================================================

project(smt9_recomp C)
if(WIN32)
    enable_language(RC ASM_MASM)
endif()
add_executable(apu_dma_identity_test EXCLUDE_FROM_ALL tests/apu_dma_identity_test.c)
target_include_directories(apu_dma_identity_test PRIVATE ../vendor/xboxrecomp/src ../vendor/xboxrecomp/include)
target_link_libraries(apu_dma_identity_test PRIVATE xboxrecomp)
add_executable(nv2a_register_test EXCLUDE_FROM_ALL tests/nv2a_register_test.c)
target_sources(nv2a_register_test PRIVATE tests/native_vulkan_test.c)
add_executable(input_foreground_test EXCLUDE_FROM_ALL tests/input_foreground_test.c)
target_include_directories(input_foreground_test PRIVATE ../vendor/xboxrecomp/src ../vendor/xboxrecomp/include)
if(MSVC)
    target_compile_options(input_foreground_test PRIVATE /UNDEBUG)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_library(dsp56300_emu_ffi STATIC IMPORTED)
set_target_properties(dsp56300_emu_ffi PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../build_dsp_jit/x86_64-pc-windows-msvc/release/dsp56300_emu_ffi.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../reference/dsp56300_release/dsp56300-0.1.3/crates/emu-ffi/include")
add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")
target_sources(nv2a_register_test PRIVATE
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c"
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c"
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c")
target_include_directories(nv2a_register_test PRIVATE "${XBOXRECOMP_DIR}/src/kernel")
target_link_libraries(nv2a_register_test PRIVATE xbox_nv2a xbox_d3d8 platform d3d11 dxgi dxguid)
if(MSVC)
    target_compile_options(nv2a_register_test PRIVATE /UNDEBUG /Ob2)
endif()
add_executable(apu_device_test EXCLUDE_FROM_ALL tests/apu_device_test.c)
target_include_directories(apu_device_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
target_link_libraries(apu_device_test PRIVATE xbox_apu xbox_nv2a platform dsp56300_emu_ffi)
if(MSVC)
    target_compile_options(apu_device_test PRIVATE /UNDEBUG)
endif()
set(SMT9_IMPORTED_LANGUAGE_ROOT "" CACHE PATH "Imported ISO package directory")
set(SMT9_IMPORTED_LANGUAGE_ID "" CACHE STRING "Imported ISO language identifier")
option(SMT9_WITH_CHINESE "Include the built-in Chinese game language" OFF)
if(SMT9_IMPORTED_LANGUAGE_ROOT)
    load_cache("${CMAKE_CURRENT_SOURCE_DIR}/../build/native" READ_WITH_PREFIX BUILTIN_ SMT9_WITH_CHINESE)
    set(SMT9_WITH_CHINESE "${BUILTIN_SMT9_WITH_CHINESE}")
endif()
set(SMT9_ROOT_FROM_EXE ".")
if(SMT9_WITH_CHINESE)
    set(SMT9_IMAGE_RELATIVE "runtime/native_game.xbe")
else()
    set(SMT9_IMAGE_RELATIVE "runtime/native_game_jp.xbe")
endif()
set(SMT9_CHECKPOINT_RELATIVE "runtime/instant.state")
if(SMT9_IMPORTED_LANGUAGE_ROOT)
    file(RELATIVE_PATH SMT9_PACKAGE_RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${SMT9_IMPORTED_LANGUAGE_ROOT}")
    set(SMT9_ROOT_FROM_EXE "../../../../..")
    set(SMT9_IMAGE_RELATIVE "${SMT9_PACKAGE_RELATIVE}/native_game.xbe")
    set(SMT9_CHECKPOINT_RELATIVE "${SMT9_PACKAGE_RELATIVE}/instant.state")
endif()
configure_file(src/game_paths.h.in generated/game_paths.h @ONLY)
add_executable(Config WIN32 src/config_main.c src/config_controls.c src/config.c src/language.c src/game_paths.c src/config.rc)
configure_file(src/updater.rc.in generated/updater.rc @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/updater.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/updater.ps1;${CMAKE_CURRENT_SOURCE_DIR}/../Apply-Update.ps1")
add_executable(Updater WIN32 src/updater.c "${CMAKE_CURRENT_BINARY_DIR}/generated/updater.rc")
target_link_libraries(Updater PRIVATE user32 ole32)
if(MSVC)
    target_compile_options(Updater PRIVATE /utf-8)
endif()
target_include_directories(Config PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(Config PRIVATE user32 shell32 comdlg32 xinput)
if(MSVC)
    target_compile_options(Config PRIVATE /utf-8)
endif()
# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
set(SMT9_RECOMP_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/generated" CACHE PATH
    "Directory containing the selected original generator outputs")
option(SMT9_MACHINE_CHECKPOINT "Enable instant save and load controls" ON)
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${SMT9_RECOMP_GEN_DIR}/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/game_paths.c
    src/config.c
    src/language.c
    src/font_assets.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

add_library(smt9_native_matrix OBJECT src/native_matrix.asm)
target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:smt9_native_matrix>)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE src/app.rc)
    set(SMT9_VULKAN_RUNTIME
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/backends/vulkan/dxcompiler.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../reference/gpu_runtime/DXC_LICENSE_LLVM"
        "${CMAKE_CURRENT_SOURCE_DIR}/../reference/gpu_runtime/DXC_LICENSE_MS")
    set(SMT9_OPENGL_RUNTIME
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/backends/opengl/d3d11.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/backends/opengl/nogi.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/backends/opengl/wined3d.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/../reference/gpu_runtime/WINED3D_COPYING.LIB")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
        ${SMT9_VULKAN_RUNTIME} ${SMT9_OPENGL_RUNTIME})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/vulkan"
        COMMAND ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/vulkan/d3d11.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/vulkan/nvgi.dll"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/vulkan/DXVK_LICENSE"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SMT9_VULKAN_RUNTIME}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/vulkan"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/opengl"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SMT9_OPENGL_RUNTIME}
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/backends/opengl")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)
if(SMT9_MACHINE_CHECKPOINT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SMT9_MACHINE_CHECKPOINT)
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_CALL_ARGUMENT_TARGET=0x002218D0u RECOMP_CALL_ARGUMENT_TARGET2=0x00012AB5u RECOMP_CALL_ARGUMENT_TARGET3=0x002F03E0u RECOMP_CALL_ARGUMENT_TARGET4=0x00016100u RECOMP_CALL_ARGUMENT_TARGET5=0x00220ED1u RECOMP_CALL_ARGUMENT_TARGET6=0x002F00D0u RECOMP_CALL_ARGUMENT_TARGET7=0x002EDE10u RECOMP_CALL_ARGUMENT_TARGET8=0x00105930u RECOMP_CALL_ARGUMENT_TARGET9=0x00106FE0u)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${SMT9_RECOMP_GEN_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    shell32
    d3d11
    dxgi
    dxguid
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /utf-8)
endif()

option(SMT9_LINK_TIME_OPTIMIZATION "Optimize Release game/runtime code across translation units" ON)
set(SMT9_PROFILE_MODE "Off" CACHE STRING "MSVC profile-guided build: Off, Generate, or Use")
set_property(CACHE SMT9_PROFILE_MODE PROPERTY STRINGS Off Generate Use)
set(SMT9_PROFILE_DATA "${CMAKE_CURRENT_BINARY_DIR}/smt9_recomp.pgd" CACHE FILEPATH
    "MSVC profile database for this game build")
if(NOT SMT9_PROFILE_MODE MATCHES "^(Off|Generate|Use)$")
    message(FATAL_ERROR "SMT9_PROFILE_MODE must be Off, Generate, or Use")
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    if(SMT9_LINK_TIME_OPTIMIZATION)
        get_target_property(SMT9_RUNTIME_TARGETS xboxrecomp INTERFACE_LINK_LIBRARIES)
        set_property(TARGET ${PROJECT_NAME} ${SMT9_RUNTIME_TARGETS}
            nv2a_register_test apu_device_test apu_dma_identity_test
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    elseif(NOT SMT9_PROFILE_MODE STREQUAL "Off")
        message(FATAL_ERROR "Profile-guided builds require SMT9_LINK_TIME_OPTIMIZATION")
    endif()
    if(SMT9_PROFILE_MODE STREQUAL "Generate")
        target_link_options(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Release>:/FASTGENPROFILE:PGD=${SMT9_PROFILE_DATA}>")
    elseif(SMT9_PROFILE_MODE STREQUAL "Use")
        if(NOT EXISTS "${SMT9_PROFILE_DATA}")
            message(FATAL_ERROR "SMT9_PROFILE_DATA must name an existing trained profile database")
        endif()
        target_link_options(${PROJECT_NAME} PRIVATE
            "$<$<CONFIG:Release>:/USEPROFILE:PGD=${SMT9_PROFILE_DATA}>")
    endif()
elseif(NOT SMT9_PROFILE_MODE STREQUAL "Off")
    message(FATAL_ERROR "Profile-guided builds require the MSVC compiler")
endif()
mark_as_advanced(SMT9_LINK_TIME_OPTIMIZATION SMT9_PROFILE_MODE SMT9_PROFILE_DATA)

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
