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
)

set(SHARE_LIB_DIR ${CMAKE_SOURCE_DIR}/src/cocotb/share/lib)
set(COCOTB_DEF_DIR ${CMAKE_SOURCE_DIR}/src/cocotb/share/def)

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

    set(ALL_SOURCES ${ARG_SOURCES})
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
        list(APPEND ALL_SOURCES ${RC_FILENAME})
    endif()

    add_library(${TARGET_NAME} SHARED ${ALL_SOURCES})

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

add_cocotb_library(gpi
    SOURCES
        ${SHARE_LIB_DIR}/gpi/GpiCbHdl.cpp
        ${SHARE_LIB_DIR}/gpi/GpiCommon.cpp
        ${SHARE_LIB_DIR}/gpi/dynload.cpp
        ${SHARE_LIB_DIR}/gpi/logging.cpp
    DEFINES GPI_EXPORTS
    LIBRARIES ${GPI_LINK_LIBS}
    RC_LIBRARIES ""
)

# Out-of-process IPC server: spawned as `python -m cocotb._ipc` by GPI and
# drives the cocotb Python testbench over a local socket. Deliberately does
# not link Python: the testbench runs in a separate interpreter.
#
# When COCOTB_IPC_EMBED_ZIP is enabled (recommended for standalone
# shared-library distributions, off for wheels), the first-party Python
# packages are packed into a zip and embedded into the library so the
# spawned child can import cocotb even without it being installed
# (see tools/make_ipc_zip.py and the COCOTB_IPC_EMBED env var).
option(COCOTB_IPC_EMBED_ZIP "Embed the cocotb Python package zip into libcocotbipc" OFF)

set(COCOTB_IPC_ZIP "")
if(COCOTB_IPC_EMBED_ZIP)
    # scikit-build-core passes the build interpreter as Python_EXECUTABLE;
    # a standalone CMake build falls back to python3 found on PATH.
    if(NOT Python3_EXECUTABLE AND Python_EXECUTABLE)
        set(Python3_EXECUTABLE "${Python_EXECUTABLE}")
    endif()
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

    set(COCOTB_IPC_ZIP "${CMAKE_CURRENT_BINARY_DIR}/cocotb-ipc.zip")
    file(GLOB_RECURSE COCOTB_IPC_ZIP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.pyi"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*/py.typed"
    )
    list(FILTER COCOTB_IPC_ZIP_SOURCES EXCLUDE REGEX "/__pycache__/")
    # scikit-build-core exposes the project version as an environment
    # variable; plain CMake builds leave it unset and the zip then
    # carries no dist-info (cocotb._version degrades gracefully).
    set(COCOTB_IPC_ZIP_VERSION_ARGS "")
    if(SKBUILD_PROJECT_VERSION)
        set(COCOTB_IPC_ZIP_VERSION_ARGS --version "${SKBUILD_PROJECT_VERSION}")
    elseif(NOT "$ENV{SKBUILD_PROJECT_VERSION}" STREQUAL "")
        set(COCOTB_IPC_ZIP_VERSION_ARGS --version "$ENV{SKBUILD_PROJECT_VERSION}")
    endif()
    add_custom_command(
        OUTPUT "${COCOTB_IPC_ZIP}"
        COMMAND Python3::Interpreter
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_ipc_zip.py"
                --src-dir "${CMAKE_CURRENT_SOURCE_DIR}/src"
                --output "${COCOTB_IPC_ZIP}"
                ${COCOTB_IPC_ZIP_VERSION_ARGS}
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_ipc_zip.py"
            ${COCOTB_IPC_ZIP_SOURCES}
        COMMENT "Generating cocotb-ipc.zip"
        VERBATIM
    )
    if(NOT WIN32)
        enable_language(ASM)
    endif()
endif()

