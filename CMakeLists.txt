cmake_minimum_required(VERSION 3.28)
project(abthumb VERSION 0.1.4 LANGUAGES C CXX)

option(ABT_FUZZ "Build libFuzzer + ASan targets" OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: nothing to redistribute, safe to load into any process (D-001).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C
  _CRT_SECURE_NO_WARNINGS
  ABT_VERSION_STR="${PROJECT_VERSION}"
  ABT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  ABT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  ABT_VERSION_PATCH=${PROJECT_VERSION_PATCH})

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# No exceptions, no RTTI (D-001): strip CMake's default /EHsc and /GR.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "/O2 /Ob3 /DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "/O2 /Ob3 /DNDEBUG")

add_compile_options(
  /W4 /utf-8 /Zi /Gy /Gw /guard:cf /diagnostics:caret
  /wd4200   # zero-sized arrays in structs
  /wd4201   # nameless struct/union
  "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus;/GR-;/EHs-c-;/D_HAS_EXCEPTIONS=0>")

# Code-quality gates (D-035). ABT_WERROR: any /W4 warning fails the build. ABT_ANALYZE: MSVC static
# analysis (/analyze) on our code only; SDK and third-party headers are external and silenced.
option(ABT_WERROR "Treat compiler warnings as errors" OFF)
option(ABT_ANALYZE "Run MSVC /analyze on our targets" OFF)
if(ABT_WERROR)
  add_compile_options(/WX)
endif()
if(ABT_ANALYZE)
  add_compile_options(/analyze /analyze:external- /external:anglebrackets /external:W0
    /external:env:INCLUDE /external:env:EXTERNAL_INCLUDE
    # Noise for this code base: ignored lstrcpyn/logging return values, SAL annotation of exported entry points,
    # GetTickCount for short relative timeouts, 25 KB stack buffers (render_shape).
    /wd6031 /wd28251 /wd28159 /wd6262)
endif()
add_link_options(/DEBUG:FULL /guard:cf "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")

if(ABT_FUZZ)
  add_compile_options(/fsanitize=address)
  add_compile_definitions(ABT_FUZZING=1)
endif()

# ---------------------------------------------------------------- core (C11, portable)
add_library(abt_core STATIC
  src/core/abt_core.c
  src/core/abt_image.c
  src/core/abt_probe.c
  src/core/abt_eps.c
  src/core/abt_psd.c
  src/core/abt_psd_layers.c
  src/core/abt_pixbuf.c
  third_party/miniz/miniz.c
)
target_compile_definitions(abt_core PRIVATE MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME)
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "/W0;/analyze-")
target_include_directories(abt_core PUBLIC src/core PRIVATE third_party/miniz)

if(ABT_FUZZ)
  # Coverage feedback for the code under test: /fsanitize=fuzzer on the targets alone only
  # instruments the harness, which leaves libFuzzer mutating blind.
  target_compile_options(abt_core PRIVATE
    /fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
    /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
  # libFuzzer targets (tests/fuzz): only abt_core + fuzzers are built.
  foreach(t probe psd qoi)
    add_executable(fuzz_${t} tests/fuzz/fuzz_${t}.c)
    target_compile_options(fuzz_${t} PRIVATE /fsanitize=fuzzer)
    target_link_libraries(fuzz_${t} PRIVATE abt_core)
  endforeach()
else()

# ---------------------------------------------------------------- Windows helpers (C++)
add_library(abt_win STATIC
  src/win/abt_log.cpp
  src/win/abt_paths.cpp
  src/win/abt_registry.cpp
  src/win/abt_wic.cpp
  src/win/abt_proto.cpp
  src/win/abt_decode.cpp
)
target_include_directories(abt_win PUBLIC src/win)
target_link_libraries(abt_win PUBLIC abt_core windowscodecs shlwapi ole32 oleaut32 uuid advapi32 shell32 propsys mscms gdi32 user32)

include(cmake/pdfium.cmake)

# ---------------------------------------------------------------- render pipeline (worker + CLI)
add_library(abt_render STATIC
  src/render/render.cpp
  src/render/render_pdf.cpp
  src/render/render_gs.cpp
)
target_include_directories(abt_render PUBLIC src/render)
target_link_libraries(abt_render PUBLIC abt_win pdfium)

# ---------------------------------------------------------------- render worker
add_executable(abthumb_render
  src/worker/worker_main.cpp
)
set_target_properties(abthumb_render PROPERTIES OUTPUT_NAME abthumb-render)
target_link_libraries(abthumb_render PRIVATE abt_render)
add_custom_command(TARGET abthumb_render POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ABT_PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:abthumb_render>")

# ---------------------------------------------------------------- shell extension DLL
add_library(abthumb SHARED
  src/shellext/dllmain.cpp
  src/shellext/thumbnail.cpp
  src/shellext/client.cpp
  src/shellext/preview.cpp
  src/shellext/abthumb.def
  src/shellext/abthumb.rc
)
target_link_libraries(abthumb PRIVATE abt_win gdi32 user32 msimg32)

# ---------------------------------------------------------------- CLI
add_executable(abthumb_cli
  src/cli/main.cpp
)
set_target_properties(abthumb_cli PROPERTIES OUTPUT_NAME abthumb PDB_NAME abthumb_cli)
target_link_libraries(abthumb_cli PRIVATE abt_render delayimp)
target_link_options(abthumb_cli PRIVATE /DELAYLOAD:pdfium.dll)

# ---------------------------------------------------------------- per-user preview host (COM local server)
add_executable(abthumb_preview WIN32
  src/cli/preview_host.cpp
)
set_target_properties(abthumb_preview PROPERTIES OUTPUT_NAME abthumb-preview)
target_link_libraries(abthumb_preview PRIVATE abt_win user32)
add_dependencies(abthumb_preview abthumb)

# ---------------------------------------------------------------- unit tests
add_executable(abt_tests
  tests/unit/test_main.c
  tests/unit/test_core.c
  tests/unit/test_eps.c
  tests/unit/test_psd.c
)
target_include_directories(abt_tests PRIVATE tests/unit)
target_compile_definitions(abt_tests PRIVATE ABT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(abt_tests PRIVATE abt_core)
target_compile_options(abt_tests PRIVATE /analyze-)  # test code: unchecked mallocs are fine, a NULL crash is a failure anyway

# ---------------------------------------------------------------- agent
add_executable(abthumb_agent WIN32
  src/agent/agent_main.cpp
  src/agent/cache.cpp
  src/agent/filekey.cpp
  src/agent/pool.cpp
  src/agent/prewarm.cpp
  src/agent/server.cpp
)
set_target_properties(abthumb_agent PROPERTIES OUTPUT_NAME abthumb-agent)
target_link_libraries(abthumb_agent PRIVATE abt_win)
add_dependencies(abthumb_agent abthumb_render abthumb)

endif() # NOT ABT_FUZZ
