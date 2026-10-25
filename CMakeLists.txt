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

# Request Development.Module, not Development. The latter also requires
# Development.Embed / libpython, which manylinux images intentionally omit.
find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)

if(WIN32)
    set(LIB_EXT "dll")
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    set(LIB_EXT "so")
endif()

if(MSVC)
    set(EXTRA_CXX_FLAGS "/permissive-" "/W4")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MANIFEST:NO")
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

# A library built with the embedded zip runs testbenches in a Python
# environment that has no cocotb installed (see pygpi/embed_zip.cpp).
# Editable installs must serve the live sources, so they default to OFF.
if(SKBUILD_STATE STREQUAL "editable")
    set(COCOTB_EMBED_PYTHON_DEFAULT OFF)
else()
    set(COCOTB_EMBED_PYTHON_DEFAULT ON)
endif()
option(COCOTB_EMBED_PYTHON
    "Embed the cocotb Python packages into every simulator library"
    ${COCOTB_EMBED_PYTHON_DEFAULT})

set(COCOTB_EMBED_ZIP "")
set(COCOTB_EMBED_ZIP_ASM "")
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

    if(NOT WIN32)
        enable_language(ASM)
        if(APPLE)
            set(_embed_zip_sym "_")
            set(_embed_zip_section ".section __TEXT,__const")
        else()
            set(_embed_zip_sym "")
            set(_embed_zip_section ".section .rodata")
        endif()
        set(COCOTB_EMBED_ZIP_ASM "${CMAKE_CURRENT_BINARY_DIR}/cocotb_embed_zip.s")
        file(WRITE "${COCOTB_EMBED_ZIP_ASM}"
            "/* Generated by CMake: the cocotb package zip embedded in this library. */\n"
            "${_embed_zip_section}\n"
            ".globl ${_embed_zip_sym}cocotb_zip_start\n"
            ".globl ${_embed_zip_sym}cocotb_zip_end\n"
            "${_embed_zip_sym}cocotb_zip_start:\n"
            ".incbin \"${COCOTB_EMBED_ZIP}\"\n"
            "${_embed_zip_sym}cocotb_zip_end:\n"
        )
        if(NOT APPLE)
            file(APPEND "${COCOTB_EMBED_ZIP_ASM}"
                ".section .note.GNU-stack,\"\",@progbits\n")
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
    endif()
endif()

if(WIN32)
    function(create_sxs_assembly_manifest NAME FILENAME LIBRARIES DEPENDENCY_ONLY MANIFEST_VAR)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(ARCHITECTURE "amd64")
        else()
            set(ARCHITECTURE "x86")
        endif()

        set(DEPENDENCIES "")
        foreach(LIB ${LIBRARIES})
            string(APPEND DEPENDENCIES "        <dependency>\n")
            string(APPEND DEPENDENCIES "            <dependentAssembly>\n")
            string(APPEND DEPENDENCIES "                <assemblyIdentity name=\"${LIB}\" version=\"1.0.0.0\" type=\"win32\" processorArchitecture=\"${ARCHITECTURE}\" />\n")
            string(APPEND DEPENDENCIES "            </dependentAssembly>\n")
            string(APPEND DEPENDENCIES "        </dependency>\n")
        endforeach()

        if(NOT DEPENDENCY_ONLY)
            set(MANIFEST_BODY "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n")
            string(APPEND MANIFEST_BODY "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n")
            string(APPEND MANIFEST_BODY "    <assemblyIdentity name=\"${NAME}\" version=\"1.0.0.0\" type=\"win32\" processorArchitecture=\"${ARCHITECTURE}\" />\n")
            string(APPEND MANIFEST_BODY "    <file name=\"${FILENAME}\" />\n")
            string(APPEND MANIFEST_BODY "${DEPENDENCIES}")
            string(APPEND MANIFEST_BODY "</assembly>\n")
        else()
            set(MANIFEST_BODY "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n")
            string(APPEND MANIFEST_BODY "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n")
            string(APPEND MANIFEST_BODY "${DEPENDENCIES}")
            string(APPEND MANIFEST_BODY "</assembly>\n")
        endif()

        set(${MANIFEST_VAR} "${MANIFEST_BODY}" PARENT_SCOPE)
    endfunction()

    function(create_rc_file RC_FILENAME NAME FILENAME LIBRARIES RUNTIME_LIBRARIES)
        create_sxs_assembly_manifest(${NAME} ${FILENAME} "${LIBRARIES}" FALSE MANIFEST)

        string(REPLACE "\"" "\"\"" MANIFEST "${MANIFEST}")
        string(REPLACE "\n" "\\r\\n\"\n\"" MANIFEST "${MANIFEST}")
        set(MANIFEST "\"${MANIFEST}\"")

        set(RC_BODY "#pragma code_page(65001) // UTF-8\n")
        string(APPEND RC_BODY "#include <Windows.h>\n\n")
        string(APPEND RC_BODY "LANGUAGE 0x00, 0x00\n\n")
        string(APPEND RC_BODY "ISOLATIONAWARE_MANIFEST_RESOURCE_ID RT_MANIFEST\n")
        string(APPEND RC_BODY "BEGIN\n")
        string(APPEND RC_BODY "${MANIFEST}\n")
        string(APPEND RC_BODY "END\n")

        if(RUNTIME_LIBRARIES)
            create_sxs_assembly_manifest(${NAME} ${FILENAME} "${RUNTIME_LIBRARIES}" TRUE RUNTIME_MANIFEST)
            string(REPLACE "\"" "\"\"" RUNTIME_MANIFEST "${RUNTIME_MANIFEST}")
            string(REPLACE "\n" "\\r\\n\"\n\"" RUNTIME_MANIFEST "${RUNTIME_MANIFEST}")
            set(RUNTIME_MANIFEST "\"${RUNTIME_MANIFEST}\"")
            string(APPEND RC_BODY "\n1000 RT_MANIFEST\n")
            string(APPEND RC_BODY "BEGIN\n")
            string(APPEND RC_BODY "${RUNTIME_MANIFEST}\n")
            string(APPEND RC_BODY "END\n")
        endif()

        file(WRITE ${RC_FILENAME} "${RC_BODY}")
    endfunction()