add_cocotb_library(cocotbipc
    SOURCES
        ${SHARE_LIB_DIR}/ipc/codec_binary.cpp
        ${SHARE_LIB_DIR}/ipc/codec_json.cpp
        ${SHARE_LIB_DIR}/ipc/dispatcher.cpp
        ${SHARE_LIB_DIR}/ipc/embed.cpp
        ${SHARE_LIB_DIR}/ipc/ipc_protocol.cpp
        ${SHARE_LIB_DIR}/ipc/ipc_tcp.cpp
        ${SHARE_LIB_DIR}/ipc/ipc_zip.cpp
        ${SHARE_LIB_DIR}/ipc/logging.cpp
    DEFINES IPC_EXPORTS
    LIBRARIES gpi
    RC_LIBRARIES ""
)

if(WIN32)
    target_link_libraries(cocotbipc PRIVATE ws2_32)
endif()

if(COCOTB_IPC_EMBED_ZIP)
    # Build the zip before compiling anything that embeds it.
    add_custom_target(cocotb_ipc_zip DEPENDS "${COCOTB_IPC_ZIP}")
    add_dependencies(cocotbipc cocotb_ipc_zip)
    target_compile_definitions(cocotbipc PRIVATE COCOTB_HAVE_EMBEDDED_ZIP=1)

    if(WIN32)
        # Ship the zip as an RCDATA resource in the same generated .rc
        # that already carries the manifest (forward slashes from the
        # CMake path are valid for both rc.exe and windres). The rc is
        # written at configure time; this rule only refreshes its
        # timestamp when the zip changes so the resource recompiles.
        set(_cocotb_zip_rc "${CMAKE_CURRENT_BINARY_DIR}/cocotbipc.rc")
        file(APPEND "${_cocotb_zip_rc}"
            "\n// Embedded cocotb package zip (tools/make_ipc_zip.py)\n"
            "IDR_COCOTBIPCZIP RCDATA \"${COCOTB_IPC_ZIP}\"\n"
        )
        add_custom_command(
            OUTPUT "${_cocotb_zip_rc}"
            COMMAND ${CMAKE_COMMAND} -E touch "${_cocotb_zip_rc}"
            DEPENDS "${COCOTB_IPC_ZIP}"
            COMMENT "Refreshing cocotbipc.rc for the embedded zip"
            VERBATIM
        )
    else()
        # Ship the zip as raw bytes in a read-only section, addressed by
        # the cocotb_zip_start/cocotb_zip_end labels. C symbols carry a
        # leading underscore on Mach-O (Darwin's ABI).
        if(APPLE)
            set(_cocotb_zip_sym "_")
            set(_cocotb_zip_section ".section __TEXT,__const")
        else()
            set(_cocotb_zip_sym "")
            set(_cocotb_zip_section ".section .rodata")
        endif()
        set(COCOTB_IPC_ZIP_ASM "${CMAKE_CURRENT_BINARY_DIR}/cocotb_ipc_zip.s")
        file(WRITE "${COCOTB_IPC_ZIP_ASM}"
            "/* Generated by CMake: embedded cocotb package zip. */\n"
            "${_cocotb_zip_section}\n"
            ".globl ${_cocotb_zip_sym}cocotb_zip_start\n"
            ".globl ${_cocotb_zip_sym}cocotb_zip_end\n"
            "${_cocotb_zip_sym}cocotb_zip_start:\n"
            ".incbin \"${COCOTB_IPC_ZIP}\"\n"
            "${_cocotb_zip_sym}cocotb_zip_end:\n"
        )
        if(NOT APPLE)
            file(APPEND "${COCOTB_IPC_ZIP_ASM}"
                ".section .note.GNU-stack,\"\",@progbits\n")
        endif()
        # The .s content only names the zip, so refresh its timestamp
        # when the zip changes to force a re-assemble.
        add_custom_command(
            OUTPUT "${COCOTB_IPC_ZIP_ASM}"
            COMMAND ${CMAKE_COMMAND} -E touch "${COCOTB_IPC_ZIP_ASM}"
            DEPENDS "${COCOTB_IPC_ZIP}"
            COMMENT "Refreshing cocotb_ipc_zip.s for the embedded zip"
            VERBATIM
        )
        target_sources(cocotbipc PRIVATE "${COCOTB_IPC_ZIP_ASM}")
    endif()
