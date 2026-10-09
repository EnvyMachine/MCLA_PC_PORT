# The PC executable: host runtime + kernel/XAM implementations + recompiled game.

# The translated shaders' support header is compiled into the executable.
set(XENOS_COMMON_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/render/xenos/common.hlsli)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${XENOS_COMMON_HLSL})
file(READ ${XENOS_COMMON_HLSL} XENOS_COMMON_HLSL_TEXT)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/xenos_common_hlsl.cpp
    "// Generated from render/xenos/common.hlsli by MCLA/CMakeLists.txt.\n"
    "namespace render::xenos { extern const char kCommonHlsl[] = R\"HLSL(${XENOS_COMMON_HLSL_TEXT})HLSL\"; }\n")

add_executable(MCLA
    main.cpp
    apu/audio_out.cpp
    apu/ffmpeg.cpp
    apu/xma.cpp
    debug/d3d_trace.cpp
    debug/d3d_trace_hooks.generated.cpp
    gpu/command_processor.cpp
    gpu/microcode.cpp
    gpu/shader_registry.cpp
    render/descriptors.cpp
    render/device.cpp
    render/renderer.cpp
    render/screenshot.cpp
    render/shader_cache.cpp
    render/shared_memory.cpp
    render/texture_cache.cpp
    render/upload.cpp
    render/window.cpp
    render/xenos/translator.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/xenos_common_hlsl.cpp
    runtime/crash.cpp
    runtime/game_patches.cpp
    runtime/input.cpp
    runtime/kernel.cpp
    runtime/loader.cpp
    runtime/log.cpp
    runtime/memory.cpp
    runtime/memory_watch.cpp
    runtime/profiler.cpp
    runtime/thread.cpp
    runtime/vfs.cpp
    kernel/audio_net_imports.cpp
    kernel/generated_stubs.cpp
    kernel/io_imports.cpp
    kernel/memory_imports.cpp
    kernel/rtl_imports.cpp
    kernel/thread_imports.cpp
    kernel/video_imports.cpp
    kernel/xam_imports.cpp
    res/mcla.rc
)

# A Windows program (no console window); main() stays the entry point.
target_link_options(MCLA PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")

# ppc/ first so its ppc_config.h/ppc_context.h win over XenonUtils' template copy.
target_include_directories(MCLA BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/ppc)
set(XENOS_RECOMP_THIRDPARTY ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty)
target_include_directories(MCLA PRIVATE
    ${XENOS_RECOMP_THIRDPARTY}/xxHash
    ${XENOS_RECOMP_THIRDPARTY}/dxc-bin/inc
)

target_link_libraries(MCLA PRIVATE
    MCLARecompLib
    XenonUtils
    xinput
    d3d12
    dxgi
    dxguid
    dbghelp
    bcrypt
    synchronization
    winmm
    xaudio2
    ole32
)

target_compile_definitions(MCLA PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(MCLA PRIVATE
    "/EHsc"
    "/clang:-march=sandybridge"
    "/clang:-fno-strict-aliasing"
    "-Wno-unused-function"
    "-Wno-unused-variable"
    "-Wno-unused-parameter"
)

# XMA audio decodes with FFmpeg 7.1's xma2 decoder, loaded at run time from
# <exe>/ffmpeg (apu/ffmpeg.cpp). Copy the DLLs from an existing FFmpeg 7.1 shared
# build (OBS Studio ships one) when available.
set(MCLA_FFMPEG_DIR "C:/Program Files/obs-studio/bin/64bit" CACHE PATH "Folder with FFmpeg 7.1 DLLs (avcodec-61.dll)")
if(EXISTS "${MCLA_FFMPEG_DIR}/avcodec-61.dll")
    set(MCLA_FFMPEG_DLLS avcodec-61.dll avutil-59.dll swresample-5.dll)
    foreach(optional libx264-164.dll zlib.dll)
        if(EXISTS "${MCLA_FFMPEG_DIR}/${optional}")
            list(APPEND MCLA_FFMPEG_DLLS ${optional})
        endif()
    endforeach()
    list(TRANSFORM MCLA_FFMPEG_DLLS PREPEND "${MCLA_FFMPEG_DIR}/")
    add_custom_command(TARGET MCLA POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:MCLA>/ffmpeg
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MCLA_FFMPEG_DLLS} $<TARGET_FILE_DIR:MCLA>/ffmpeg
    )
endif()

# DXC compiles translated shaders at run time.
add_custom_command(TARGET MCLA POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${XENOS_RECOMP_THIRDPARTY}/dxc-bin/bin/x64/dxcompiler.dll
        ${XENOS_RECOMP_THIRDPARTY}/dxc-bin/bin/x64/dxil.dll
        $<TARGET_FILE_DIR:MCLA>
)
