cmake_minimum_required(VERSION 3.20)

# Configure with clang-cl (see tools/build_mcla.ps1). The recompiled code relies on
# Clang extensions (alias/weak attributes, __builtin_debugtrap, __int128).
project(MCLARecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Embed debug info in objects (/Z7): avoids PDB contention across parallel jobs.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")

set(XENONRECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenonRecomp)
set(THIRDPARTY_ROOT ${XENONRECOMP_ROOT}/thirdparty)

# XenonUtils provides the XEX loader used at runtime.
add_subdirectory(${THIRDPARTY_ROOT}/disasm ${CMAKE_BINARY_DIR}/disasm)
add_subdirectory(${XENONRECOMP_ROOT}/XenonUtils ${CMAKE_BINARY_DIR}/XenonUtils)

add_subdirectory(MCLARecompLib)
add_subdirectory(MCLA)
