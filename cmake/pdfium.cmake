# Fetches the pinned PDFium binary build (D-003) into third_party/pdfium.
set(ABT_PDFIUM_TAG "chromium/8165")
set(ABT_PDFIUM_SHA256 "CA7338AE7779B8B1B82F4580305C4D762D214F8DF1D54B47647FE3CC75989DED")
set(ABT_PDFIUM_URL "https://github.com/bblanchon/pdfium-binaries/releases/download/${ABT_PDFIUM_TAG}/pdfium-win-x64.tgz")
set(ABT_PDFIUM_DIR "${CMAKE_SOURCE_DIR}/third_party/pdfium")

if(NOT EXISTS "${ABT_PDFIUM_DIR}/include/fpdfview.h")
  message(STATUS "Downloading PDFium ${ABT_PDFIUM_TAG}")
  file(DOWNLOAD "${ABT_PDFIUM_URL}" "${ABT_PDFIUM_DIR}/pdfium-win-x64.tgz"
       EXPECTED_HASH SHA256=${ABT_PDFIUM_SHA256} TLS_VERIFY ON SHOW_PROGRESS)
  file(ARCHIVE_EXTRACT INPUT "${ABT_PDFIUM_DIR}/pdfium-win-x64.tgz" DESTINATION "${ABT_PDFIUM_DIR}")
endif()

add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
  IMPORTED_LOCATION "${ABT_PDFIUM_DIR}/bin/pdfium.dll"
  IMPORTED_IMPLIB "${ABT_PDFIUM_DIR}/lib/pdfium.dll.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${ABT_PDFIUM_DIR}/include")
