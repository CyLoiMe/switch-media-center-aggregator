cmake_minimum_required(VERSION 3.18)

# SMCA — Switch Media Center Aggregator.
# One platform, one artifact: SMCA.nro for Nintendo Switch (Atmosphère).

set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Platform options (PLATFORM_SWITCH, USE_DEKO3D, …) come from borealis.
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

if (NOT PLATFORM_SWITCH)
    message(FATAL_ERROR
        "SMCA builds for Nintendo Switch only. Configure with:\n"
        "  cmake -B build -DPLATFORM_SWITCH=ON")
endif ()

# Nintendo Switch options
cmake_dependent_option(BUILTIN_NSP "Built in NSP forwarder" OFF "PLATFORM_SWITCH" OFF)
cmake_dependent_option(USE_LIBUSBHSFS "Use libusbhsfs for ums access" OFF "PLATFORM_SWITCH" OFF)

# Render the video with the CPU: slower, but works without a GPU path.
option(MPV_SW_RENDER "Using CPU to draw videos" OFF)
if (MPV_SW_RENDER)
    list(APPEND APP_PLATFORM_OPTION -DMPV_SW_RENDER)
endif ()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel." FORCE)
elseif (CMAKE_BUILD_TYPE STREQUAL Debug)
    list(APPEND APP_PLATFORM_OPTION -D_DEBUG -D_GLIBCXX_ASSERTIONS)
endif()

# Add git info
if (NOT DEFINED GIT_TAG_SHORT)
    execute_process(COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 5 OUTPUT_VARIABLE GIT_TAG_SHORT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()

if (NOT DEFINED VERSION_BUILD)
    execute_process(COMMAND git rev-list --count --all
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 5 OUTPUT_VARIABLE VERSION_BUILD
        OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()
message(STATUS "git commit ${GIT_TAG_SHORT} build ${VERSION_BUILD}")

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# project info
project(SMCA)  # short token for files/binaries/IDs; display name = "Switch Media Center Aggregator"
set(VERSION_MAJOR "0")
set(VERSION_MINOR "2")
set(VERSION_ALTER "1")
# Homebrew forwarder title ID, in the community forwarder range (05...), which is
# outside Nintendo's commercial 01... space — so it cannot collide with an
# official title, and no title-ID registry check is needed. The low 32 bits spell
# the app name in ASCII ("SMCA" = 534D4341), the same convention the rest of the
# homebrew scene uses. (hacbrewpack lower-cases the filename → see
# scripts/forwarder/CMakeLists.txt.)
set(PROJECT_TITLEID "0500534D43410000")
set(PROJECT_AUTHOR "CyLoiMe")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# Embed the changelog (single source of truth at the repo root) into the
# resources so the in-app Changelog screen can read it. Configure-time copy:
# editing CHANGELOG.md re-triggers CMake configure. The copy under resources/ is
# gitignored.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md ${PROJECT_RESOURCES}/CHANGELOG.md COPYONLY)

if (CUSTOM_RESOURCES_DIR)
    set(BRLS_RESOURCES_DIR ${CUSTOM_RESOURCES_DIR})
endif ()

# sources
file(GLOB_RECURSE MAIN_SRC "${PROJECT_SOURCE_DIR}/app/src/*.cpp")
# System service initialization functions required for libnx
list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)

if (USE_LIBROMFS)
    message(STATUS "USE LIBROMFS")
    add_definitions(-DUSE_LIBROMFS)
    set(LIBROMFS_PROJECT_NAME ${PROJECT_NAME})
    set(LIBROMFS_RESOURCE_LOCATION "${PROJECT_RESOURCES}")
endif ()

# build borealis and other third party libraries
add_subdirectory(library)
# yoga builds with -fno-rtti, so it emits its own hidden std::exception typeinfo, which
# the link then uses for every catch in the app: libc++ exceptions such as
# std::runtime_error no longer matched catch (const std::exception&)
target_compile_options(yogacore PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-frtti>)

# Cross compilation: the Switch portlibs are found through pkg-config. If a
# dependency is not found, point PKG_CONFIG_LIBDIR at the matching pkgconfig dir.
find_package(PkgConfig REQUIRED)

pkg_search_module(MPV REQUIRED mpv)
list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS})
list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARIES})
link_directories(${MPV_LIBRARY_DIRS})

pkg_search_module(LIBWEBP QUIET libwebp)
if (LIBWEBP_FOUND)
    message(STATUS "Found libwebp: ${LIBWEBP_INCLUDE_DIRS} ${LIBWEBP_LIBRARIES}")
    list(APPEND APP_PLATFORM_INCLUDE ${LIBWEBP_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${LIBWEBP_LIBRARIES})
    list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
    link_directories(${LIBWEBP_LIBRARY_DIRS})
endif ()

find_package(CURL REQUIRED)
list(APPEND APP_PLATFORM_LIB CURL::libcurl)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

if (BUILTIN_NSP)
    find_library(NSP_LIB NAMES nsp REQUIRED)
    list(APPEND APP_PLATFORM_LIB nsp)
    # FORWARDER_TITLEID: same source of truth as the NSP (PROJECT_TITLEID),
    # used by main.cpp to detect an already installed HOME tile (ns).
    list(APPEND APP_PLATFORM_OPTION -DBUILTIN_NSP -DFORWARDER_TITLEID=0x${PROJECT_TITLEID}ULL)
    add_subdirectory(scripts/forwarder)
endif ()

if (USE_LIBUSBHSFS)
    find_library(USBHSFS_LIB NAMES usbhsfs REQUIRED)
    list(APPEND APP_PLATFORM_LIB lwext4 ntfs-3g usbhsfs)
    list(APPEND APP_PLATFORM_OPTION -DUSE_LIBUSBHSFS)
endif ()

# building target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE app/include ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections -Wunused-variable ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE borealis lunasvg ${APP_PLATFORM_LIB})
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION=${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}
    APP_BUILD=${VERSION_BUILD}
    BUILD_TAG_SHORT=${GIT_TAG_SHORT}
    BUILD_PACKAGE_NAME=${PROJECT_NAME}
)

# building release file: the NRO, with the resources packed as romfs and the NACP
# (title, author, version) generated from the values above.
if (BUILTIN_NSP)
    add_dependencies(${PROJECT_NAME} forwarder.nsp)
endif ()
if (USE_DEKO3D)
    gen_dksh("${PROJECT_RESOURCES}/shaders")
endif ()
add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
    COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/resources/font/keymap*.ttf ${CMAKE_BINARY_DIR}/resources/font/*_fallback.ttf
    COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
)
