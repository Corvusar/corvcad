# Measurement harness for the KiCad 10.0.6 schematic and board slices (port gate 2).
#
# Compiles every plausible non-GUI candidate source from a pinned KiCad checkout into one
# object library. Run with `ninja -k 0`: files that fail are recorded instead of stopping
# the build, measure.sh archives what compiled, and the probe links against that archive
# with a linker map. A static archive contributes only members the probe actually needs,
# so the map is the measured translation-unit closure. This is a desktop proof; it does
# not build KiCad's GUI and is not part of the iPad app.
#
#   cmake -S integrations/kicad_core/probe -B build/kicad-probe -G Ninja \
#         -DKICAD_SOURCE=$HOME/kicad-core/kicad
cmake_minimum_required(VERSION 3.25)
# The probes measure size and timing, so an unset build type would skew both (73 MB vs 32.5 MB).
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
project(corvcad_kicad_probe C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(KICAD_SOURCE "" CACHE PATH "KiCad 10.0.6 checkout from integrations/kicad_core/fetch.py")
if(NOT EXISTS "${KICAD_SOURCE}/eeschema/schematic.cpp")
  message(FATAL_ERROR "Set KICAD_SOURCE to a KiCad 10.0.6 checkout")
endif()
set(K "${KICAD_SOURCE}")
set(GEN "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}/drawing_sheet")

find_package(wxWidgets REQUIRED COMPONENTS base core xml net propgrid aui adv html gl richtext stc)
include(${wxWidgets_USE_FILE})
find_package(Boost REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET libzstd freetype2 harfbuzz fontconfig libgit2 protobuf odbc cairo pixman-1)
find_package(CURL REQUIRED)
find_program(PROTOC protoc REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Development)
file(MAKE_DIRECTORY "${GEN}/dialogs" "${GEN}/cpp/api" "${GEN}/specctra_import_export")

# --- Generated sources, produced by KiCad's own CMake scripts -----------------------
foreach(spec
    "common/stroke_params.keywords|stroke_params_lexer.h|stroke_params_keywords.cpp|STROKEPARAMS_T|KICOMMON_API|kicommon.h"
    "common/netlist.keywords|netlist_lexer.h|netlist_keywords.cpp|NL_T|KICOMMON_API|kicommon.h"
    "common/drc_rules.keywords|drc_rules_lexer.h|drc_rules_keywords.cpp|DRCRULE_T|KICOMMON_API|kicommon.h"
    "common/lib_table.keywords|lib_table_lexer.h|lib_table_keywords.cpp|LIB_TABLE_T|KICOMMON_API|kicommon.h"
    "common/template_fieldnames.keywords|template_fieldnames_lexer.h|template_fieldnames_keywords.cpp|TFIELD_T|KICOMMON_API|kicommon.h"
    "common/drawing_sheet/drawing_sheet.keywords|drawing_sheet/drawing_sheet_lexer.h|drawing_sheet/drawing_sheet_keywords.cpp|DRAWINGSHEET_T|KICOMMON_API|kicommon.h"
    "common/embedded_files.keywords|embedded_files_lexer.h|embedded_files_keywords.cpp|EMBEDDED_FILES_T|KICOMMON_API|kicommon.h"
    "common/pcb_plot_params.keywords|pcb_plot_params_lexer.h|pcb_plot_params_keywords.cpp|PCBPLOTPARAMS_T||"
    "eeschema/schematic.keywords|schematic_lexer.h|schematic_keywords.cpp|TSCHEMATIC_T||"
    "eeschema/dialogs/dialog_bom_cfg.keywords|dialogs/dialog_bom_cfg_lexer.h|dialogs/dialog_bom_cfg_keywords.cpp|T_BOMCFG_T||"
    # Board lexers (pcbcommon and pcbnew_kiface_objects); compiled into the board library.
    "PCB|common/pcb.keywords|pcb_lexer.h|pcb_keywords.cpp|PCB_KEYS_T||"
    "PCB|pcbnew/specctra_import_export/specctra.keywords|specctra_import_export/specctra_lexer.h|specctra_import_export/specctra_keywords.cpp|DSN||")
  set(target GENERATED_SOURCES)
  if(spec MATCHES "^PCB\\|")
    string(SUBSTRING "${spec}" 4 -1 spec)
    set(target PCB_GENERATED_SOURCES)
  endif()
  string(REPLACE "|" ";" parts "${spec}")
  list(GET parts 0 input)
  list(GET parts 1 header)
  list(GET parts 2 cpp)
  list(GET parts 3 enum)
  list(GET parts 4 macro)
  list(GET parts 5 macroInclude)
  execute_process(COMMAND ${CMAKE_COMMAND} -Denum=${enum} -DinputFile=${K}/${input}
      -DoutHeaderFile=${GEN}/${header} -DoutCppFile=${GEN}/${cpp}
      -DexportMacro=${macro} -DexportMacroInclude=${macroInclude}
      -P ${K}/cmake/BuildSteps/TokenList2DsnLexer.cmake COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND ${target} ${GEN}/${cpp})
endforeach()

# The release version from KiCadVersion.cmake, as a source tarball build has it: with git,
# `git describe --dirty` would stamp "10.0.6-dirty" (the patch series) into title blocks.
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC_PATH=${K} -DOUTPUT_FILE=${GEN}/kicad_build_version.h
    -DKICAD_CMAKE_MODULE_PATH=${K}/cmake -DKICAD_VERSION_EXTRA= -DGIT_EXECUTABLE=/nonexistent/git
    -P ${K}/cmake/BuildSteps/WriteVersionHeader.cmake
    WORKING_DIRECTORY ${K} COMMAND_ERROR_IS_FATAL ANY)

