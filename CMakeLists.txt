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
    set(PYTHON_EXT_SUFFIX ".${Python_SOABI}.pyd")
else()
    set(PYTHON_EXT_SUFFIX ".${Python_SOABI}.so")
endif()

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

    if(WIN32)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-all-symbols")
    else()
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
set(COCOTB_DEF_DIR ${CMAKE_SOURCE_DIR}/src/cocotb/share/def)

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

    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE WIN32)
    endif()
endfunction()

set(GPI_LINK_LIBS "")
if(UNIX)
    list(APPEND GPI_LINK_LIBS dl)
endif()

add_cocotb_library(cocotb_bootstrap
    SOURCES
        ${SHARE_LIB_DIR}/gpi/dynload.cpp
        ${SHARE_LIB_DIR}/gpi/bootstrapper.cpp
    LIBRARIES ${GPI_LINK_LIBS}
)

add_cocotb_library(gpi
    SOURCES
        ${SHARE_LIB_DIR}/gpi/GpiCbHdl.cpp
        ${SHARE_LIB_DIR}/gpi/GpiCommon.cpp
        ${SHARE_LIB_DIR}/gpi/dynload.cpp
        ${SHARE_LIB_DIR}/gpi/logging.cpp
    DEFINES GPI_EXPORTS
    LIBRARIES ${GPI_LINK_LIBS}
)

add_cocotb_library(simulator
    SOURCES
        ${SHARE_LIB_DIR}/pygpi/bind.cpp
        ${SHARE_LIB_DIR}/pygpi/embed.cpp
        ${SHARE_LIB_DIR}/pygpi/logging.cpp
    DEFINES PYGPI_EXPORTS
    LIBRARIES gpi
)
# Python::Module links the import library on Windows and leaves Python
# symbols unresolved on Unix (libpython is preloaded at runtime).
target_link_libraries(simulator PRIVATE Python::Module)

if(SKBUILD_STATE STREQUAL "editable")
    set_target_properties(simulator PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src/cocotb
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/src/cocotb
    )
else()
    set_target_properties(simulator PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cocotb
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/cocotb
    )
endif()

set_target_properties(simulator PROPERTIES
    PREFIX ""
    SUFFIX ${PYTHON_EXT_SUFFIX}
)

# Embed the Python packages of src/ into the PyGPI library, so a built tree (and
# a wheel) runs simulations without a pip-installed cocotb. Editable installs
# default to off: their libraries are loaded from the source tree, where the
# interpreter already finds the packages.
option(COCOTB_EMBED_PYTHON "Embed the cocotb Python packages into the PyGPI library" ON)
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

    target_sources(simulator PRIVATE ${SHARE_LIB_DIR}/pygpi/embed_zip.cpp)
    target_compile_definitions(simulator PRIVATE COCOTB_HAVE_EMBEDDED_ZIP)
    add_dependencies(simulator cocotb_embed_zip)

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
        target_sources(simulator PRIVATE "${COCOTB_EMBED_ZIP_RC}")
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
        target_sources(simulator PRIVATE "${COCOTB_EMBED_ZIP_ASM}")
    endif()
else()
    message(STATUS "Embedding the cocotb Python packages: OFF")
endif()

function(add_vpi_library SIM_NAME SIM_DEFINE)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs "EXTRA_LIBS" "EXTRA_LIB_DIRS")
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(TOLOWER ${SIM_NAME} SIM_LOWER)
    set(TARGET_NAME gpi_impl_${SIM_LOWER}_vpi)

    add_cocotb_library(${TARGET_NAME}
        SOURCES
            ${SHARE_LIB_DIR}/gpi/vpi/VpiImpl.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiCbHdl.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiObj.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiIterator.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiSignal.cpp
        DEFINES COCOTBVPI_EXPORTS GPI_IMPL_ENTRY=gpi_impl_${SIM_LOWER}_vpi_entry ${SIM_DEFINE}
        LIBRARIES gpi ${ARG_EXTRA_LIBS}
    )

    if(WIN32)
        target_link_directories(${TARGET_NAME} PRIVATE ${COCOTB_DEF_DIR})
    endif()

    if(ARG_EXTRA_LIB_DIRS)
        target_link_directories(${TARGET_NAME} PRIVATE ${ARG_EXTRA_LIB_DIRS})
    endif()

    if(NOT WIN32)
        if(APPLE)
            target_link_options(${TARGET_NAME} PRIVATE -undefined dynamic_lookup)
        else()
            target_link_options(${TARGET_NAME} PRIVATE -Wl,--allow-shlib-undefined)
        endif()
    endif()
