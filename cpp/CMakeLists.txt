cmake_minimum_required(VERSION 3.18.1)
project(hardcopy_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(HARDCOPY_OPTIMIZE_ALL_CONFIGS
       "Compile the core and bundled deps at -O2 even in Debug configurations" ON)
if(HARDCOPY_OPTIMIZE_ALL_CONFIGS AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (hardcopy default)" FORCE)
endif()

option(HARDCOPY_TEXT_MODE        "Enable text conversion pipeline"        ON)
option(HARDCOPY_VERTICAL_LAYOUT "Enable vertical (CJK) layout"           ON)
option(HARDCOPY_EPUB            "Enable EPUB input parser"               ON)
option(HARDCOPY_DOCX            "Enable DOCX input parser"               OFF)
option(HARDCOPY_PDFIN           "Enable PDF input parser (uses MuPDF)"   OFF)
option(HARDCOPY_PARALLEL        "Enable parallel page rendering"         ON)
option(HARDCOPY_COMIC           "Enable comic/manga input (CBZ/MOBI)"   ON)
option(HARDCOPY_DITHER          "Enable e-ink dithering (Bayer/FS)"      ON)
option(HARDCOPY_PRINT           "Enable note print layout (N-up PDF)"    ON)
option(HARDCOPY_BUILD_CLI       "Build the desktop CLI (cli/main.cpp)"   OFF)
option(HARDCOPY_BUILD_GUI       "Build the desktop GUI (gui/main.cpp)"   OFF)

set(MUPDF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf" CACHE STRING
    "Path to prebuilt MuPDF tree (include/ + lib/<abi>/)")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann-json"
)
FetchContent_MakeAvailable(nlohmann_json)

if(HARDCOPY_PDFIN)
    if(MUPDF_ROOT)
        set(_mupdf_candidates "")
        if(CMAKE_ANDROID_ARCH_ABI)
            list(APPEND _mupdf_candidates "${MUPDF_ROOT}/lib/${CMAKE_ANDROID_ARCH_ABI}")
        endif()
        if(ANDROID_ABI)
            list(APPEND _mupdf_candidates "${MUPDF_ROOT}/lib/${ANDROID_ABI}")
        endif()
        if(NOT ANDROID AND NOT CMAKE_ANDROID_ARCH_ABI)
            list(APPEND _mupdf_candidates "${MUPDF_ROOT}/lib/x86_64")
        endif()

        set(_mupdf_libdir "")
        foreach(_d ${_mupdf_candidates})
            if(EXISTS "${_d}/libmupdf.a")
                set(_mupdf_libdir "${_d}")
                break()
            endif()
        endforeach()
        if(NOT _mupdf_libdir)
            file(GLOB _mupdf_libdirs "${MUPDF_ROOT}/lib/*")
            foreach(_d ${_mupdf_libdirs})
                if(EXISTS "${_d}/libmupdf.a")
                    set(_mupdf_libdir "${_d}")
                    break()
                endif()
            endforeach()
        endif()
        if(NOT _mupdf_libdir)
            set(_mupdf_libdir "${MUPDF_ROOT}/lib")
        endif()
        if(EXISTS "${_mupdf_libdir}/libmupdf.a"
           AND EXISTS "${MUPDF_ROOT}/include/mupdf/fitz.h")
            set(MUPDF_LIB     "${_mupdf_libdir}/libmupdf.a"        CACHE FILEPATH "" FORCE)
            set(MUPDF_INCLUDE "${MUPDF_ROOT}/include"              CACHE PATH     "" FORCE)
            if(EXISTS "${_mupdf_libdir}/libmupdf-third.a")
                set(MUPDF_THIRD_LIB "${_mupdf_libdir}/libmupdf-third.a" CACHE FILEPATH "" FORCE)
            endif()
        else()
            set(MUPDF_LIB     "MUPDF_LIB-NOTFOUND")
            set(MUPDF_INCLUDE "MUPDF_INCLUDE-NOTFOUND")
        endif()
    else()
        find_library(MUPDF_LIB NAMES mupdf)
        find_library(MUPDF_THIRD_LIB NAMES mupdf-third)
        find_path(MUPDF_INCLUDE NAMES mupdf/fitz.h)
    endif()
    if(NOT MUPDF_LIB OR NOT MUPDF_INCLUDE)
        message(WARNING "MuPDF not found (set MUPDF_ROOT); HARDCOPY_PDFIN disabled.")
        set(HARDCOPY_PDFIN OFF)
    endif()
endif()

if(HARDCOPY_TEXT_MODE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        freetype
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype"
    )
    FetchContent_MakeAvailable(freetype)

    if(HARDCOPY_PDFIN)
        set(harfbuzz_SOURCE_DIR "${MUPDF_ROOT}/harfbuzz")
        if(NOT EXISTS "${harfbuzz_SOURCE_DIR}/src/hb.h")
            message(FATAL_ERROR "MuPDF SDK is missing bundled HarfBuzz headers")
        endif()
    else()
        set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
        set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            harfbuzz
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/harfbuzz"
        )
        FetchContent_MakeAvailable(harfbuzz)
    endif()
