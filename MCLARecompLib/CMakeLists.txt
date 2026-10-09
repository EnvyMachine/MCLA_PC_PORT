# Static library holding the game code translated by XenonRecomp (../ppc).
# Regenerate ../ppc with XenonRecomp + config/MCLA.toml, then re-run CMake configure
# so the glob picks up the new file list.
file(GLOB MCLA_PPC_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/ppc/*.cpp)

add_library(MCLARecompLib STATIC ${MCLA_PPC_SOURCES})

target_include_directories(MCLARecompLib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/ppc
        ${XENONRECOMP_ROOT}/thirdparty/simde
)

target_compile_options(MCLARecompLib
    PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/ppc_overrides.h"
        "/clang:-march=sandybridge"
        "/clang:-fno-strict-aliasing"
        "-Wno-unused-label"
        "-Wno-unused-variable"
        "-Wno-unused-but-set-variable"
        "-Wno-unused-function"
)