endif()

function(add_vpi_library SIM_NAME SIM_DEFINE)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs "EXTRA_LIBS" "EXTRA_LIB_DIRS")
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    string(TOLOWER ${SIM_NAME} SIM_LOWER)
    set(TARGET_NAME cocotbvpi_${SIM_LOWER})

    add_cocotb_library(${TARGET_NAME}
        SOURCES
            ${SHARE_LIB_DIR}/gpi/vpi/VpiImpl.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiCbHdl.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiObj.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiIterator.cpp
            ${SHARE_LIB_DIR}/gpi/vpi/VpiSignal.cpp
        DEFINES COCOTBVPI_EXPORTS ${SIM_DEFINE}
        LIBRARIES gpi ${ARG_EXTRA_LIBS}
        RC_LIBRARIES "gpi"
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
    set(TARGET_NAME cocotbvhpi_${SIM_LOWER})

    add_cocotb_library(${TARGET_NAME}
        SOURCES
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiImpl.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiCbHdl.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiObj.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiIterator.cpp
            ${SHARE_LIB_DIR}/gpi/vhpi/VhpiSignal.cpp
        DEFINES COCOTBVHPI_EXPORTS ${SIM_DEFINE}
        LIBRARIES gpi ${ARG_EXTRA_LIBS}
        RC_LIBRARIES "gpi"
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

add_cocotb_library(cocotbfli_modelsim
    SOURCES
        ${SHARE_LIB_DIR}/gpi/fli/FliImpl.cpp
        ${SHARE_LIB_DIR}/gpi/fli/FliCbHdl.cpp
        ${SHARE_LIB_DIR}/gpi/fli/FliObjHdl.cpp
    DEFINES COCOTBFLI_EXPORTS
    LIBRARIES gpi ${MODELSIM_EXTRA_LIBS}
    RC_LIBRARIES "gpi"
)

if(WIN32)
    target_link_directories(cocotbfli_modelsim PRIVATE ${COCOTB_DEF_DIR})
endif()

if(NOT WIN32)
    if(APPLE)
        target_link_options(cocotbfli_modelsim PRIVATE -undefined dynamic_lookup)
    else()
        target_link_options(cocotbfli_modelsim PRIVATE -Wl,--allow-shlib-undefined)
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
    add_dependencies(cocotbvpi_icarus icarus_import_lib)
    add_dependencies(cocotbvpi_modelsim modelsim_import_lib)
    add_dependencies(cocotbvhpi_modelsim modelsim_import_lib)
    add_dependencies(cocotbfli_modelsim modelsim_import_lib)
    add_dependencies(cocotbvpi_ghdl ghdl_import_lib)
    add_dependencies(cocotbvhpi_nvc nvcvhpi_import_lib)
    add_dependencies(cocotbvpi_aldec aldec_import_lib)
    add_dependencies(cocotbvhpi_aldec aldec_import_lib)
endif()

if(SKBUILD AND NOT SKBUILD_STATE STREQUAL "editable")
    install(TARGETS gpi cocotbipc DESTINATION cocotb/libs)

    set(SIM_TARGETS
        cocotbvpi_icarus cocotbvpi_modelsim cocotbvhpi_modelsim
        cocotbfli_modelsim cocotbvpi_ghdl cocotbvpi_aldec cocotbvhpi_aldec
        cocotbvhpi_nvc)

    if(UNIX)
        list(APPEND SIM_TARGETS
            cocotbvpi_ius cocotbvhpi_ius cocotbvpi_vcs
            cocotbvpi_verilator cocotbvpi_dsim)
    endif()

    install(TARGETS ${SIM_TARGETS} DESTINATION cocotb/libs OPTIONAL)
endif()