endif()

function(add_cocotb_library TARGET_NAME)
    set(options "")
    set(oneValueArgs "OUTPUT_NAME")
    set(multiValueArgs "SOURCES" "LIBRARIES" "DEFINES" "RC_LIBRARIES" "RC_RUNTIME_LIBRARIES")
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Every cocotb interface library carries the GPI sources and the PyGPI
    # sources so that exactly one cocotb library file needs to be loaded per
    # simulation run (the GPI entry point starts the interpreter directly and
    # `import cocotb.simulator` resolves through the interpreter's built-in
    # module table, see pygpi/embed.cpp).
    set(ALL_SOURCES
        ${ARG_SOURCES}
        $<TARGET_OBJECTS:gpi>
        ${SHARE_LIB_DIR}/pygpi/bind.cpp
        ${SHARE_LIB_DIR}/pygpi/embed.cpp
        ${SHARE_LIB_DIR}/pygpi/embed_zip.cpp
        ${SHARE_LIB_DIR}/pygpi/logging.cpp
        ${SHARE_LIB_DIR}/pygpi/pyapi.cpp
    )
    if(WIN32)
        set(RC_FILENAME "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.rc")
        set(LIB_NAME ${ARG_OUTPUT_NAME})
        if(NOT LIB_NAME)
            set(LIB_NAME ${TARGET_NAME})
        endif()

        if(MSVC AND LIB_NAME MATCHES "^lib(.+)")
            string(REGEX REPLACE "^lib" "" LIB_NAME ${LIB_NAME})
        endif()

        set(RC_LIBS ${ARG_RC_LIBRARIES})
        if(MSVC)
            set(RC_LIBS_PROCESSED "")
            foreach(LIB ${RC_LIBS})
                if(LIB MATCHES "^lib(.+)")
                    string(REGEX REPLACE "^lib" "" LIB ${LIB})
                endif()
                list(APPEND RC_LIBS_PROCESSED ${LIB})
            endforeach()
            set(RC_LIBS ${RC_LIBS_PROCESSED})
        endif()

        set(RC_RUNTIME_LIBS ${ARG_RC_RUNTIME_LIBRARIES})
        if(MSVC AND RC_RUNTIME_LIBS)
            set(RC_RUNTIME_LIBS_PROCESSED "")
            foreach(LIB ${RC_RUNTIME_LIBS})
                if(LIB MATCHES "^lib(.+)")
                    string(REGEX REPLACE "^lib" "" LIB ${LIB})
                endif()
                list(APPEND RC_RUNTIME_LIBS_PROCESSED ${LIB})
            endforeach()
            set(RC_RUNTIME_LIBS ${RC_RUNTIME_LIBS_PROCESSED})
        endif()


        create_rc_file(
            ${RC_FILENAME} ${LIB_NAME} "${LIB_NAME}.${LIB_EXT}" "${RC_LIBS}" "${RC_RUNTIME_LIBS}"
        )
        if(COCOTB_EMBED_PYTHON)
            # `create_rc_file` rewrites the file on every configure, so this
            # append cannot duplicate the resource.
            file(APPEND "${RC_FILENAME}"
                "\n// The cocotb package zip generated by tools/make_embed_zip.py\n"
                "IDR_COCOTBEMBEDZIP RCDATA \"${COCOTB_EMBED_ZIP}\"\n"
            )
            add_custom_command(
                OUTPUT "${RC_FILENAME}"
                COMMAND ${CMAKE_COMMAND} -E touch "${RC_FILENAME}"
                DEPENDS "${COCOTB_EMBED_ZIP}"
                COMMENT "Refreshing ${TARGET_NAME}.rc for the embedded zip"
                VERBATIM
            )
        endif()
        list(APPEND ALL_SOURCES ${RC_FILENAME})
    endif()

    add_library(${TARGET_NAME} SHARED ${ALL_SOURCES})

    if(COCOTB_EMBED_PYTHON)
        add_dependencies(${TARGET_NAME} cocotb_embed_zip)
        target_compile_definitions(${TARGET_NAME} PRIVATE COCOTB_HAVE_EMBEDDED_ZIP=1)
        if(NOT WIN32)
            target_sources(${TARGET_NAME} PRIVATE "${COCOTB_EMBED_ZIP_ASM}")
        endif()
    endif()

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

    if(WIN32)
        # cocotb_tools.config.lib_name_path spells Windows libraries without a
        # lib prefix, so pin it here rather than let toolchains which default
        # to one (MinGW) decide a name the Python side will not look up.
        set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE ${INCLUDE_DIRS})
    # GPI_EXPORTS: gpi.h declarations must be dllexport/default-visible for
    # same-image references, because the GPI functions they declare are
    # defined inside this very library (gpi objects are compiled in with
    # GPI_EXPORTS too). Without it MSVC would emit __imp_gpi_* references
    # that no import library satisfies.
    # PYGPI_EXPORTS and Py_LIMITED_API apply to the pygpi sources: they are
    # compiled against the stable ABI of the oldest supported Python so that
    # only stable ABI symbols can be referenced (see pygpi/pyapi.hpp).
    target_compile_definitions(${TARGET_NAME} PRIVATE
        ${ARG_DEFINES} ${EXTRA_DEFINES}
        GPI_EXPORTS PYGPI_EXPORTS Py_LIMITED_API=0x03090000
    )
    target_compile_options(${TARGET_NAME} PRIVATE ${EXTRA_CXX_FLAGS})

    # dl provides dlopen()/dlsym() for gpi/dynload.cpp and pygpi/pyapi.cpp on
    # Unix; psapi backs utils_lookup_global_sym() on Windows. libpython is
    # deliberately NOT linked: every Python C API symbol is resolved at
    # runtime instead (see pygpi/pyapi.hpp).
    if(ARG_LIBRARIES)
        target_link_libraries(${TARGET_NAME} PRIVATE ${ARG_LIBRARIES})
    endif()
    target_link_libraries(${TARGET_NAME} PRIVATE ${GPI_LINK_LIBS})

    if(MSVC)
        # pyconfig.h nominates the interpreter's import library for the link
        # with `#pragma comment(lib, ...)`, which with Py_LIMITED_API set is
        # python3.lib (python3t.lib in a free-threaded build, the versioned
        # _d.lib name when _DEBUG). Cancel those implicit default libraries:
        # cocotb resolves every Python C API symbol at runtime and never links
        # libpython, so a symbol referenced statically must fail as an
        # unresolved external instead of being satisfied by libpython
        # silently. This is the Windows counterpart of the
        # -Wl,--no-undefined gate the Unix builds use.
        target_link_options(${TARGET_NAME} PRIVATE
            "/NODEFAULTLIB:python3.lib"
            "/NODEFAULTLIB:python3t.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}_d.lib"
            "/NODEFAULTLIB:python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}t_d.lib"
        )
    endif()

    if(WIN32)
        target_compile_definitions(${TARGET_NAME} PRIVATE WIN32)
    endif()

    if(NOT WIN32)
        if(APPLE)
            set(_rpath "@loader_path")
        else()
            set(_rpath "\$ORIGIN")
        endif()
        set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "${_rpath}")

        if(APPLE)
            set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_NAME_DIR "@rpath")
        endif()
    endif()
