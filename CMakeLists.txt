cmake_minimum_required(VERSION 3.10)
project(ebook_reader LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include src third_party)

# ==================== 番茄下载器（外置 / 内嵌可选） ====================
#
# 默认外置：下载器作为独立文件放在 exe 同目录，编译后自动复制。
#           优点：不触发杀软 Dropper 启发式；主程序体积小（约 0.7 MB）。
#
# 内嵌：  -DEMBED_FANQIE=ON 把下载器编译进 exe，运行时释放到用户目录。
#           优点：单文件分发；缺点：体积 +10.9 MB，多数杀软会报 HEUR/QVM。
option(EMBED_FANQIE "Embed fanqie downloader into exe (may trigger AV heuristics)" OFF)

if(WIN32)
    enable_language(RC)
    set(VERSION_RESOURCE ${CMAKE_SOURCE_DIR}/resources/version.rc)
    set(FANQIE_TOOL_SRC ${CMAKE_SOURCE_DIR}/tools/tomato/win64/TomatoNovelDownloader.exe)
    set(FANQIE_TOOL_NAME TomatoNovelDownloader.exe)
else()
    set(VERSION_RESOURCE)
    if(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(FANQIE_PLATFORM_DIR macos_arm64)
        else()
            set(FANQIE_PLATFORM_DIR macos_amd64)
        endif()
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(FANQIE_PLATFORM_DIR linux_arm64)
        else()
            set(FANQIE_PLATFORM_DIR linux_amd64)
        endif()
    endif()
    set(FANQIE_TOOL_SRC ${CMAKE_SOURCE_DIR}/tools/tomato/${FANQIE_PLATFORM_DIR}/TomatoNovelDownloader)
    set(FANQIE_TOOL_NAME TomatoNovelDownloader)
endif()

set(FANQIE_CONFIG_SRC ${CMAKE_SOURCE_DIR}/tools/tomato/config.yml)

if(EXISTS ${FANQIE_TOOL_SRC})
    set(FANQIE_TOOL_AVAILABLE TRUE)
else()
    set(FANQIE_TOOL_AVAILABLE FALSE)
endif()

# ---- 内嵌资源准备 ----
set(FANQIE_EMBED_RESOURCE)
if(EMBED_FANQIE AND FANQIE_TOOL_AVAILABLE)
    if(WIN32)
        set(FANQIE_EXE_PATH ${FANQIE_TOOL_SRC})
        set(FANQIE_CONFIG_PATH ${FANQIE_CONFIG_SRC})
        configure_file(
            ${CMAKE_SOURCE_DIR}/resources/fanqie.rc.in
            ${CMAKE_BINARY_DIR}/fanqie.rc
            @ONLY
        )
        set(FANQIE_EMBED_RESOURCE ${CMAKE_BINARY_DIR}/fanqie.rc)
        add_definitions(-DHAS_EMBEDDED_FANQIE)
        message(STATUS "Fanqie: EMBEDDED into exe (single-file, may trigger AV)")
    else()
        # POSIX：objcopy 把二进制嵌入为可链接对象
        if(APPLE)
            set(FANQIE_OBJCOPY_TARGET "mach-o-x86-64")
        else()
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(FANQIE_OBJCOPY_TARGET "elf64-littleaarch64")
                set(FANQIE_OBJCOPY_ARCH "aarch64")
            else()
                set(FANQIE_OBJCOPY_TARGET "elf64-x86-64")
                set(FANQIE_OBJCOPY_ARCH "i386:x86-64")
            endif()
        endif()
        set(FANQIE_EXE_OBJ ${CMAKE_BINARY_DIR}/fanqie_exe.o)
        find_program(OBJCOPY_EXE objcopy)
        if(OBJCOPY_EXE)
            add_custom_command(
                OUTPUT ${FANQIE_EXE_OBJ}
                COMMAND ${OBJCOPY_EXE}
                        --input-target binary
                        --output-target ${FANQIE_OBJCOPY_TARGET}
                        --binary-architecture ${FANQIE_OBJCOPY_ARCH}
                        TomatoNovelDownloader ${FANQIE_EXE_OBJ}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/tomato/${FANQIE_PLATFORM_DIR}
                DEPENDS ${FANQIE_TOOL_SRC}
                COMMENT "Embedding fanqie downloader"
            )
            set(FANQIE_EMBED_RESOURCE ${FANQIE_EXE_OBJ})
            add_definitions(-DHAS_EMBEDDED_FANQIE)
            message(STATUS "Fanqie: EMBEDDED into exe (single-file, may trigger AV)")
        else()
            message(WARNING "objcopy not found; falling back to external mode")
            set(EMBED_FANQIE OFF)
        endif()
    endif()
elseif(EMBED_FANQIE AND NOT FANQIE_TOOL_AVAILABLE)
    message(WARNING "EMBED_FANQIE=ON but downloader not found; falling back to external")
    set(EMBED_FANQIE OFF)
endif()

if(NOT EMBED_FANQIE AND FANQIE_TOOL_AVAILABLE)
    message(STATUS "Fanqie: EXTERNAL (copied to output dir)")
elseif(NOT FANQIE_TOOL_AVAILABLE)
    message(STATUS "Fanqie: downloader not found; fanqie feature unavailable")
endif()

# ==================== TUI 后端 ====================

if(WIN32)
    add_definitions(-DPLATFORM_WINDOWS)
    set(TUI_SOURCES src/tui_windows.cpp)
else()
    add_definitions(-DPLATFORM_UNIX)
    find_package(Curses REQUIRED)
    include_directories(${CURSES_INCLUDE_DIR})
    set(TUI_SOURCES src/tui_ncurses.cpp)
endif()

# ==================== 可选：HTTP API 测试服务 ====================
# 启用后可通过 `ebook_reader --serve [--port 8686]` 以纯 HTTP 方式调用所有功能，
# 不进入 TUI，适合自动化测试。默认关闭。
option(ENABLE_API "Expose all features as HTTP API endpoints (test mode)" OFF)

# ==================== 可选：音乐播放器 ====================
# 默认关闭。开启后包含本地曲库、歌词、歌单、在线搜索等完整功能。
# 依赖：libcurl（在线搜索）、miniaudio（third_party/ 内置）、Windows winmm/ole32
option(ENABLE_MUSIC "Enable built-in music player" OFF)

# ==================== 静态链接 ====================
# ON: 把 zlib / libcurl 及其依赖 / libstdc++ / libgcc / winpthread 全部静态编入 exe，
#     产物不依赖任何第三方 DLL。代价是体积增大（约 +8 MB）。
option(STATIC_LINK "Statically link all third-party libraries (no DLL dependencies)" OFF)

if(STATIC_LINK AND WIN32)
    find_program(PKG_CONFIG_EXE pkg-config)
    if(NOT PKG_CONFIG_EXE)
        message(FATAL_ERROR "STATIC_LINK requires pkg-config to resolve libcurl static deps")
    endif()
    # -s: 链接时直接 strip 符号表，静态版体积从约 15 MB 降到 11 MB
    add_link_options(-static -static-libgcc -static-libstdc++ -s)
    message(STATUS "Static linking: enabled (-static -static-libgcc -static-libstdc++ -s)")
endif()

# ==================== 可选 libcurl ====================
if(STATIC_LINK AND WIN32)
    # 静态模式下不依赖 CURL 的 CMake 配置，直接用 pkg-config 取全部静态依赖
    execute_process(
        COMMAND ${PKG_CONFIG_EXE} --libs --static libcurl
        OUTPUT_VARIABLE CURL_STATIC_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${PKG_CONFIG_EXE} --cflags libcurl
        OUTPUT_VARIABLE CURL_STATIC_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT CURL_STATIC_LIBS)
        message(FATAL_ERROR "pkg-config --libs --static libcurl returned nothing")
    endif()
    separate_arguments(CURL_STATIC_LIB_LIST UNIX_COMMAND "${CURL_STATIC_LIBS}")
    # -LD:/... 形式的库路径要转成 CMake 的 link_directories
    set(CURL_LINK_DIRS)
    set(CURL_STATIC_LIBS_CLEAN)
    foreach(arg ${CURL_STATIC_LIB_LIST})
        if(arg MATCHES "^-L(.+)$")
            list(APPEND CURL_LINK_DIRS "${CMAKE_MATCH_1}")
        else()
            list(APPEND CURL_STATIC_LIBS_CLEAN "${arg}")
        endif()
    endforeach()
    link_directories(${CURL_LINK_DIRS})
    # -lcurl 在 MinGW 下会优先选中 libcurl.dll.a（import lib），必须换成完整路径
    find_library(CURL_STATIC_ARCHIVE NAMES libcurl.a curl)
    if(NOT CURL_STATIC_ARCHIVE)
        message(FATAL_ERROR "STATIC_LINK: libcurl.a not found")
    endif()
    list(INSERT CURL_STATIC_LIBS_CLEAN 0 "${CURL_STATIC_ARCHIVE}")
    add_definitions(-DCURL_STATICLIB)
    set(CURL_FOUND TRUE)
    set(CURL_STATIC_MODE TRUE)
    message(STATUS "libcurl: STATIC (${CURL_STATIC_ARCHIVE})")
else()
find_package(CURL)
endif()

if(CURL_FOUND)
    include_directories(${CURL_INCLUDE_DIRS})
    add_definitions(-DHAS_CURL)
    set(NET_SOURCES
        src/http_client.cpp
        src/site_parser.cpp
        src/fanqie_downloader.cpp
        src/fanqie_resource.cpp
        src/sonovel.cpp
    )
else()
    set(NET_SOURCES)
    message(STATUS "libcurl not found. Online reading disabled.")
endif()

# ==================== 可选：本地音乐 API ====================
# 启用：cmake -DENABLE_LOCAL_MUSIC_API=ON -DLOCAL_MUSIC_API_BASE=http://127.0.0.1:3000
# 需先在本地运行 NeteaseCloudMusicApi（或兼容服务）
option(ENABLE_LOCAL_MUSIC_API "Enable local NeteaseCloudMusicApi source" OFF)
set(LOCAL_MUSIC_API_BASE "http://127.0.0.1:3000" CACHE STRING "Local music API base URL")

if(ENABLE_LOCAL_MUSIC_API)
    if(NOT ENABLE_MUSIC)
        message(WARNING "ENABLE_LOCAL_MUSIC_API requires ENABLE_MUSIC=ON; ignored")
    elseif(CURL_FOUND)
        add_definitions(-DHAS_LOCAL_MUSIC_API)
        add_definitions(-DLOCAL_MUSIC_API_BASE="${LOCAL_MUSIC_API_BASE}")
        message(STATUS "Local music API enabled: ${LOCAL_MUSIC_API_BASE}")
    else()
        message(WARNING "ENABLE_LOCAL_MUSIC_API requires libcurl; ignored")
    endif()
endif()


# ==================== 源文件 ====================
set(SOURCES
    src/main.cpp
    src/reader.cpp
    src/bookmark.cpp
    src/config.cpp
    src/parser_txt.cpp
    src/zip_reader.cpp
    src/markup_util.cpp
    src/encoding.cpp
    src/fs_util.cpp
    src/parser_epub.cpp
    src/parser_docx.cpp
    src/parser_pdf.cpp
    src/mcp_client.cpp
    src/mcp_pool.cpp
    ${TUI_SOURCES}
    ${NET_SOURCES}
    ${VERSION_RESOURCE}
    ${FANQIE_EMBED_RESOURCE}
)

add_executable(ebook_reader ${SOURCES})

# ==================== HTTP API 测试服务源文件 ====================
if(ENABLE_API)
    if(NOT CURL_FOUND)
        message(WARNING "ENABLE_API without libcurl: only reading/config endpoints available")
    endif()
    add_definitions(-DHAS_API)
    target_sources(ebook_reader PRIVATE
        src/http_server.cpp
        src/api_routes.cpp
    )
    message(STATUS "HTTP API test server enabled (--serve)")
endif()

# 外置模式：构建后把下载器与配置复制到输出目录
# 内嵌模式：不复制，运行时自行释放
if(FANQIE_TOOL_AVAILABLE AND NOT EMBED_FANQIE)
    add_custom_command(TARGET ebook_reader POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FANQIE_TOOL_SRC}
                ${CMAKE_BINARY_DIR}/${FANQIE_TOOL_NAME}
        COMMENT "Copying fanqie downloader (external mode)"
    )
    if(EXISTS ${FANQIE_CONFIG_SRC})
        add_custom_command(TARGET ebook_reader POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${FANQIE_CONFIG_SRC}
                    ${CMAKE_BINARY_DIR}/config.yml
            COMMENT "Copying fanqie config (external mode)"
        )
    endif()