endif()

set(HARDCOPY_NEEDS_JPEG_ENCODER OFF)
if(HARDCOPY_TEXT_MODE
   OR (HARDCOPY_COMIC AND HARDCOPY_DITHER)
   OR (HARDCOPY_PRINT AND HARDCOPY_DITHER))
    set(HARDCOPY_NEEDS_JPEG_ENCODER ON)
endif()

if(HARDCOPY_NEEDS_JPEG_ENCODER AND NOT HARDCOPY_PDFIN)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON  CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG ON CACHE BOOL "" FORCE)
    set(WITH_JAVA OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libjpeg_turbo
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo"
    )
    FetchContent_GetProperties(libjpeg_turbo)
    if(NOT libjpeg_turbo_POPULATED)
        FetchContent_Populate(libjpeg_turbo)
        file(READ "${libjpeg_turbo_SOURCE_DIR}/CMakeLists.txt" _ljt_cmake)
        string(REPLACE
            "message(FATAL_ERROR \"The libjpeg-turbo build system cannot be integrated into another build system using add_subdirectory().  Use ExternalProject_Add() instead.\")"
            "message(STATUS \"libjpeg-turbo add_subdirectory guard patched out.\")"
            _ljt_cmake "${_ljt_cmake}")
        file(WRITE "${libjpeg_turbo_SOURCE_DIR}/CMakeLists.txt" "${_ljt_cmake}")
        set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE STRING "" FORCE)
        set(CMAKE_INSTALL_DOCDIR "share/doc" CACHE STRING "" FORCE)
        set(CMAKE_INSTALL_MANDIR "share/man" CACHE STRING "" FORCE)
        add_subdirectory("${libjpeg_turbo_SOURCE_DIR}" "${libjpeg_turbo_BINARY_DIR}" EXCLUDE_FROM_ALL)
    endif()
endif()

if(HARDCOPY_DITHER)
    FetchContent_Declare(
        stb
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    )
    FetchContent_GetProperties(stb)
    if(NOT stb_POPULATED)
        FetchContent_Populate(stb)
    endif()
endif()

if(HARDCOPY_EPUB OR HARDCOPY_DOCX OR HARDCOPY_COMIC)
    set(MZ_FETCH_LIBS OFF CACHE BOOL "" FORCE)
    set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
    set(MZ_LZMA OFF CACHE BOOL "" FORCE)
    set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
    set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
    set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
    set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
    set(MZ_WZAES OFF CACHE BOOL "" FORCE)
    set(MZ_ICONV OFF CACHE BOOL "" FORCE)
    set(MZ_COMPAT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        minizip_ng
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minizip-ng"
    )
    FetchContent_MakeAvailable(minizip_ng)
endif()

if((HARDCOPY_EPUB OR HARDCOPY_DOCX) AND NOT pugixml_POPULATED)
    FetchContent_Declare(
        pugixml
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml"
    )
    FetchContent_MakeAvailable(pugixml)
endif()

set(HARDCOPY_SOURCES
    hardcopy/api.cpp
    hardcopy/print/pwg.cpp
    hardcopy/options.cpp
    hardcopy/preset_resolve.cpp
    hardcopy/pipeline.cpp
)

