cmake_minimum_required(VERSION 3.18)

# Prevent CMake from picking up GCC-style env flags when targeting MSVC
if(WIN32)
    set(ENV{CFLAGS} "")
    set(ENV{CXXFLAGS} "")
    set(ENV{LDFLAGS} "")

    set(CMAKE_CXX_FLAGS "/W3" CACHE STRING "CXX compiler flags" FORCE)
    set(CMAKE_CXX_FLAGS_DEBUG "/MDd /Zi /Ob0 /Od /RTC1" CACHE STRING "CXX debug flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "/MD /O2 /Ob2 /DNDEBUG" CACHE STRING "CXX release flags" FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/MD /Zi /O2 /Ob1 /DNDEBUG" CACHE STRING "CXX release with debug info flags" FORCE)
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG" CACHE STRING "CXX min size release flags" FORCE)
endif()

project(cocotb LANGUAGES CXX)

if(MSVC)
    message(STATUS "MSVC detected - using forced MSVC-compatible flags")
    message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_SKIP_RPATH TRUE)

# Request Development.Module, not Development. The latter also requires
# Development.Embed / libpython, which manylinux images intentionally omit.
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(MSVC)
    set(EXTRA_CXX_FLAGS "/permissive-" "/W4")
else()
    set(BASE_WARNS
        "-Wall" "-Wextra" "-Wcast-qual" "-Wwrite-strings" "-Wconversion"
        "-Wno-missing-field-initializers" "-Werror=shadow"
    )
    set(CXX_WARNS ${BASE_WARNS} "-Wnon-virtual-dtor" "-Woverloaded-virtual")
    set(EXTRA_CXX_FLAGS
        "-std=c++11" "-fvisibility=hidden" "-fvisibility-inlines-hidden"
        ${CXX_WARNS}
    )
    if(NOT WIN32)
        list(APPEND EXTRA_CXX_FLAGS "-flto")
    endif()

    # WIN32 intentionally gets no extra linker flags: the family libraries must
    # export the bootstrap entry points, the COCOTB_EXPORT-marked interface
    # layers and the startup routine arrays the simulator looks up by name, so
    # -Wl,--exclude-all-symbols would break them.
    if(NOT WIN32)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -flto")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")
        endif()
    endif()
endif()

set(EXTRA_DEFINES "__STDC_FORMAT_MACROS")

set(INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/src/cocotb/share/include
    ${CMAKE_SOURCE_DIR}/src/cocotb
    ${Python_INCLUDE_DIRS}
)

set(SHARE_LIB_DIR ${CMAKE_SOURCE_DIR}/src/cocotb/share/lib)

function(add_cocotb_library TARGET_NAME)
    set(options "")
    set(oneValueArgs "OUTPUT_NAME")
    set(multiValueArgs "SOURCES" "LIBRARIES" "DEFINES")
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_library(${TARGET_NAME} SHARED ${ARG_SOURCES})

    if(SKBUILD_STATE STREQUAL "editable")
        set_target_properties(${TARGET_NAME} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src/cocotb/libs
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src/cocotb/libs
        )
    endif()

    if(ARG_OUTPUT_NAME)
        set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
    endif()

    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".so")
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE ${INCLUDE_DIRS})
    target_compile_definitions(${TARGET_NAME} PRIVATE ${ARG_DEFINES} ${EXTRA_DEFINES})
    target_compile_options(${TARGET_NAME} PRIVATE ${EXTRA_CXX_FLAGS})

    if(ARG_LIBRARIES)
        target_link_libraries(${TARGET_NAME} PRIVATE ${ARG_LIBRARIES})
    endif()

    if(MSVC)
        # pyconfig.h nominates the interpreter's import library for the link
        # with `#pragma comment(lib, ...)`, which resolves to pythonXY.lib
        # (pythonXYt.lib in a free-threaded build, the versioned _d.lib name
        # when _DEBUG). Cancel those implicit default libraries: cocotb
        # resolves every Python C API symbol at runtime and never links
        # libpython, so a symbol referenced statically must fail as an
        # unresolved external instead of being satisfied by libpython
        # silently. This is the Windows counterpart of the
        # -Wl,--no-undefined gate the Unix builds use.
        target_link_options(${TARGET_NAME} PRIVATE
            "/NODEFAULTLIB:python3.lib"
            "/NODEFAULTLIB:python3t.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}t.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}_d.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}t_d.lib"
        )
    endif()

    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE WIN32)
    endif()