endif()

if(UNIX)
    target_link_libraries(ebook_reader ${CURSES_LIBRARIES})
    target_link_libraries(ebook_reader pthread)
endif()

if(WIN32)
    # crypt32: 网易云会话文件用 DPAPI 保护 cookie
    target_link_libraries(ebook_reader ws2_32 shell32 winmm ole32 crypt32)
endif()

if(CURL_FOUND)
    if(CURL_STATIC_MODE)
        target_link_libraries(ebook_reader ${CURL_STATIC_LIBS_CLEAN})
    else()
        target_link_libraries(ebook_reader ${CURL_LIBRARIES})
    endif()
endif()

# ZIP 解压（EPUB / DOCX 解析所需）
if(STATIC_LINK AND WIN32)
    # 静态模式下不调用 find_package(ZLIB)，直接从工具链前缀目录找 libz.a
    if(NOT ZLIB_STATIC_LIB)
        find_library(ZLIB_STATIC_LIB NAMES libz.a zlibstatic)
    endif()
    if(NOT ZLIB_STATIC_INCLUDE)
        find_path(ZLIB_STATIC_INCLUDE zlib.h)
    endif()
    if(NOT ZLIB_STATIC_LIB)
        message(FATAL_ERROR "STATIC_LINK: libz.a not found")
    endif()
    include_directories(${ZLIB_STATIC_INCLUDE})
    target_link_libraries(ebook_reader ${ZLIB_STATIC_LIB})
    message(STATUS "zlib: STATIC (${ZLIB_STATIC_LIB})")