if(HARDCOPY_TEXT_MODE)
    list(APPEND HARDCOPY_SOURCES
        hardcopy/parser/txt_parser.cpp
        hardcopy/parser/md_parser.cpp
        hardcopy/parser/bilingual_parser.cpp
        hardcopy/source_export.cpp
        hardcopy/parser/text_normalizer.cpp
        hardcopy/parser/text_encoding.cpp
        hardcopy/parser/zh_convert.cpp
        hardcopy/layout/horizontal_layout.cpp
        hardcopy/render/font_face.cpp
        hardcopy/render/page_raster.cpp
        hardcopy/render/pdf_bitmap_processor.cpp
        hardcopy/render/pdf_columns.cpp
        hardcopy/render/pdf_line_spacing.cpp
        hardcopy/pdf/streaming_pdf_writer.cpp
    )
endif()

if(HARDCOPY_NEEDS_JPEG_ENCODER)
    list(APPEND HARDCOPY_SOURCES hardcopy/encode/jpeg.cpp hardcopy/encode/flate.cpp
        hardcopy/pdf/page_image_dump.cpp)
    set(_hc_libjpeg_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-src/thirdparty/libjpeg")
    set(_hc_libjpeg_cfg "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mupdf-src/scripts/libjpeg")
    if(HARDCOPY_PDFIN AND EXISTS "${_hc_libjpeg_src}/jpeglib.h" AND EXISTS "${_hc_libjpeg_cfg}/jconfig.h")
        list(APPEND HARDCOPY_SOURCES hardcopy/encode/jpeg_libjpeg.cpp)
        set_source_files_properties(hardcopy/encode/jpeg_libjpeg.cpp PROPERTIES
            INCLUDE_DIRECTORIES "${_hc_libjpeg_cfg};${_hc_libjpeg_src}")
        set_source_files_properties(hardcopy/encode/jpeg.cpp PROPERTIES
            COMPILE_DEFINITIONS HARDCOPY_JPEG_DIRECT=1)
        message(STATUS "JPEG: baseline encoder via MuPDF libjpeg (${_hc_libjpeg_src})")
    elseif(HARDCOPY_PDFIN)
        message(STATUS "JPEG: mupdf-src libjpeg headers missing; using MuPDF progressive writer")
    endif()
endif()

if(HARDCOPY_TEXT_MODE AND HARDCOPY_VERTICAL_LAYOUT)
    list(APPEND HARDCOPY_SOURCES hardcopy/layout/vertical_layout.cpp)
endif()
if(HARDCOPY_EPUB)
    list(APPEND HARDCOPY_SOURCES
        hardcopy/parser/epub_parser.cpp
        hardcopy/parser/css_subset.cpp
        hardcopy/parser/css_font_face.cpp
        hardcopy/parser/font_alias.cpp
        hardcopy/util/zip_reader.cpp)
endif()
if(HARDCOPY_DOCX)
    list(APPEND HARDCOPY_SOURCES hardcopy/parser/docx_parser.cpp)
    if(NOT HARDCOPY_EPUB)
        list(APPEND HARDCOPY_SOURCES hardcopy/util/zip_reader.cpp)
    endif()
endif()
if(HARDCOPY_PDFIN)
    list(APPEND HARDCOPY_SOURCES hardcopy/parser/pdf_parser.cpp
        hardcopy/parser/pdf_tables.cpp
        hardcopy/parser/pdf_reading_order.cpp
        hardcopy/pdf/annotation_exporter.cpp
        hardcopy/pdf/text_boxes.cpp)
    if(HARDCOPY_TEXT_MODE)
        list(APPEND HARDCOPY_SOURCES hardcopy/layout/epub_mupdf.cpp
            hardcopy/pdf/raster_reflow.cpp)
    endif()
    if(HARDCOPY_COMIC)
        list(APPEND HARDCOPY_SOURCES hardcopy/parser/pdf_comic_parser.cpp)
    endif()
endif()
if(HARDCOPY_DITHER)
    list(APPEND HARDCOPY_SOURCES
        hardcopy/image/dither.cpp
        hardcopy/image/image_decoder.cpp
        hardcopy/image/stb_impl.cpp
    )
endif()
if(HARDCOPY_COMIC)
    list(APPEND HARDCOPY_SOURCES
        hardcopy/parser/cbz_parser.cpp
        hardcopy/parser/mobi_parser.cpp
        hardcopy/util/zip_writer.cpp
    )
    if(NOT HARDCOPY_EPUB AND NOT HARDCOPY_DOCX)
        list(APPEND HARDCOPY_SOURCES hardcopy/util/zip_reader.cpp)
    endif()
endif()
if(HARDCOPY_PRINT AND HARDCOPY_DITHER)
    list(APPEND HARDCOPY_SOURCES hardcopy/print/print_layout.cpp)
endif()
set(_hc_libwebp_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libwebp")
set(HARDCOPY_WEBP OFF)
if(EXISTS "${_hc_libwebp_dir}/src/webp/decode.h")
    set(HARDCOPY_WEBP ON)
    if(HARDCOPY_PDFIN AND HARDCOPY_NEEDS_JPEG_ENCODER)
        list(APPEND HARDCOPY_SOURCES hardcopy/layout/epub_webp.cpp)
    endif()
endif()
list(REMOVE_DUPLICATES HARDCOPY_SOURCES)

add_library(hardcopy_core STATIC ${HARDCOPY_SOURCES})

target_include_directories(hardcopy_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/hardcopy/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/hardcopy
)

if(HARDCOPY_NEEDS_JPEG_ENCODER AND NOT HARDCOPY_PDFIN)
    FetchContent_GetProperties(libjpeg_turbo)
    target_include_directories(hardcopy_core PRIVATE
        ${libjpeg_turbo_SOURCE_DIR}
        ${libjpeg_turbo_BINARY_DIR}
    )
endif()

target_link_libraries(hardcopy_core
    PRIVATE nlohmann_json::nlohmann_json
)

if(HARDCOPY_TEXT_MODE)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_TEXT_MODE=1)
    target_link_libraries(hardcopy_core PRIVATE freetype)
    if(HARDCOPY_PDFIN)
        target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HB_MUPDF=1)
        target_include_directories(hardcopy_core PRIVATE ${harfbuzz_SOURCE_DIR}/src)
        target_compile_options(hardcopy_core PRIVATE
            "-include" "${CMAKE_CURRENT_SOURCE_DIR}/hardcopy/hb_mupdf_prefix.h")
    else()
        target_link_libraries(hardcopy_core PRIVATE harfbuzz)
    endif()
