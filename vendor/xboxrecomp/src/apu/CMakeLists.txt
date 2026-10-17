# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global/Encode processors: original DSP56300 JIT and DMA

get_filename_component(APU_SAMPLERATE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../reference/libsamplerate" ABSOLUTE)
add_library(xbox_apu STATIC
    apu_resampler.c
    "${APU_SAMPLERATE_SOURCE}/src/samplerate.c"
    "${APU_SAMPLERATE_SOURCE}/src/src_linear.c"
    "${APU_SAMPLERATE_SOURCE}/src/src_zoh.c"
    apu_core.c
    apu_ac97.c
    apu_vp.c
    apu_dsp.c
    dsp/dsp.c
    dsp/dsp_jit.c
    dsp/dsp_dma.c
    apu_mmio_hook.c
    apu_xaudio2.c
)

if(NOT TARGET dsp56300_emu_ffi)
    message(FATAL_ERROR "The native DSP56300 JIT dependency must be supplied by the parent project")
endif()
target_link_libraries(xbox_apu PRIVATE dsp56300_emu_ffi)
target_include_directories(xbox_apu PRIVATE
    "${APU_SAMPLERATE_SOURCE}/src" "${APU_SAMPLERATE_SOURCE}/include")
target_compile_definitions(xbox_apu PRIVATE HAVE_STDBOOL_H ENABLE_SINC_FAST_CONVERTER
    PACKAGE="libsamplerate" VERSION="0.2.2")

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /UNDEBUG
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32 bcrypt ntdll userenv ws2_32)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