# Lemon grammars (libeval, libeval_compiler, text_eval), as common/CMakeLists.txt does.
execute_process(COMMAND cc -O1 -w -o ${GEN}/lemon ${K}/thirdparty/lemon/lemon.c COMMAND_ERROR_IS_FATAL ANY)
foreach(grammar libeval/grammar libeval_compiler/grammar text_eval/text_eval)
  get_filename_component(dir ${grammar} DIRECTORY)
  file(MAKE_DIRECTORY ${GEN}/${dir})
  execute_process(COMMAND ${CMAKE_COMMAND} -DLEMON_EXE=${GEN}/lemon
      -DLEMON_TEMPLATE=${K}/thirdparty/lemon/lempar.c -DGRAMMAR_FILE=${K}/common/${grammar}.lemon
      -DGRAMMAR_DIR=${GEN}/${dir} -P ${K}/cmake/BuildSteps/LemonParserGenerator.cmake
      WORKING_DIRECTORY ${GEN}/${dir} COMMAND_ERROR_IS_FATAL ANY)
endforeach()

# KiCad's IPC API messages (api/CMakeLists.txt). kicommon always compiles
# api/serializable.cpp and api/api_utils.cpp, and model items serialise themselves to these
# types, so protobuf is a hard dependency of the KiCad model.
set(KIAPI_PROTOS common/envelope.proto common/types/base_types.proto common/types/enums.proto
    common/types/project_settings.proto common/commands/base_commands.proto
    common/commands/editor_commands.proto common/commands/project_commands.proto board/board.proto
    board/board_commands.proto board/board_types.proto schematic/schematic_types.proto
    schematic/schematic_commands.proto)
execute_process(COMMAND ${PROTOC} --cpp_out=${GEN}/cpp/api --proto_path=${K}/api/proto
    --experimental_allow_proto3_optional ${KIAPI_PROTOS} WORKING_DIRECTORY ${K}/api COMMAND_ERROR_IS_FATAL ANY)
foreach(proto ${KIAPI_PROTOS})
  string(REGEX REPLACE "\\.proto$" ".pb.cc" cc ${proto})
  list(APPEND GENERATED_SOURCES ${GEN}/cpp/api/${cc})