endfunction()

set(GPI_LINK_LIBS "")
if(UNIX)
    list(APPEND GPI_LINK_LIBS dl)
elseif(WIN32)
    # utils_lookup_global_sym() enumerates the process modules with psapi.
    list(APPEND GPI_LINK_LIBS psapi)
endif()

# Everything every simulator family library carries: the bootstrap entry point
# (and the vlog/vhpi startup routine arrays simulators look up by name), the GPI
# core, and PyGPI. The same file is loaded twice in a simulator process - once
# as the VPI/VHPI/FLI object and once as the COCOTB_BOOTSTRAP entry that starts
# Python - which is why the layers cannot be separate libraries anymore.
set(COCOTB_FAMILY_SOURCES
    ${SHARE_LIB_DIR}/gpi/bootstrapper.cpp
    ${SHARE_LIB_DIR}/gpi/dynload.cpp
    ${SHARE_LIB_DIR}/gpi/GpiCbHdl.cpp
    ${SHARE_LIB_DIR}/gpi/GpiCommon.cpp
    ${SHARE_LIB_DIR}/gpi/logging.cpp
    ${SHARE_LIB_DIR}/pygpi/bind.cpp
    ${SHARE_LIB_DIR}/pygpi/embed.cpp
    ${SHARE_LIB_DIR}/pygpi/logging.cpp
    ${SHARE_LIB_DIR}/pygpi/pyapi.cpp
)

# The per-interface layers. Each starts with its dispatch unit, which is what
# lets a library call the simulator's own VPI/VHPI/FLI entry points without
# linking against them: the table is filled from the process with
# dlsym(RTLD_DEFAULT) (POSIX) or EnumProcessModules (Windows) at load time.
set(COCOTB_VPI_SOURCES
    ${SHARE_LIB_DIR}/gpi/abi/vpi_dispatch.cpp
    ${SHARE_LIB_DIR}/gpi/vpi/VpiImpl.cpp
    ${SHARE_LIB_DIR}/gpi/vpi/VpiCbHdl.cpp
    ${SHARE_LIB_DIR}/gpi/vpi/VpiObj.cpp
    ${SHARE_LIB_DIR}/gpi/vpi/VpiIterator.cpp
    ${SHARE_LIB_DIR}/gpi/vpi/VpiSignal.cpp
)

set(COCOTB_VHPI_SOURCES
    ${SHARE_LIB_DIR}/gpi/abi/vhpi_dispatch.cpp
    ${SHARE_LIB_DIR}/gpi/vhpi/VhpiImpl.cpp
    ${SHARE_LIB_DIR}/gpi/vhpi/VhpiCbHdl.cpp
    ${SHARE_LIB_DIR}/gpi/vhpi/VhpiObj.cpp
    ${SHARE_LIB_DIR}/gpi/vhpi/VhpiIterator.cpp
    ${SHARE_LIB_DIR}/gpi/vhpi/VhpiSignal.cpp
)

set(COCOTB_FLI_SOURCES
    ${SHARE_LIB_DIR}/gpi/abi/fli_dispatch.cpp
    ${SHARE_LIB_DIR}/gpi/fli/FliImpl.cpp
    ${SHARE_LIB_DIR}/gpi/fli/FliCbHdl.cpp
    ${SHARE_LIB_DIR}/gpi/fli/FliObjHdl.cpp
)