endfunction()

function(add_vhpi_library SIM_NAME SIM_DEFINE)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs "EXTRA_LIBS" "EXTRA_LIB_DIRS")
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(TOLOWER ${SIM_NAME} SIM_LOWER)
    set(TARGET_NAME gpi_impl_${SIM_LOWER}_vhpi)

    add_cocotb_library(${TARGET_NAME}
        SOURCES
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiImpl.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiCbHdl.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiObj.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiIterator.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiSignal.cpp
        DEFINES COCOTBVHPI_EXPORTS GPI_IMPL_ENTRY=gpi_impl_${SIM_LOWER}_vhpi_entry ${SIM_DEFINE}
        LIBRARIES gpi ${ARG_EXTRA_LIBS}
    )

    if(WIN32)
        target_link_directories(${TARGET_NAME} PRIVATE ${COCOTB_DEF_DIR})
    endif()

    if(ARG_EXTRA_LIB_DIRS)
        target_link_directories(${TARGET_NAME} PRIVATE ${ARG_EXTRA_LIB_DIRS})
    endif()

    if(NOT WIN32)
        if(APPLE)
            target_link_options(${TARGET_NAME} PRIVATE -undefined dynamic_lookup)
        else()
            target_link_options(${TARGET_NAME} PRIVATE -Wl,--allow-shlib-undefined)
        endif()
    endif()
endfunction()

set(ICARUS_EXTRA_LIBS "")
if(WIN32)
    set(ICARUS_EXTRA_LIBS icarus)
endif()
add_vpi_library(ICARUS ICARUS EXTRA_LIBS ${ICARUS_EXTRA_LIBS})

set(MODELSIM_EXTRA_LIBS "")
if(WIN32)
    set(MODELSIM_EXTRA_LIBS modelsim)
endif()

add_vpi_library(MODELSIM MODELSIM EXTRA_LIBS ${MODELSIM_EXTRA_LIBS})
add_vhpi_library(MODELSIM MODELSIM EXTRA_LIBS ${MODELSIM_EXTRA_LIBS})

add_cocotb_library(gpi_impl_modelsim_fli
    SOURCES
        ${SHARE_LIB_DIR}/gpi/fli/FliImpl.cpp
        ${SHARE_LIB_DIR}/gpi/fli/FliCbHdl.cpp
        ${SHARE_LIB_DIR}/gpi/fli/FliObjHdl.cpp
    DEFINES COCOTBFLI_EXPORTS GPI_IMPL_ENTRY=gpi_impl_modelsim_fli_entry
    LIBRARIES gpi ${MODELSIM_EXTRA_LIBS}
)

if(WIN32)
    target_link_directories(gpi_impl_modelsim_fli PRIVATE ${COCOTB_DEF_DIR})
endif()

if(NOT WIN32)
    if(APPLE)
        target_link_options(gpi_impl_modelsim_fli PRIVATE -undefined dynamic_lookup)
    else()
        target_link_options(gpi_impl_modelsim_fli PRIVATE -Wl,--allow-shlib-undefined)
    endif()
endif()

set(GHDL_EXTRA_LIBS "")
if(WIN32)
    set(GHDL_EXTRA_LIBS ghdl)
endif()
add_vpi_library(GHDL GHDL EXTRA_LIBS ${GHDL_EXTRA_LIBS})