else()
    find_package(ZLIB REQUIRED)
    include_directories(${ZLIB_INCLUDE_DIRS})
    target_link_libraries(ebook_reader ${ZLIB_LIBRARIES})

    # Windows 动态链接时需要把 zlib1.dll 放到 exe 同目录（FindZLIB 不提供 DLL 路径，自行推导）
    if(WIN32)
        get_filename_component(_ZLIB_LIB_DIR "${ZLIB_LIBRARY_RELEASE}" DIRECTORY)
        get_filename_component(_ZLIB_PREFIX_DIR "${_ZLIB_LIB_DIR}" DIRECTORY)
        find_file(ZLIB_DLL_FILE
                  NAMES zlib1.dll zlib.dll z.dll
                  PATHS "${_ZLIB_PREFIX_DIR}/bin" "${_ZLIB_LIB_DIR}"
                  NO_DEFAULT_PATH)
        if(ZLIB_DLL_FILE)
            add_custom_command(TARGET ebook_reader POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${ZLIB_DLL_FILE}
                        ${CMAKE_BINARY_DIR}/
                COMMENT "Copying zlib runtime"
            )
            message(STATUS "zlib runtime: ${ZLIB_DLL_FILE}")
        else()
            message(STATUS "zlib runtime: not found (zlib1.dll must be on PATH)")
        endif()
    endif()