endforeach()

# OpenGL shaders as C++ strings (common/gal/CMakeLists.txt add_shader).
file(GLOB SHADERS ${K}/common/gal/shaders/*.glsl)
file(MAKE_DIRECTORY ${GEN}/gal/shaders)
foreach(shader ${SHADERS})
  get_filename_component(name ${shader} NAME_WE)
  execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_FILE=${shader} -DOUT_CPP_DIR=${GEN}/gal/
      -DOUT_HEADER_DIR=${GEN}/gal/shaders/ -DOUT_CPP_FILENAME=glsl_${name}.cpp
      -DOUT_HEADER_FILENAME=glsl_${name}.h -DOUT_VAR_NAME=glsl_${name}
      -P ${K}/cmake/BuildSteps/CreateShaderCpp.cmake COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND GENERATED_SOURCES ${GEN}/gal/glsl_${name}.cpp)
endforeach()

# Stubs: build_version.cpp includes the OpenCASCADE header only to print its version string;
# OpenCASCADE is not part of the slice.
file(WRITE ${GEN}/Standard_Version.hxx "#pragma once\n#define OCC_VERSION_COMPLETE \"not linked\"\n")
# SPICE (docs/NGSPICE_PROPOSAL.md, S2): NGSPICE_BUILD is an ngspice_ios.sh work directory with its
# mac build. Then KiCad's NGSPICE wrapper (patch 0011) and the session's simulation are compiled
# with CORVCAD_NGSPICE_STATIC, and the desktop programs link build-mac/libngspice.a. Only those
# sources get the define and ngspice's headers, which this header then forwards to; everything
# else (build_version.cpp prints the ngspice version as "unknown") keeps an empty header, so the
# rest of the slice's compile commands stay as they are.
file(WRITE ${GEN}/ngspice/sharedspice.h
    "#pragma once\n#ifdef CORVCAD_NGSPICE_STATIC\n#include_next <ngspice/sharedspice.h>\n#endif\n")
set(NGSPICE_BUILD "" CACHE PATH "ngspice_ios.sh work directory (optional)")
set(NGSPICE_LINK "")
if(NGSPICE_BUILD)
  file(GLOB NGSPICE_INCLUDE "${NGSPICE_BUILD}/ngspice-*/src/include")
  if(NOT EXISTS "${NGSPICE_INCLUDE}/ngspice/sharedspice.h" OR NOT EXISTS "${NGSPICE_BUILD}/build-mac/libngspice.a")
    message(FATAL_ERROR "NGSPICE_BUILD needs ngspice_ios.sh's sources and mac build")
  endif()
  set(NGSPICE_LINK "${NGSPICE_BUILD}/build-mac/libngspice.a;-lc++")
endif()

foreach(have HAVE_STRCASECMP HAVE_STRNCASECMP HAVE_STRTOKR HAVE_MATH_H HAVE_CXX_CMATH HAVE_CMATH_ASINH
             HAVE_CMATH_ACOSH HAVE_CMATH_ATANH HAVE_CMATH_ISINF HAVE_CLOCK_GETTIME
             HAVE_GETTIMEOFDAY_FUNC MALLOC_IN_STDLIB_H HAVE_ISO646_H)
  set(${have} 1)
endforeach()
set(DEFAULT_INSTALL_PATH "/usr/local")
set(KICAD_DATA "${CMAKE_BINARY_DIR}/data")
configure_file(${K}/cmake/config.h.cmake ${GEN}/config.h)

# --- Candidate sources ----------------------------------------------------------------
set(CANDIDATE_GLOBS
    libs/core/*.cpp libs/kimath/src/*.cpp libs/kimath/src/*/*.cpp libs/sexpr/*.cpp
    libs/kiplatform/os/apple/*.mm libs/kiplatform/os/apple/*.cpp libs/kiplatform/port/wxosx/*.cpp
    libs/kiplatform/port/wxosx/*.mm
    libs/kiplatform/os/common/*.cpp
    common/*.cpp common/settings/*.cpp common/project/*.cpp common/font/*.cpp
    common/properties/*.cpp common/drawing_sheet/*.cpp common/libraries/*.cpp
    common/text_eval/*.cpp common/io/*.cpp common/io/kicad/*.cpp common/libeval/*.cpp
    common/libeval_compiler/*.cpp common/plotters/*.cpp common/gal/*.cpp common/jobs/*.cpp
    eeschema/*.cpp eeschema/sch_io/*.cpp eeschema/sch_io/kicad_sexpr/*.cpp
    eeschema/netlist_exporters/*.cpp eeschema/erc/*.cpp eeschema/libraries/*.cpp
    eeschema/sim/*.cpp eeschema/sim/kibis/*.cpp common/api/*.cpp eeschema/api/*.cpp common/git/*.cpp
    eeschema/sch_io/*/*.cpp eeschema/symbol_editor/symbol_editor_settings.cpp common/kicad_curl/*.cpp
    common/database/*.cpp common/http_lib/*.cpp thirdparty/nanodbc/nanodbc/*.cpp
    scripting/python_scripting.cpp common/view/view_item.cpp common/kicad_gl/*.cpp
    common/widgets/ui_common.cpp
    # Desktop-only for the proof: GUI code that model files currently drag in (see closure.py).
    common/dialogs/*.cpp common/widgets/*.cpp common/tool/*.cpp common/tool/ui/*.cpp common/view/*.cpp
    common/gal/opengl/*.cpp common/gal/cairo/*.cpp common/preview_items/*.cpp common/dialog_about/*.cpp
    common/import_gfx/*.cpp common/io/*/*.cpp common/dialogs/git/*.cpp kicad/pcm/*.cpp kicad/pcm/dialogs/*.cpp
    eeschema/tools/*.cpp eeschema/dialogs/*.cpp eeschema/widgets/*.cpp eeschema/symbol_editor/*.cpp
    eeschema/sync_sheet_pin/*.cpp eeschema/printing/*.cpp eeschema/libview/*.cpp eeschema/import_gfx/*.cpp
    thirdparty/glad/src/gl.c thirdparty/nanosvg/nanosvg.cpp thirdparty/libcontext/libcontext.cpp
    thirdparty/clipper2/Clipper2Lib/src/*.cpp thirdparty/fmt/src/format.cc
    thirdparty/tinyspline_lib/*.c thirdparty/delaunator/*.cpp)
foreach(glob ${CANDIDATE_GLOBS})
  file(GLOB found ${K}/${glob})
  list(APPEND CANDIDATES ${found})
endforeach()
# Out of the slice by design: the GUI module entry point, the IPC API server and plugins, 3D
# mouse support, a Windows-only stream shim, and without NGSPICE_BUILD the ngspice wrapper.
list(FILTER CANDIDATES EXCLUDE REGEX
    "(/eeschema/eeschema\\.cpp|api_(handler|plugin|server)[^/]*\\.cpp|spacemouse[^/]*\\.cpp|streamwrapper\\.cpp|design_block_info\\.cpp)$")
if(NOT NGSPICE_BUILD)
  list(FILTER CANDIDATES EXCLUDE REGEX "/sim/ngspice\\.cpp$")
endif()

# Include roots: KiCad's own plus every bundled third-party include dir.
file(GLOB THIRDPARTY LIST_DIRECTORIES true ${K}/thirdparty/*)
# eeschema's own include roots (eeschema/CMakeLists.txt) plus the folders its headers
# reach with angle brackets.
set(INCLUDES ${GEN} ${K} ${K}/include ${K}/common ${K}/common/dialogs ${K}/eeschema
    ${K}/eeschema/symbol_editor ${K}/eeschema/dialogs ${K}/eeschema/tools ${K}/eeschema/widgets
    ${K}/eeschema/libview ${K}/eeschema/netlist_exporters ${K}/eeschema/sim ${K}/pcbnew ${K}/kicad
    ${K}/resources/bitmaps_png ${K}/thirdparty/nanodbc
    ${K}/thirdparty/magic_enum/magic_enum ${K}/scripting ${K}/api/include ${GEN}/cpp ${GEN}/cpp/api ${Python3_INCLUDE_DIRS}
    ${K}/libs/core/include ${K}/libs/kimath/include ${K}/libs/sexpr/include
    ${K}/libs/kiplatform/include ${K}/thirdparty/clipper2/Clipper2Lib/include
    ${K}/thirdparty/fmt/include ${K}/thirdparty/nlohmann_json ${K}/thirdparty/pegtl/include)
foreach(dir ${THIRDPARTY})
  if(IS_DIRECTORY ${dir})
    list(APPEND INCLUDES ${dir})
    if(IS_DIRECTORY ${dir}/include)
      list(APPEND INCLUDES ${dir}/include)
    endif()
  endif()
endforeach()

add_library(kicad_candidates OBJECT ${CANDIDATES} ${GENERATED_SOURCES})
target_include_directories(kicad_candidates PRIVATE ${INCLUDES} ${Boost_INCLUDE_DIRS})
set(KICAD_DEFINES GLM_FORCE_CTOR_INIT KICAD_CONFIG_DIR=kicad USINGZ
    BS_THREAD_POOL_NATIVE_EXTENSIONS=1)
# SCRIPTING is constructed by InitPgm; the bundle path only matters inside KiCad.app.
target_compile_definitions(kicad_candidates PRIVATE ${KICAD_DEFINES} KICOMMON_DLL=1 KIAPI_IMPORTEXPORT=
    OSX_BUNDLE_PYTHON_SITE_PACKAGES_DIR="/site-packages")
target_link_libraries(kicad_candidates PRIVATE PkgConfig::DEPS)
target_compile_options(kicad_candidates PRIVATE -w)
if(NGSPICE_BUILD)
  set_source_files_properties(${K}/eeschema/sim/ngspice.cpp ${K}/eeschema/sim/spice_simulator.cpp
      PROPERTIES COMPILE_DEFINITIONS CORVCAD_NGSPICE_STATIC INCLUDE_DIRECTORIES "${NGSPICE_INCLUDE}")
endif()

# Probes compiled here; measure.sh links them against archives of what compiled.
add_library(probe_main OBJECT probe.cpp)
target_include_directories(probe_main PRIVATE ${INCLUDES} ${Boost_INCLUDE_DIRS})
target_compile_definitions(probe_main PRIVATE ${KICAD_DEFINES} KIAPI_IMPORTEXPORT=)
target_link_libraries(probe_main PRIVATE PkgConfig::DEPS)

# --- Board slice ----------------------------------------------------------------------
# pcbnew sources build with pcbnew's own include roots (pcbnew/CMakeLists.txt) and the
# PCBNEW define, as pcbcommon does, so they get their own object library and archive.
set(PCB_GLOBS pcbnew/*.cpp pcbnew/*/*.cpp pcbnew/*/*/*.cpp pcbnew/*/*/*/*.cpp
    thirdparty/zint/backend/*.c common/transline_calculations/*.cpp thirdparty/markdown2html/*.cpp
    thirdparty/markdown2html/md_parser/*.c thirdparty/markdown2html/html_formatter/*.c
    3d-viewer/3d_viewer/eda_3d_viewer_settings.cpp)
foreach(glob ${PCB_GLOBS})
  file(GLOB found ${K}/${glob})
  list(APPEND PCB_CANDIDATES ${found})
endforeach()
# The editor module entry point, SWIG output, 3D mouse, IPC API server, zint's tests, the
# OpenCASCADE exporters (STEP, U3D, Python 3D utils), a stale file and a test program.
list(FILTER PCB_CANDIDATES EXCLUDE REGEX
    "(/pcbnew/pcbnew\\.cpp|/pcbnew/python/swig/.*|/navlib/.*|api_handler[^/]*\\.cpp|/zint/backend/tests/.*|/exporters/(step|u3d)/.*|pcbnew_utils_3d\\.cpp|zones_functions_for_undo_redo\\.cpp|specctra_test\\.cpp)$")
# pcb_io importers include their own headers with angle brackets (pcbnew/pcb_io/*/CMakeLists.txt).
file(GLOB PCB_IO_DIRS LIST_DIRECTORIES true ${K}/pcbnew/pcb_io/*)
list(FILTER PCB_IO_DIRS INCLUDE REGEX ".*/[a-z_0-9]+$")
set(PCB_INCLUDES ${GEN} ${GEN}/specctra_import_export ${K} ${K}/include ${K}/common ${K}/common/dialogs
    ${K}/pcbnew ${K}/pcbnew/dialogs ${K}/pcbnew/autorouter ${K}/pcbnew/generators ${K}/pcbnew/exporters
    ${K}/pcbnew/importers ${K}/pcbnew/specctra_import_export ${K}/pcbnew/python/scripting
    ${K}/pcbnew/pcb_io ${PCB_IO_DIRS} ${K}/thirdparty/markdown2html ${K}/common/transline_calculations
    ${K}/3d-viewer ${K}/3d-viewer/dialogs ${K}/utils/idftools ${K}/kicad ${K}/resources/bitmaps_png
    ${K}/thirdparty/magic_enum/magic_enum ${K}/scripting ${K}/api/include ${GEN}/cpp ${GEN}/cpp/api
    ${Python3_INCLUDE_DIRS} ${K}/libs/core/include ${K}/libs/kimath/include ${K}/libs/sexpr/include
    ${K}/libs/kiplatform/include ${K}/thirdparty/clipper2/Clipper2Lib/include ${K}/thirdparty/fmt/include
    ${K}/thirdparty/nlohmann_json ${K}/thirdparty/pegtl/include)
foreach(dir ${THIRDPARTY})
  if(IS_DIRECTORY ${dir})
    list(APPEND PCB_INCLUDES ${dir})
    if(IS_DIRECTORY ${dir}/include)
      list(APPEND PCB_INCLUDES ${dir}/include)
    endif()
  endif()
endforeach()

add_library(kicad_pcb_candidates OBJECT ${PCB_CANDIDATES} ${PCB_GENERATED_SOURCES})
# Per-file include roots, so adding them does not rebuild every other object.
file(GLOB MARKDOWN_SOURCES ${K}/thirdparty/markdown2html/*.cpp ${K}/thirdparty/markdown2html/*/*.c)
set_source_files_properties(${MARKDOWN_SOURCES} PROPERTIES INCLUDE_DIRECTORIES
    "${K}/thirdparty/markdown2html/md_parser;${K}/thirdparty/markdown2html/html_formatter")
set_source_files_properties(${K}/common/gal/opengl/opengl_gal.cpp ${K}/common/gal/opengl/antialiasing.cpp
    ${K}/common/gal/opengl/opengl_compositor.cpp TARGET_DIRECTORY kicad_candidates
    PROPERTIES INCLUDE_DIRECTORIES ${GEN}/gal/shaders)
target_include_directories(kicad_pcb_candidates PRIVATE ${PCB_INCLUDES} ${Boost_INCLUDE_DIRS})
target_compile_definitions(kicad_pcb_candidates PRIVATE ${KICAD_DEFINES} PCBNEW KIAPI_IMPORTEXPORT=)
target_link_libraries(kicad_pcb_candidates PRIVATE PkgConfig::DEPS)
target_compile_options(kicad_pcb_candidates PRIVATE -w)

add_library(pcb_probe_main OBJECT pcb_probe.cpp)
target_include_directories(pcb_probe_main PRIVATE ${PCB_INCLUDES} ${Boost_INCLUDE_DIRS})
target_compile_definitions(pcb_probe_main PRIVATE ${KICAD_DEFINES} PCBNEW KIAPI_IMPORTEXPORT=)
target_link_libraries(pcb_probe_main PRIVATE PkgConfig::DEPS)

# --- Headless replacements (integrations/kicad_core/headless) --------------------------
# CorvCAD files that stand in for editor-bound KiCad translation units. measure.sh links
# them ahead of the slice for corvcad_kicad_probe_headless, whose closure must hold no GUI.
file(GLOB HEADLESS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../headless/*.cpp)
if(NGSPICE_BUILD)
  # KiCad's own spice_simulator.cpp then creates the NGSPICE instance.
  list(FILTER HEADLESS_SOURCES EXCLUDE REGEX "/simulator_stub\\.cpp$")
endif()
add_library(kicad_headless OBJECT ${HEADLESS_SOURCES})
target_include_directories(kicad_headless PRIVATE ${INCLUDES} ${Boost_INCLUDE_DIRS})
target_compile_definitions(kicad_headless PRIVATE ${KICAD_DEFINES} KICOMMON_DLL=1 KIAPI_IMPORTEXPORT=)
target_link_libraries(kicad_headless PRIVATE PkgConfig::DEPS)

# Board replacements (headless/pcbnew) build like pcbnew sources. measure.sh links them
# first for corvcad_kicad_pcb_probe_headless.
file(GLOB PCB_HEADLESS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../headless/pcbnew/*.cpp)
add_library(kicad_pcb_headless OBJECT ${PCB_HEADLESS_SOURCES})
target_include_directories(kicad_pcb_headless PRIVATE ${PCB_INCLUDES} ${Boost_INCLUDE_DIRS})
target_compile_definitions(kicad_pcb_headless PRIVATE ${KICAD_DEFINES} PCBNEW KIAPI_IMPORTEXPORT=)
target_link_libraries(kicad_pcb_headless PRIVATE PkgConfig::DEPS)

# --- Native schematic session (port gate 3)-------------------------------------------
# The engine's cc_sch_session C ABI over KiCad's SCHEMATIC, with the backend-neutral test.
# The engine's route planner is shared with the compatible backend.
add_library(session_main OBJECT ../session/kicad_session.cpp ../session/abi_test.c
    ../../../engine/src/schematic_route.cpp)
target_include_directories(session_main PRIVATE ${INCLUDES} ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/include)
target_compile_definitions(session_main PRIVATE ${KICAD_DEFINES} KIAPI_IMPORTEXPORT=)
target_link_libraries(session_main PRIVATE PkgConfig::DEPS)
if(NGSPICE_BUILD)
  target_compile_definitions(session_main PRIVATE CORVCAD_NGSPICE_STATIC)
  target_include_directories(session_main PRIVATE ${NGSPICE_INCLUDE})
endif()

# --- Native board checks (port gate 5) ------------------------------------------------
# cc_board_native_* over KiCad's BOARD, built like pcbnew sources, with its C test. It links
# with the session (program start, Kiface) and both closures, as the NATIVE_KICAD app does.
add_library(board_main OBJECT ../session/kicad_board.cpp ../session/board_test.c)
target_include_directories(board_main PRIVATE ${PCB_INCLUDES} ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/include)
target_compile_definitions(board_main PRIVATE ${KICAD_DEFINES} PCBNEW KIAPI_IMPORTEXPORT=)
target_link_libraries(board_main PRIVATE PkgConfig::DEPS)

file(WRITE ${CMAKE_BINARY_DIR}/link_flags.txt
    "${wxWidgets_LIBRARIES};${ZLIB_LIBRARIES};${DEPS_LDFLAGS};${Python3_LIBRARIES};${CURL_LIBRARIES};${NGSPICE_LINK}")