endfunction()

set(GPI_LINK_LIBS "")
if(UNIX)
    list(APPEND GPI_LINK_LIBS dl)
endif()
if(WIN32)
    # psapi for EnumProcessModules() used by utils_lookup_global_sym()
    list(APPEND GPI_LINK_LIBS psapi)
endif()

# The GPI is built as an object library that every interface library links
# in: a process may only load a single cocotb library per run, so gpi cannot
# be a shared library of its own.
add_library(gpi OBJECT
    ${SHARE_LIB_DIR}/gpi/GpiCbHdl.cpp
    ${SHARE_LIB_DIR}/gpi/GpiCommon.cpp
    ${SHARE_LIB_DIR}/gpi/dynload.cpp
    ${SHARE_LIB_DIR}/gpi/logging.cpp
)
target_include_directories(gpi PRIVATE ${INCLUDE_DIRS})
target_compile_definitions(gpi PRIVATE GPI_EXPORTS ${EXTRA_DEFINES})
target_compile_options(gpi PRIVATE ${EXTRA_CXX_FLAGS})
# Objects are placed into shared libraries, so they must be position
# independent.
set_target_properties(gpi PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(gpi PRIVATE WIN32)
endif()

# Compilation units for each GPI interface family. A simulator family
# library compiles every interface its simulator's flows use into a single
# shared library, so exactly one cocotb library file is loaded per run.
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

# Build the library of one simulator family, named cocotb_<family>, carrying
# every GPI interface (VPI/VHPI/FLI) the simulator's flows can use. Each
# interface contributes its sources, export defines and dispatch-table
# activation define; SIM_DEFINE is the compile-time simulator switch shared
# by all of the family's sources.
function(add_family_library FAMILY SIM_DEFINE)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "INTERFACES")

    set(TARGET_NAME cocotb_${FAMILY})
    set(SOURCES "")
    set(DEFINES ${SIM_DEFINE})
    foreach(IFACE IN LISTS ARG_INTERFACES)
        list(APPEND SOURCES ${COCOTB_${IFACE}_SOURCES})
        list(APPEND DEFINES COCOTB${IFACE}_EXPORTS COCOTB_${IFACE}_DYN)
    endforeach()

    add_cocotb_library(${TARGET_NAME}
        SOURCES ${SOURCES}
        DEFINES ${DEFINES}
    )

    # Fail the link if the library still references a simulator ABI symbol
    # statically instead of resolving it at runtime through the dispatch
    # table, or a Python C API symbol statically instead of through the
    # runtime symbol table of pygpi/pyapi.hpp (macOS and Windows linkers
    # reject undefined symbols by default).
    if(NOT WIN32 AND NOT APPLE)
        target_link_options(${TARGET_NAME} PRIVATE -Wl,--no-undefined)
    endif()
endfunction()

add_family_library(icarus ICARUS INTERFACES VPI)
add_family_library(modelsim MODELSIM INTERFACES VPI VHPI FLI)
add_family_library(ghdl GHDL INTERFACES VPI)

if(UNIX)
    add_family_library(ius IUS INTERFACES VPI VHPI)
    add_family_library(vcs VCS INTERFACES VPI)
    add_family_library(verilator VERILATOR INTERFACES VPI)
    add_family_library(dsim DSim INTERFACES VPI)
endif()

add_family_library(nvc NVC INTERFACES VHPI)
add_family_library(aldec ALDEC INTERFACES VPI VHPI)

if(SKBUILD AND NOT SKBUILD_STATE STREQUAL "editable")
    set(SIM_TARGETS
        cocotb_icarus cocotb_modelsim cocotb_ghdl cocotb_aldec cocotb_nvc)

    if(UNIX)
        list(APPEND SIM_TARGETS
            cocotb_ius cocotb_vcs cocotb_verilator cocotb_dsim)
    endif()

    install(TARGETS ${SIM_TARGETS} DESTINATION cocotb/libs OPTIONAL)
endif()
