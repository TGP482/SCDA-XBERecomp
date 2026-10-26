# Splinter Cell: Double Agent (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(scda LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
# SDL3 for controllers and the headset's audio, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
# Opus for the wideband voice link, static, without the stack protector and fortify flags that need GCC's libssp
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG v1.5.2 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(zydis tomlplusplus imgui SDL3 opus)

file(GLOB RECOMP_CONFIGS CONFIGURE_DEPENDS config/*.toml)

# What the game needs to build its code on the player's PC (src/app/game_code.cpp), beside the executables: the
# recompiler, clang and lld from the toolchain building this, the configs and the headers the generated code includes
set(COMPILER_DIR ${CMAKE_BINARY_DIR}/compiler)
get_filename_component(LLVM_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
file(GLOB COMPILER_HEADERS CONFIGURE_DEPENDS src/cpu/x86_ops.h src/cpu/x86_context.h)
file(GLOB FREESTANDING_HEADERS CONFIGURE_DEPENDS src/cpu/freestanding/*)
add_custom_target(compiler_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPILER_DIR}/config ${COMPILER_DIR}/include/cpu
            ${COMPILER_DIR}/include/freestanding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLVM_BIN}/clang.exe ${LLVM_BIN}/lld-link.exe
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/LLVM-LICENSE.txt ${COMPILER_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RECOMP_CONFIGS} ${COMPILER_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMPILER_HEADERS} ${COMPILER_DIR}/include/cpu
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FREESTANDING_HEADERS} ${COMPILER_DIR}/include/freestanding
    VERBATIM)

# the movies' subtitles beside the executables (src/game/subtitles.cpp): fan transcripts made with tools/subtitles
file(GLOB SUBTITLE_FILES CONFIGURE_DEPENDS subtitles/*.srt)
if(SUBTITLE_FILES)
    add_custom_target(subtitle_files ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/subtitles
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SUBTITLE_FILES} ${CMAKE_BINARY_DIR}/subtitles
        VERBATIM)
endif()

# x86 to C++ recompiler
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS tools/recomp/*.cpp)
add_executable(xbe_recomp ${RECOMP_SOURCES})
set_target_properties(xbe_recomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${COMPILER_DIR})
target_include_directories(xbe_recomp PRIVATE src tools/recomp)
target_compile_definitions(xbe_recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(xbe_recomp PRIVATE Zydis tomlplusplus::tomlplusplus)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)

file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/(game|versus)/")
# SDL's and Opus's headers go to the input, gyro and voice code alone (both are linked LINK_ONLY)
set_source_files_properties(src/core/input.cpp src/core/gyro.cpp src/audio/voice.cpp src/audio/voice_link.cpp
    PROPERTIES
    INCLUDE_DIRECTORIES "${sdl3_SOURCE_DIR}/include;${opus_SOURCE_DIR}/include")

# Generate build_info.h and track it as a dependency of version-related
find_package(Git QUIET)
set(BUILD_INFO ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(build_info
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${BUILD_INFO} -DGIT=${GIT_EXECUTABLE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO}
    VERBATIM)
set_source_files_properties(src/app/system_info.cpp src/app/version.rc PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR} OBJECT_DEPENDS ${BUILD_INFO})

# One executable per recompiled image: the shared host and the game hooks only that image has. The image's code goes
# to <target>.dll beside it, which the executable delay loads: built here from the image's config (partitions as that
# config sets them), while a release leaves it out and the executable builds it from the player's xbe
# (src/app/game_code.cpp, which compiles with the same flags).
function(scda_image target xbe config partitions game_dir)
    set(GEN ${CMAKE_CURRENT_SOURCE_DIR}/generated/${game_dir})
    set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
    math(EXPR last "${partitions} - 1")
    foreach(i RANGE ${last})
        list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${GEN}/codegen.stamp
        BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h ${GEN}/exports.def
                   ${GEN}/imports.def ${GEN}/code.lib
        COMMAND xbe_recomp config/${config}
        COMMAND ${LLVM_BIN}/lld-link.exe /lib /machine:x64 /def:${GEN}/imports.def /out:${GEN}/code.lib
        COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
        DEPENDS xbe_recomp ${RECOMP_CONFIGS} ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Recompiling ${xbe}"
        VERBATIM)
    add_custom_target(recompile_${target} DEPENDS ${GEN}/codegen.stamp)
    file(GLOB GAME_SOURCES CONFIGURE_DEPENDS src/${game_dir}/*.cpp)
    # the xbe's SHA-1: the executable checks the game it finds is the release it was made from
    set(xbe_file ${CMAKE_CURRENT_SOURCE_DIR}/assets/${xbe})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${xbe_file})
    file(SHA1 ${xbe_file} xbe_sha1)
    set(XBE_HASH ${CMAKE_CURRENT_BINARY_DIR}/${target}_xbe.cpp)
    file(CONFIGURE OUTPUT ${XBE_HASH} CONTENT "extern const char kImageSha1[] = \"${xbe_sha1}\";\n")
    add_executable(${target} WIN32 ${HOST_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES} ${XBE_HASH}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/version.rc)
    add_dependencies(${target} recompile_${target} build_info compiler_files)
    if(TARGET subtitle_files)
        add_dependencies(${target} subtitle_files)
    endif()
    # the import library of what the executable exports to its DLL goes to the compiler folder
    set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON ARCHIVE_OUTPUT_DIRECTORY ${COMPILER_DIR})
    target_include_directories(${target} PRIVATE src ${GEN})
    target_include_directories(${target} SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -fno-strict-aliasing)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> $<LINK_ONLY:opus> d3d12 dxgi dxguid d3dcompiler dwrite xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
    target_link_libraries(${target} PRIVATE ${GEN}/code.lib delayimp)
    target_link_options(${target} PRIVATE -fuse-ld=lld -g -Wl,/delayload:${target}.dll)

    # without a C runtime: the executable passes on what the code calls
    add_library(${target}_code SHARED ${GEN_SOURCES})
    set_target_properties(${target}_code PROPERTIES OUTPUT_NAME ${target} CXX_EXTENSIONS OFF)
    add_dependencies(${target}_code recompile_${target})
    target_include_directories(${target}_code PRIVATE src ${GEN})
    target_include_directories(${target}_code SYSTEM PRIVATE src/cpu/freestanding)
    target_compile_definitions(${target}_code PRIVATE X86_GAME_CODE)
    target_compile_options(${target}_code PRIVATE -fms-compatibility-version=19.40 -O2 -fno-strict-aliasing -nostdinc
        -nostdinc++ -w)
    target_link_libraries(${target}_code PRIVATE ${target})
    target_link_options(${target}_code PRIVATE -fuse-ld=lld -Wl,/noentry,/nodefaultlib,/opt:noicf
        -Wl,/def:${GEN}/exports.def)
    # a stamp from an earlier build on the executable's first start would outrank this DLL
    add_custom_command(TARGET ${target}_code POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f ${COMPILER_DIR}/work/${target}/built.txt)
endfunction()

scda_image(SCDA default.xbe scda_v2.toml 128 game)        # solo and co-op
scda_image(SCDA_Versus sc4_adv.xbe sc4_adv.toml 64 versus)  # versus