endif()

if(HARDCOPY_NEEDS_JPEG_ENCODER)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_JPEG=1)
    if(NOT HARDCOPY_PDFIN)
        target_link_libraries(hardcopy_core PRIVATE turbojpeg-static)
    endif()
endif()

set(_hc_libdeflate_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdeflate")
if(HARDCOPY_NEEDS_JPEG_ENCODER AND EXISTS "${_hc_libdeflate_dir}/CMakeLists.txt")
    set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(LIBDEFLATE_DECOMPRESSION_SUPPORT OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_GZIP_SUPPORT OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
    set(LIBDEFLATE_USER_SET_RELEASE_FLAGS ON)
    add_subdirectory("${_hc_libdeflate_dir}" EXCLUDE_FROM_ALL)
    target_link_libraries(hardcopy_core PRIVATE libdeflate::libdeflate_static)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_LIBDEFLATE=1)
    message(STATUS "Flate: libdeflate (${_hc_libdeflate_dir})")
elseif(HARDCOPY_NEEDS_JPEG_ENCODER)
    message(STATUS "Flate: libdeflate not found; using MuPDF zlib when PDFIN is on")
endif()

if(HARDCOPY_WEBP)
    enable_language(C)
    set(_hc_webp_src "${_hc_libwebp_dir}/src")
    add_library(hc_webpdecoder STATIC
        ${_hc_webp_src}/dec/alpha_dec.c
        ${_hc_webp_src}/dec/buffer_dec.c
        ${_hc_webp_src}/dec/frame_dec.c
        ${_hc_webp_src}/dec/idec_dec.c
        ${_hc_webp_src}/dec/io_dec.c
        ${_hc_webp_src}/dec/quant_dec.c
        ${_hc_webp_src}/dec/tree_dec.c
        ${_hc_webp_src}/dec/vp8_dec.c
        ${_hc_webp_src}/dec/vp8l_dec.c
        ${_hc_webp_src}/dec/webp_dec.c
        ${_hc_webp_src}/dsp/alpha_processing.c
        ${_hc_webp_src}/dsp/cpu.c
        ${_hc_webp_src}/dsp/dec.c
        ${_hc_webp_src}/dsp/dec_clip_tables.c
        ${_hc_webp_src}/dsp/filters.c
        ${_hc_webp_src}/dsp/lossless.c
        ${_hc_webp_src}/dsp/rescaler.c
        ${_hc_webp_src}/dsp/upsampling.c
        ${_hc_webp_src}/dsp/yuv.c
        ${_hc_webp_src}/dsp/alpha_processing_sse2.c
        ${_hc_webp_src}/dsp/dec_sse2.c
        ${_hc_webp_src}/dsp/filters_sse2.c
        ${_hc_webp_src}/dsp/lossless_sse2.c
        ${_hc_webp_src}/dsp/rescaler_sse2.c
        ${_hc_webp_src}/dsp/upsampling_sse2.c
        ${_hc_webp_src}/dsp/yuv_sse2.c
        ${_hc_webp_src}/dsp/alpha_processing_sse41.c
        ${_hc_webp_src}/dsp/dec_sse41.c
        ${_hc_webp_src}/dsp/lossless_sse41.c
        ${_hc_webp_src}/dsp/upsampling_sse41.c
        ${_hc_webp_src}/dsp/yuv_sse41.c
        ${_hc_webp_src}/dsp/lossless_avx2.c
        ${_hc_webp_src}/dsp/alpha_processing_neon.c
        ${_hc_webp_src}/dsp/dec_neon.c
        ${_hc_webp_src}/dsp/filters_neon.c
        ${_hc_webp_src}/dsp/lossless_neon.c
        ${_hc_webp_src}/dsp/rescaler_neon.c
        ${_hc_webp_src}/dsp/upsampling_neon.c
        ${_hc_webp_src}/dsp/yuv_neon.c
        ${_hc_webp_src}/utils/bit_reader_utils.c
        ${_hc_webp_src}/utils/color_cache_utils.c
        ${_hc_webp_src}/utils/filters_utils.c
        ${_hc_webp_src}/utils/huffman_utils.c
        ${_hc_webp_src}/utils/palette.c
        ${_hc_webp_src}/utils/quant_levels_dec_utils.c
        ${_hc_webp_src}/utils/rescaler_utils.c
        ${_hc_webp_src}/utils/random_utils.c
        ${_hc_webp_src}/utils/thread_utils.c
        ${_hc_webp_src}/utils/utils.c
    )
    target_include_directories(hc_webpdecoder
        PRIVATE "${_hc_libwebp_dir}"
        PUBLIC  "${_hc_webp_src}")
    target_link_libraries(hardcopy_core PRIVATE hc_webpdecoder)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_WEBP=1)
    if(HARDCOPY_PDFIN AND HARDCOPY_NEEDS_JPEG_ENCODER)
        target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_EPUB_WEBP=1)
    endif()
    message(STATUS "WebP: libwebp decoder (${_hc_libwebp_dir})")
