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

# ==================== 可选：音乐播放器 ====================
# 默认关闭。开启后包含本地曲库、歌词、歌单、在线搜索等完整功能。
# 依赖：libcurl（在线搜索）、miniaudio（third_party/ 内置）、Windows winmm/ole32
option(ENABLE_MUSIC "Enable built-in music player" OFF)

# ==================== 可选 libcurl ====================
find_package(CURL)
if(CURL_FOUND)
    include_directories(${CURL_INCLUDE_DIRS})
    add_definitions(-DHAS_CURL)
    set(NET_SOURCES
        src/http_client.cpp
        src/site_parser.cpp
        src/fanqie_downloader.cpp
        src/fanqie_resource.cpp
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
    src/config.cpp
    src/parser_txt.cpp
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
    target_link_libraries(ebook_reader ws2_32 shell32 winmm ole32)
endif()

if(CURL_FOUND)
    target_link_libraries(ebook_reader ${CURL_LIBRARIES})
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
        )
        if(WIN32)
            target_link_libraries(ebook_reader winmm ole32)
        endif()
        message(STATUS "Music player enabled")
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
    target_link_libraries(fanqie_test ${CURL_LIBRARIES})
    if(WIN32)
        target_link_libraries(fanqie_test ws2_32)
    endif()
endif()