endif()

install(TARGETS ebook_reader DESTINATION bin)

# ==================== 音乐播放器源文件 ====================
if(ENABLE_MUSIC)
    if(NOT CURL_FOUND)
        message(WARNING "ENABLE_MUSIC requires libcurl; music player disabled")
    else()
        add_definitions(-DHAS_MUSIC)
        target_sources(ebook_reader PRIVATE
            src/music.cpp
            src/player.cpp
            src/playlist_store.cpp
            src/music_ui.cpp
            src/online_music.cpp
            src/netease_api.cpp
            src/term_qr.cpp
            third_party/qrcodegen.cpp
        )
        if(WIN32)
            target_link_libraries(ebook_reader winmm ole32)
        endif()

        # 网易云 eapi 需要 AES-128-ECB + MD5
        find_package(OpenSSL REQUIRED)
        include_directories(${OPENSSL_INCLUDE_DIR})
        target_link_libraries(ebook_reader ${OPENSSL_CRYPTO_LIBRARY})
        message(STATUS "Music player enabled (netease eapi via ${OPENSSL_CRYPTO_LIBRARY})")
    endif()
endif()


# POSIX 需要 iconv 做编码转换（glibc 内置，macOS 需额外链接）
if(APPLE)
    find_library(ICONV_LIB iconv)
    if(ICONV_LIB)
        target_link_libraries(ebook_reader ${ICONV_LIB})
    endif()
endif()


# ==================== 测试目标 ====================
if(CURL_FOUND)
    add_executable(fanqie_test
        tools/fanqie_test.cpp
        src/fanqie_downloader.cpp
        src/fanqie_resource.cpp
        src/http_client.cpp
        src/fs_util.cpp
        ${VERSION_RESOURCE}
        ${FANQIE_EMBED_RESOURCE}
    )
    if(CURL_STATIC_MODE)
        target_link_libraries(fanqie_test ${CURL_STATIC_LIBS_CLEAN})
    else()
        target_link_libraries(fanqie_test ${CURL_LIBRARIES})
    endif()
    if(WIN32)
        target_link_libraries(fanqie_test ws2_32)
    endif()
endif()