# Filled in by the embedding block below.
set(COCOTB_FAMILY_DEFINES "")
set(COCOTB_FAMILY_EXTRA_SOURCES "")

# Embed the Python packages of src/ into every family library, so a built tree
# (and a wheel) runs simulations without a pip-installed cocotb. Editable
# installs default to off: their libraries are loaded from the source tree,
# where the interpreter already finds the packages.
option(COCOTB_EMBED_PYTHON "Embed the cocotb Python packages into the simulator libraries" ON)
if(SKBUILD_STATE STREQUAL "editable")
    set_property(CACHE COCOTB_EMBED_PYTHON PROPERTY VALUE OFF)
endif()

if(COCOTB_EMBED_PYTHON)
    set(COCOTB_EMBED_ZIP "${CMAKE_CURRENT_BINARY_DIR}/cocotb-embed.zip")
    file(GLOB_RECURSE COCOTB_EMBED_ZIP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.pyi"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*/py.typed"
    )
    list(FILTER COCOTB_EMBED_ZIP_SOURCES EXCLUDE REGEX "/__pycache__/")

    # Version recorded in the embedded package metadata. Wheel builds take it
    # from scikit-build-core; plain CMake builds reproduce the same string from
    # the checkout, so cocotb.__version__ is not "0.0.0+unknown" there either.
    set(COCOTB_VERSION "" CACHE STRING
        "Version recorded in the embedded Python package metadata (default: build backend version, else git-derived, else the VERSION file)")
    set(COCOTB_EMBED_ZIP_VERSION "")
    if(SKBUILD_PROJECT_VERSION_FULL)
        set(COCOTB_EMBED_ZIP_VERSION "${SKBUILD_PROJECT_VERSION_FULL}")
    elseif(SKBUILD_PROJECT_VERSION)
        set(COCOTB_EMBED_ZIP_VERSION "${SKBUILD_PROJECT_VERSION}")
    elseif(NOT "$ENV{SKBUILD_PROJECT_VERSION_FULL}" STREQUAL "")
        set(COCOTB_EMBED_ZIP_VERSION "$ENV{SKBUILD_PROJECT_VERSION_FULL}")
    elseif(NOT "$ENV{SKBUILD_PROJECT_VERSION}" STREQUAL "")
        set(COCOTB_EMBED_ZIP_VERSION "$ENV{SKBUILD_PROJECT_VERSION}")
    elseif(NOT COCOTB_VERSION STREQUAL "")
        set(COCOTB_EMBED_ZIP_VERSION "${COCOTB_VERSION}")
    else()
        # Mirrors [tool.setuptools-git-versioning] in pyproject.toml:
        # {VERSION}.dev{commits since VERSION changed}+r{sha}[.dirty]
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" COCOTB_EMBED_ZIP_VERSION)
        string(STRIP "${COCOTB_EMBED_ZIP_VERSION}" COCOTB_EMBED_ZIP_VERSION)
        find_package(Git QUIET)
        if(Git_FOUND)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" log -1 --format=%H -- VERSION
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE _version_commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _git_rc
                ERROR_QUIET
            )
            if(_git_rc EQUAL 0 AND NOT _version_commit STREQUAL "")
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" rev-list --count "${_version_commit}..HEAD"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _commit_count
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _git_rc
                    ERROR_QUIET
                )
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _head_sha
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _git_rc
                    ERROR_QUIET
                )
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _dirty
                    RESULT_VARIABLE _git_rc
                    ERROR_QUIET
                )
                if(_git_rc EQUAL 0 AND _commit_count MATCHES "^[0-9]+$" AND NOT _head_sha STREQUAL "")
                    string(APPEND COCOTB_EMBED_ZIP_VERSION ".dev${_commit_count}+r${_head_sha}")
                    if(NOT _dirty STREQUAL "")
                        string(APPEND COCOTB_EMBED_ZIP_VERSION ".dirty")
                    endif()
                endif()
            endif()
        endif()
    endif()
    if(COCOTB_EMBED_ZIP_VERSION STREQUAL "")
        set(COCOTB_EMBED_ZIP_VERSION_ARGS "")
    else()
        message(STATUS "Embedded Python package version: ${COCOTB_EMBED_ZIP_VERSION}")
        set(COCOTB_EMBED_ZIP_VERSION_ARGS --version "${COCOTB_EMBED_ZIP_VERSION}")
    endif()

    # The zip content depends on the version string and on the exact set of
    # source files, neither of which is a file the custom command can depend
    # on: an up-to-date OUTPUT is not rebuilt just because its command changed,
    # so a reused build directory would keep shipping the old metadata and
    # files. Record both in a stamp that only changes when they do.
    set(COCOTB_EMBED_ZIP_STAMP "${CMAKE_CURRENT_BINARY_DIR}/cocotb-embed.stamp")
    set(_embed_zip_stamp "${COCOTB_EMBED_ZIP_VERSION}\n")
    foreach(_embed_zip_source IN LISTS COCOTB_EMBED_ZIP_SOURCES)
        string(APPEND _embed_zip_stamp "${_embed_zip_source}\n")
    endforeach()
    if(EXISTS "${COCOTB_EMBED_ZIP_STAMP}")
        file(READ "${COCOTB_EMBED_ZIP_STAMP}" _embed_zip_stamp_old)
    else()
        set(_embed_zip_stamp_old "")
    endif()
    if(NOT _embed_zip_stamp_old STREQUAL _embed_zip_stamp)
        file(WRITE "${COCOTB_EMBED_ZIP_STAMP}" "${_embed_zip_stamp}")
    endif()

    add_custom_command(
        OUTPUT "${COCOTB_EMBED_ZIP}"
        COMMAND ${Python_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_embed_zip.py"
                --src-dir "${CMAKE_CURRENT_SOURCE_DIR}/src"
                --output "${COCOTB_EMBED_ZIP}"
                ${COCOTB_EMBED_ZIP_VERSION_ARGS}
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_embed_zip.py"
            "${COCOTB_EMBED_ZIP_STAMP}"
            ${COCOTB_EMBED_ZIP_SOURCES}
        COMMENT "Generating cocotb-embed.zip"
        VERBATIM
    )
    add_custom_target(cocotb_embed_zip DEPENDS "${COCOTB_EMBED_ZIP}")

    list(APPEND COCOTB_FAMILY_EXTRA_SOURCES ${SHARE_LIB_DIR}/pygpi/embed_zip.cpp)
    string(APPEND COCOTB_FAMILY_DEFINES " COCOTB_HAVE_EMBEDDED_ZIP")

    if(WIN32)
        # A resource is the portable way to carry the bytes on Windows: the
        # library is a PE image, so there is no assembler include available.
        enable_language(RC)
        # Forward slashes: rc.exe accepts them, and backslashes would need
        # escaping inside the quoted filename.
        set(COCOTB_EMBED_ZIP_RC "${CMAKE_CURRENT_BINARY_DIR}/cocotb_embed_zip.rc")
        set(_embed_zip_rc_body "IDR_COCOTBEMBEDZIP RCDATA \"${COCOTB_EMBED_ZIP}\"\n")
        if(EXISTS "${COCOTB_EMBED_ZIP_RC}")
            file(READ "${COCOTB_EMBED_ZIP_RC}" _embed_zip_rc_old)
        else()
            set(_embed_zip_rc_old "")
        endif()
        if(NOT _embed_zip_rc_old STREQUAL _embed_zip_rc_body)
            file(WRITE "${COCOTB_EMBED_ZIP_RC}" "${_embed_zip_rc_body}")
        endif()
        list(APPEND COCOTB_FAMILY_EXTRA_SOURCES "${COCOTB_EMBED_ZIP_RC}")
    else()
        enable_language(ASM)
        if(APPLE)
            set(_embed_zip_sym "_")
            set(_embed_zip_section ".section __TEXT,__const")
        else()
            set(_embed_zip_sym "")
            set(_embed_zip_section ".section .rodata")
        endif()
        set(COCOTB_EMBED_ZIP_ASM "${CMAKE_CURRENT_BINARY_DIR}/cocotb_embed_zip.s")
        set(_embed_zip_asm_body
            "/* Generated by CMake: the cocotb package zip embedded in this library. */\n")
        string(APPEND _embed_zip_asm_body "${_embed_zip_section}\n")
        string(APPEND _embed_zip_asm_body ".globl ${_embed_zip_sym}cocotb_zip_start\n")
        string(APPEND _embed_zip_asm_body ".globl ${_embed_zip_sym}cocotb_zip_end\n")
        string(APPEND _embed_zip_asm_body "${_embed_zip_sym}cocotb_zip_start:\n")
        string(APPEND _embed_zip_asm_body ".incbin \"${COCOTB_EMBED_ZIP}\"\n")
        string(APPEND _embed_zip_asm_body "${_embed_zip_sym}cocotb_zip_end:\n")
        if(NOT APPLE)
            string(APPEND _embed_zip_asm_body
                ".section .note.GNU-stack,\"\",@progbits\n")
        endif()
        if(EXISTS "${COCOTB_EMBED_ZIP_ASM}")
            file(READ "${COCOTB_EMBED_ZIP_ASM}" _embed_zip_asm_old)
        else()
            set(_embed_zip_asm_old "")
        endif()
        if(NOT _embed_zip_asm_old STREQUAL _embed_zip_asm_body)
            file(WRITE "${COCOTB_EMBED_ZIP_ASM}" "${_embed_zip_asm_body}")
        endif()
        # The .s content only names the zip, so refresh its timestamp when the
        # zip changes to force a re-assemble.
        add_custom_command(
            OUTPUT "${COCOTB_EMBED_ZIP_ASM}"
            COMMAND ${CMAKE_COMMAND} -E touch "${COCOTB_EMBED_ZIP_ASM}"
            DEPENDS "${COCOTB_EMBED_ZIP}"
            COMMENT "Refreshing cocotb_embed_zip.s for the embedded zip"
            VERBATIM
        )
        list(APPEND COCOTB_FAMILY_EXTRA_SOURCES "${COCOTB_EMBED_ZIP_ASM}")
    endif()
else()
    message(STATUS "Embedding the cocotb Python packages: OFF")
endif()

# One library per simulator family: everything a simulator needs from cocotb,
# including every interface it can drive. All of them are built into a single
# file, so a simulator that loads two interfaces loads the same library twice
# and the entry symbols are distinct.
#
#   add_family_library(<family> <SIM_DEFINE>
#       INTERFACES <VPI|VHPI|FLI>...
#       [EXTRA_LIBS ...] [EXTRA_LIB_DIRS ...])
function(add_family_library FAMILY SIM_DEFINE)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" ""
        "INTERFACES;EXTRA_LIBS;EXTRA_LIB_DIRS")

    set(TARGET_NAME cocotb_${FAMILY})
    set(SOURCES ${COCOTB_FAMILY_SOURCES} ${COCOTB_FAMILY_EXTRA_SOURCES})
    set(DEFINES GPI_EXPORTS PYGPI_EXPORTS ${SIM_DEFINE} ${COCOTB_FAMILY_DEFINES})

    foreach(INTERFACE IN LISTS ARG_INTERFACES)
        string(TOUPPER "${INTERFACE}" INTERFACE_UPPER)
        list(APPEND SOURCES ${COCOTB_${INTERFACE_UPPER}_SOURCES})
        list(APPEND DEFINES COCOTB_${INTERFACE_UPPER}_DYN)
        if(INTERFACE_UPPER STREQUAL "FLI")
            # FliImpl.hpp maps COCOTBFLI_EXPORT to COCOTB_EXPORT only for the
            # library that defines the FLI entry points.
            list(APPEND DEFINES COCOTBFLI_EXPORTS)
        endif()
    endforeach()

    add_cocotb_library(${TARGET_NAME}
        SOURCES ${SOURCES}
        DEFINES ${DEFINES}
        LIBRARIES ${GPI_LINK_LIBS} ${ARG_EXTRA_LIBS}
    )

    if(COCOTB_EMBED_PYTHON)
        add_dependencies(${TARGET_NAME} cocotb_embed_zip)
    endif()

    if(ARG_EXTRA_LIB_DIRS)
        target_link_directories(${TARGET_NAME} PRIVATE ${ARG_EXTRA_LIB_DIRS})
    endif()

    # The simulator's own symbols arrive through the abi dispatch tables and
    # libpython's through pygpi::api, so an unresolved symbol here means one of
    # those tables is incomplete rather than that a platform is unsupported.
    if(APPLE)
        target_link_options(${TARGET_NAME} PRIVATE -undefined dynamic_lookup)
    elseif(NOT WIN32)
        target_link_options(${TARGET_NAME} PRIVATE -Wl,--no-undefined)
    endif()
