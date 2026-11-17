cmake_minimum_required(VERSION 3.28)
project(abthumb VERSION 0.1.0 LANGUAGES C CXX)

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
add_link_options(/DEBUG:FULL /guard:cf "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")

if(ABT_FUZZ)
  add_compile_options(/fsanitize=address)
  add_compile_definitions(ABT_FUZZING=1)
endif()

# ---------------------------------------------------------------- core (C11, portable)
add_library(abt_core STATIC
  src/core/abt_core.c
)
target_include_directories(abt_core PUBLIC src/core)

# ---------------------------------------------------------------- Windows helpers (C++)
add_library(abt_win STATIC
  src/win/abt_log.cpp
  src/win/abt_paths.cpp
  src/win/abt_registry.cpp
  src/win/abt_wic.cpp
)
target_include_directories(abt_win PUBLIC src/win)
target_link_libraries(abt_win PUBLIC abt_core windowscodecs shlwapi ole32 oleaut32 uuid advapi32 shell32 propsys)

# ---------------------------------------------------------------- shell extension DLL
add_library(abthumb SHARED
  src/shellext/dllmain.cpp
  src/shellext/thumbnail.cpp
  src/shellext/preview.cpp
  src/shellext/abthumb.def
  src/shellext/abthumb.rc
)
target_link_libraries(abthumb PRIVATE abt_win gdi32 user32)

# ---------------------------------------------------------------- CLI
add_executable(abthumb_cli
  src/cli/main.cpp
)
set_target_properties(abthumb_cli PROPERTIES OUTPUT_NAME abthumb PDB_NAME abthumb_cli)
target_link_libraries(abthumb_cli PRIVATE abt_win gdi32 user32)

# ---------------------------------------------------------------- per-user preview host (COM local server)
add_executable(abthumb_preview WIN32
  src/cli/preview_host.cpp
)
set_target_properties(abthumb_preview PROPERTIES OUTPUT_NAME abthumb-preview)
target_link_libraries(abthumb_preview PRIVATE abt_win user32)
add_dependencies(abthumb_preview abthumb)
