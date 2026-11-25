# Splinter Cell: Double Agent (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
if(WIN32)
    project(scda LANGUAGES C CXX RC)
else()
    project(scda LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zydis GIT_REPOSITORY https://github.com/zyantific/zydis.git GIT_TAG v4.1.1 GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
if(WIN32)
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
else()
    # Linux: a release whose SDL3 backend knows SDL 3.2's API, and whose Vulkan backend draws with dynamic rendering
    FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
endif()
# SDL3 for controllers and the headset's audio, static so it links into the exe
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
else()
    set(SDL_RENDER ON CACHE BOOL "" FORCE)  # Linux: the splash and the first start's progress window draw with it
endif()
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
if(NOT WIN32)
    # Linux: the Vulkan renderer's allocator, header only
    FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.1.0 GIT_SHALLOW TRUE)
    FetchContent_GetProperties(vma)
    if(NOT vma_POPULATED)
        FetchContent_Populate(vma)
    endif()
    find_package(Vulkan REQUIRED)
    find_package(glslang REQUIRED)
    find_package(PNG REQUIRED)
    find_package(Threads REQUIRED)
endif()

file(GLOB RECOMP_CONFIGS CONFIGURE_DEPENDS config/*.toml)

# What the game needs to build its code on the player's PC (src/app/game_code.cpp), beside the executables: the
# recompiler, clang and lld from the toolchain building this, the configs and the headers the generated code includes
set(COMPILER_DIR ${CMAKE_BINARY_DIR}/compiler)
get_filename_component(LLVM_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
file(GLOB COMPILER_HEADERS CONFIGURE_DEPENDS src/cpu/x86_ops.h src/cpu/x86_context.h)
file(GLOB FREESTANDING_HEADERS CONFIGURE_DEPENDS src/cpu/freestanding/*)
if(WIN32)
    set(COMPILER_TOOLS ${LLVM_BIN}/clang.exe ${LLVM_BIN}/lld-link.exe ${CMAKE_CURRENT_SOURCE_DIR}/tools/LLVM-LICENSE.txt)
else()
    # Linux: the game code builds with the system's clang
    set(COMPILER_TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools/LLVM-LICENSE.txt)
endif()
add_custom_target(compiler_files ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${COMPILER_DIR}/config ${COMPILER_DIR}/include/cpu
            ${COMPILER_DIR}/include/freestanding
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMPILER_TOOLS} ${COMPILER_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RECOMP_CONFIGS} ${COMPILER_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMPILER_HEADERS} ${COMPILER_DIR}/include/cpu
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FREESTANDING_HEADERS} ${COMPILER_DIR}/include/freestanding
    VERBATIM)

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
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
if(WIN32)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
else()
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
endif()

file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/(game|versus|launcher)/")
if(WIN32)
    list(FILTER HOST_SOURCES EXCLUDE REGEX "/src/linux/|/src/gpu/vulkan[^/]*\\.cpp$")
else()
    # Linux: these have counterparts in src/linux and src/gpu/vulkan*.cpp
    list(FILTER HOST_SOURCES EXCLUDE REGEX
        "/src/(gpu/d3d12|gpu/pipeline_cache|audio/xaudio2|app/main|app/cursor|app/splash|app/handover|app/focus_pause|app/game_code)\\.cpp$")
endif()
# SDL's and Opus's headers go to the input, gyro and voice code alone (both are linked LINK_ONLY)
set(SDL_USERS src/core/input.cpp src/core/gyro.cpp src/audio/voice.cpp src/audio/voice_link.cpp)
if(NOT WIN32)
    file(GLOB LINUX_SOURCES CONFIGURE_DEPENDS src/linux/*.cpp)
    list(APPEND SDL_USERS ${LINUX_SOURCES} src/gpu/vulkan.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp)
endif()
set_source_files_properties(${SDL_USERS}
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
    if(WIN32)
        set(IMPORT_LIB_COMMAND COMMAND ${LLVM_BIN}/lld-link.exe /lib /machine:x64 /def:${GEN}/imports.def /out:${GEN}/code.lib)
        set(IMPORT_LIB ${GEN}/code.lib)
    else()
        set(IMPORT_LIB_COMMAND)
        set(IMPORT_LIB)
    endif()
    add_custom_command(
        OUTPUT ${GEN}/codegen.stamp
        BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h ${GEN}/exports.def
                   ${GEN}/imports.def ${IMPORT_LIB}
        COMMAND xbe_recomp config/${config}
        ${IMPORT_LIB_COMMAND}
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
    set(${target}_SHA1 ${xbe_sha1} PARENT_SCOPE)
    if(WIN32)
        add_executable(${target} WIN32 ${HOST_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES} ${XBE_HASH}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/app/version.rc)
    else()
        add_executable(${target} ${HOST_SOURCES} ${GAME_SOURCES} ${IMGUI_SOURCES} ${XBE_HASH})
    endif()
    add_dependencies(${target} recompile_${target} build_info compiler_files)
    target_include_directories(${target} PRIVATE src ${GEN})
    target_include_directories(${target} SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -fno-strict-aliasing)
    if(WIN32)
        # the import library of what the executable exports to its DLL goes to the compiler folder
        set_target_properties(${target} PROPERTIES ENABLE_EXPORTS ON ARCHIVE_OUTPUT_DIRECTORY ${COMPILER_DIR})
        target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> $<LINK_ONLY:opus> d3d12 dxgi dxguid d3dcompiler xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32)
        target_link_libraries(${target} PRIVATE ${GEN}/code.lib delayimp)
        target_link_options(${target} PRIVATE -fuse-ld=lld -g -Wl,/delayload:${target}.dll)
    else()
        # the runtime and the overrides the .so binds to are exported (default visibility), the rest stays hidden
        target_compile_options(${target} PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
                target_include_directories(${target} SYSTEM PRIVATE ${vma_SOURCE_DIR}/include)
        target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> $<LINK_ONLY:opus>
            Vulkan::Vulkan glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits PNG::PNG
            Threads::Threads ${CMAKE_DL_LIBS})
        # the libraries an AppImage carries in usr/lib, found from usr/bin; RPATH (not RUNPATH) so theirs are too
        target_link_options(${target} PRIVATE -fuse-ld=lld -rdynamic -g -Wl,--disable-new-dtags
            "-Wl,-rpath,\$ORIGIN/../lib")
    endif()

    if(WIN32)
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
    else()
        # Linux: <target>.so beside the executable, which opens it with dlopen; its calls into the executable are
        # bound then. Same flags as the first start's build (src/linux/game_code.cpp)
        add_library(${target}_code SHARED ${GEN_SOURCES})
        set_target_properties(${target}_code PROPERTIES OUTPUT_NAME ${target} PREFIX "" CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        add_dependencies(${target}_code recompile_${target})
        target_include_directories(${target}_code PRIVATE src ${GEN})
        target_include_directories(${target}_code SYSTEM PRIVATE src/cpu/freestanding)
        target_compile_definitions(${target}_code PRIVATE X86_GAME_CODE NDEBUG)
        target_compile_options(${target}_code PRIVATE -O2 -fno-strict-aliasing -fno-stack-protector -nostdinc -nostdinc++ -w)
        target_link_options(${target}_code PRIVATE -fuse-ld=lld -Wl,--icf=none)
        target_link_libraries(${target}_code PRIVATE m)
        # after the executable: a .so newer than it is current (src/linux/game_code.cpp)
        add_dependencies(${target}_code ${target})
        set_property(TARGET ${target}_code APPEND PROPERTY LINK_DEPENDS $<TARGET_FILE:${target}>)
    endif()
    # a stamp from an earlier build on the executable's first start would outrank this DLL
    add_custom_command(TARGET ${target}_code POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f ${COMPILER_DIR}/work/${target}/built.txt)
endfunction()

scda_image(SCDA default.xbe scda_v2.toml 128 game)        # solo and co-op
scda_image(SCDA_Versus sc4_adv.xbe sc4_adv.toml 64 versus)  # versus

if(NOT WIN32)
    # Linux: the launcher an AppImage opens with (src/launcher): picks the game files, display settings, starts a game
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/launcher_xbe.cpp CONTENT
        "extern const char kLauncherGameSha1[] = \"${SCDA_SHA1}\";\nextern const char kLauncherVersusSha1[] = \"${SCDA_Versus_SHA1}\";\n")
    add_executable(SCDA_Launcher src/launcher/launcher.cpp src/linux/paths.cpp src/kernel/disc.cpp src/kernel/crypto.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp ${CMAKE_CURRENT_BINARY_DIR}/launcher_xbe.cpp)
    target_include_directories(SCDA_Launcher PRIVATE src)
    target_include_directories(SCDA_Launcher SYSTEM PRIVATE ${imgui_SOURCE_DIR} ${sdl3_SOURCE_DIR}/include)
    target_link_libraries(SCDA_Launcher PRIVATE tomlplusplus::tomlplusplus SDL3::SDL3-static)
    target_link_options(SCDA_Launcher PRIVATE -fuse-ld=lld)
endif()