endfunction()

set(ICARUS_EXTRA_LIBS "")
if(WIN32)
    set(ICARUS_EXTRA_LIBS icarus)
endif()
add_family_library(icarus ICARUS
    INTERFACES VPI
    EXTRA_LIBS ${ICARUS_EXTRA_LIBS}
)

set(MODELSIM_EXTRA_LIBS "")
if(WIN32)
    set(MODELSIM_EXTRA_LIBS modelsim)
endif()
add_family_library(modelsim MODELSIM
    INTERFACES VPI VHPI FLI
    EXTRA_LIBS ${MODELSIM_EXTRA_LIBS}
)

set(GHDL_EXTRA_LIBS "")
if(WIN32)
    set(GHDL_EXTRA_LIBS ghdl)
endif()
add_family_library(ghdl GHDL
    INTERFACES VPI
    EXTRA_LIBS ${GHDL_EXTRA_LIBS}
)

set(NVC_EXTRA_LIBS "")
if(WIN32)
    set(NVC_EXTRA_LIBS nvcvhpi)
endif()
add_family_library(nvc NVC
    INTERFACES VHPI
    EXTRA_LIBS ${NVC_EXTRA_LIBS}
)

set(ALDEC_EXTRA_LIBS "")
if(WIN32)
    set(ALDEC_EXTRA_LIBS aldec)
endif()
add_family_library(aldec ALDEC
    INTERFACES VPI VHPI
    EXTRA_LIBS ${ALDEC_EXTRA_LIBS}
)

if(UNIX)
    add_family_library(ius IUS INTERFACES VPI VHPI)
    add_family_library(vcs VCS INTERFACES VPI)
    add_family_library(verilator VERILATOR INTERFACES VPI)
    add_family_library(dsim DSim INTERFACES VPI)
endif()

if(SKBUILD AND NOT SKBUILD_STATE STREQUAL "editable")
    # One library per simulator family, one directory. There is no separate
    # Python extension: `cocotb.simulator` is a builtin module registered by
    # pygpi_initialize() in these same files.
    set(COCOTB_FAMILY_TARGETS
        cocotb_icarus cocotb_modelsim cocotb_ghdl cocotb_aldec cocotb_nvc)

    if(UNIX)
        list(APPEND COCOTB_FAMILY_TARGETS
            cocotb_ius cocotb_vcs cocotb_verilator cocotb_dsim)
    endif()

    install(TARGETS ${COCOTB_FAMILY_TARGETS} DESTINATION cocotb/libs OPTIONAL)
endif()