if(UNIX)
    add_vpi_library(IUS IUS)
    add_vhpi_library(IUS IUS)
    add_vpi_library(VCS VCS)
    add_vpi_library(VERILATOR VERILATOR)
    add_vpi_library(DSim DSim)
endif()

set(NVC_EXTRA_LIBS "")
if(WIN32)
    set(NVC_EXTRA_LIBS nvcvhpi)
endif()
add_vhpi_library(NVC NVC EXTRA_LIBS ${NVC_EXTRA_LIBS})

set(ALDEC_EXTRA_LIBS "")
if(WIN32)
    set(ALDEC_EXTRA_LIBS aldec)
endif()
add_vpi_library(ALDEC ALDEC EXTRA_LIBS ${ALDEC_EXTRA_LIBS})
add_vhpi_library(ALDEC ALDEC EXTRA_LIBS ${ALDEC_EXTRA_LIBS})

if(WIN32)
    if(MSVC)
        foreach(SIM icarus modelsim aldec ghdl nvcvhpi)
            add_custom_command(
                OUTPUT ${COCOTB_DEF_DIR}/${SIM}.lib
                COMMAND lib /def:${COCOTB_DEF_DIR}/${SIM}.def /out:${COCOTB_DEF_DIR}/${SIM}.lib /machine:$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,X64,X86>
                DEPENDS ${COCOTB_DEF_DIR}/${SIM}.def
                COMMENT "Generating import library for ${SIM}"
            )
            add_custom_target(${SIM}_import_lib ALL DEPENDS ${COCOTB_DEF_DIR}/${SIM}.lib)
        endforeach()
    else()
        foreach(SIM icarus modelsim aldec ghdl nvcvhpi)
            add_custom_command(
                OUTPUT ${COCOTB_DEF_DIR}/lib${SIM}.a
                COMMAND dlltool -d ${COCOTB_DEF_DIR}/${SIM}.def -l ${COCOTB_DEF_DIR}/lib${SIM}.a
                DEPENDS ${COCOTB_DEF_DIR}/${SIM}.def
                COMMENT "Generating import library for ${SIM}"
            )
            add_custom_target(${SIM}_import_lib ALL DEPENDS ${COCOTB_DEF_DIR}/lib${SIM}.a)
        endforeach()
    endif()
endif()

if(WIN32)
    add_dependencies(gpi_impl_icarus_vpi icarus_import_lib)
    add_dependencies(gpi_impl_modelsim_vpi modelsim_import_lib)
    add_dependencies(gpi_impl_modelsim_vhpi modelsim_import_lib)
    add_dependencies(gpi_impl_modelsim_fli modelsim_import_lib)
    add_dependencies(gpi_impl_ghdl_vpi ghdl_import_lib)
    add_dependencies(gpi_impl_nvc_vhpi nvcvhpi_import_lib)
    add_dependencies(gpi_impl_aldec_vpi aldec_import_lib)
    add_dependencies(gpi_impl_aldec_vhpi aldec_import_lib)
endif()

if(SKBUILD AND NOT SKBUILD_STATE STREQUAL "editable")
    install(TARGETS cocotb_bootstrap gpi DESTINATION cocotb/libs)

    set(SIM_TARGETS
        gpi_impl_icarus_vpi gpi_impl_modelsim_vpi gpi_impl_modelsim_vhpi
        gpi_impl_modelsim_fli gpi_impl_ghdl_vpi gpi_impl_aldec_vpi
        gpi_impl_aldec_vhpi gpi_impl_nvc_vhpi)

    if(UNIX)
        list(APPEND SIM_TARGETS
            gpi_impl_ius_vpi gpi_impl_ius_vhpi gpi_impl_vcs_vpi
            gpi_impl_verilator_vpi gpi_impl_dsim_vpi)
    endif()

    install(TARGETS ${SIM_TARGETS} DESTINATION cocotb/libs OPTIONAL)

    install(TARGETS simulator
        RUNTIME DESTINATION cocotb
        LIBRARY DESTINATION cocotb)
endif()