else()
    message(STATUS "WebP: third_party/libwebp not found; WebP pictures are not decoded")
endif()

target_sources(hardcopy_core PRIVATE hardcopy/ocr/ppocr.cpp)
if(HARDCOPY_PDFIN)
    target_sources(hardcopy_core PRIVATE hardcopy/ocr/pdf_ocr.cpp)
endif()
set(_hc_ncnn_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ncnn")
set(_hc_ppocr_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ppocr")
set(_hc_ncnn_target "")
if(ANDROID OR CMAKE_ANDROID_ARCH_ABI)
    if(CMAKE_ANDROID_ARCH_ABI)
        set(_hc_ncnn_target "${CMAKE_ANDROID_ARCH_ABI}")
    else()
        set(_hc_ncnn_target "${ANDROID_ABI}")
    endif()
elseif(MINGW)
    set(_hc_ncnn_target "mingw-x86_64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(_hc_ncnn_target "linux-x86_64")
endif()
set(_hc_ncnn_lib "")
if(_hc_ncnn_target)
    foreach(_cand libncnn.a libncnn.so.1)
        if(NOT _hc_ncnn_lib AND EXISTS "${_hc_ncnn_root}/${_hc_ncnn_target}/lib/${_cand}")
            set(_hc_ncnn_lib "${_hc_ncnn_root}/${_hc_ncnn_target}/lib/${_cand}")
        endif()
    endforeach()
endif()
if(_hc_ncnn_lib AND EXISTS "${_hc_ppocr_dir}/PP_OCRv5_mobile_rec.ncnn.bin")
    target_sources(hardcopy_core PRIVATE hardcopy/ocr/ppocr_models.cpp)
    set(_hc_ppocr_models
        "${_hc_ppocr_dir}/PP_OCRv5_mobile_det.ncnn.param"
        "${_hc_ppocr_dir}/PP_OCRv5_mobile_det.ncnn.bin"
        "${_hc_ppocr_dir}/PP_OCRv5_mobile_rec.ncnn.param"
        "${_hc_ppocr_dir}/PP_OCRv5_mobile_rec.ncnn.bin")
    set_source_files_properties(hardcopy/ocr/ppocr_models.cpp PROPERTIES
        COMPILE_DEFINITIONS "HARDCOPY_PPOCR_DIR=\"${_hc_ppocr_dir}\""
        OBJECT_DEPENDS "${_hc_ppocr_models}")
    target_include_directories(hardcopy_core PRIVATE
        "${_hc_ncnn_root}/${_hc_ncnn_target}/include/ncnn" "${_hc_ppocr_dir}")
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_OCR=1)
    target_link_libraries(hardcopy_core PUBLIC "${_hc_ncnn_lib}")
    if(ANDROID OR CMAKE_ANDROID_ARCH_ABI)
        target_link_options(hardcopy_core INTERFACE -fopenmp -static-openmp)
        target_link_libraries(hardcopy_core PUBLIC android log)
    elseif(_hc_ncnn_lib MATCHES "\\.a$")
        find_package(OpenMP)
        if(OpenMP_CXX_FOUND)
            target_link_libraries(hardcopy_core PUBLIC OpenMP::OpenMP_CXX)
        endif()
    endif()
    message(STATUS "OCR: ncnn ${_hc_ncnn_lib}")
else()
    message(STATUS "OCR: no ncnn library for '${_hc_ncnn_target}'; pdfOcr unavailable")
endif()

if(HARDCOPY_DITHER)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_DITHER=1)
    target_include_directories(hardcopy_core PRIVATE ${stb_SOURCE_DIR})
endif()
if(HARDCOPY_TEXT_MODE AND HARDCOPY_VERTICAL_LAYOUT)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_VERTICAL_LAYOUT=1)
endif()
if(HARDCOPY_COMIC)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_COMIC=1)
    if(NOT HARDCOPY_EPUB AND NOT HARDCOPY_DOCX)
        target_link_libraries(hardcopy_core PRIVATE MINIZIP::minizip)
    endif()
endif()
if(HARDCOPY_EPUB)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_EPUB=1)
    target_link_libraries(hardcopy_core PRIVATE MINIZIP::minizip pugixml::pugixml)
endif()
if(HARDCOPY_DOCX)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_DOCX=1)
    target_link_libraries(hardcopy_core PRIVATE MINIZIP::minizip pugixml::pugixml)
endif()
if(HARDCOPY_PDFIN)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_PDFIN=1)
    target_include_directories(hardcopy_core PRIVATE ${MUPDF_INCLUDE})
    target_link_libraries(hardcopy_core PRIVATE ${MUPDF_LIB})
    if(MUPDF_THIRD_LIB)
        target_link_libraries(hardcopy_core PRIVATE ${MUPDF_THIRD_LIB})
    endif()
endif()
if(HARDCOPY_TEXT_MODE AND HARDCOPY_PARALLEL)
    target_compile_definitions(hardcopy_core PRIVATE HARDCOPY_HAS_PARALLEL=1)
    find_package(Threads REQUIRED)
    target_link_libraries(hardcopy_core PRIVATE Threads::Threads)
endif()

if(HARDCOPY_BUILD_CLI)
    add_subdirectory(cli)
endif()
if(HARDCOPY_BUILD_GUI)
    add_subdirectory(gui)
endif()

function(hardcopy_optimize_tree dir)
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type MATCHES "STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE")
            target_compile_options(${_t} PRIVATE
                "$<$<NOT:$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:-O2>")
        endif()
    endforeach()
    get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_s IN LISTS _subs)
        hardcopy_optimize_tree("${_s}")
    endforeach()
endfunction()
if(HARDCOPY_OPTIMIZE_ALL_CONFIGS AND NOT MSVC)
    hardcopy_optimize_tree("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
