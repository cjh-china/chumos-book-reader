# TUI Ebook Reader

A cross-platform command-line ebook reader with a built-in music player.

Runs on Windows, Linux and macOS. Pure terminal UI, no GUI dependencies.

## Features

### Reading

- Library browser with recent files, custom folders, and auto-indexed download folder
- TXT support with automatic encoding detection (UTF-8 / UTF-16 / GBK / GB18030)
- Chinese paths handled correctly on Windows (wide-char API throughout)
- Smooth scrolling, paging, and chapter navigation
- In-book search with next/prev match jump
- UTF-8 rendering with East-Asian wide-character aware line wrapping

EPUB and DOCX are parsed natively (no third-party libraries beyond zlib):

- EPUB: reads `META-INF/container.xml` → OPF → spine order, extracts `dc:title` / `dc:creator`,
  and uses `toc.ncx` titles as chapter names
- DOCX: reads `word/document.xml` paragraph by paragraph, plus `docProps/core.xml` metadata
- Both go through a built-in minimal ZIP reader (stored / deflate)

> PDF is still a stub: it only prints a message. Only TXT, EPUB and DOCX are fully supported.

### Fanqie novel download

- Search novels on Fanqie, download as plain text
- Downloaded files land in `downloads/` and are auto-indexed into the library
- Missing files are re-downloaded automatically when opened (if a same-title book exists)
- Uses the bundled [Tomato-Novel-Downloader](https://github.com/zhongbai2333/Tomato-Novel-Downloader)

### so-novel book sources

- Aggregate search across 17 web novel sources via a local [so-novel](https://github.com/freeok/so-novel) WebUI
- Auto-starts `sonovel.exe` when the service is not running (configurable)
- Download to text, copied into `downloads/` and opened immediately
- Install directory is auto-detected, or set it in Settings (`U`)

### Music player (optional, `-DENABLE_MUSIC=ON`)

- Local library: MP3 / FLAC / WAV / OGG with ID3v2 and FLAC Vorbis Comment tag reading
- Playback via [miniaudio](https://github.com/mackron/miniaudio) (bundled)
- Play modes: sequential / shuffle / repeat-one
- Synced LRC lyrics with multi-timestamp and offset support
- Playlists, favorites, play history (persisted to `music_store.json`)
- ID3v2.3 tag editing (title / artist / album)
- Sleep timer: 15 / 30 / 60 minutes, or end-of-track
- Online search across multiple sources: NetEase (native), myfreemp3, Audius, iTunes
- Online download: audio + lyrics + cover, auto-indexed into library
- Download reports which metadata failed (lyrics / cover) instead of silently skipping

### NetEase cloud music (built in, no Node required)

The NetEase source talks to `interface.music.163.com` directly over the `eapi`
protocol, implemented natively in C++ (AES-128-ECB + MD5 via OpenSSL). No Node
runtime, no extra process. Requires `-DENABLE_MUSIC=ON`.

### Optional local music API (`-DENABLE_LOCAL_MUSIC_API=ON`)

Connects to a locally running [NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi)
for the full NetEase catalogue, including songs the public CDN blocks.

The install directory is auto-detected (env `NETEASE_API_HOME` → npm global
`node_modules` → `tools/NeteaseCloudMusicApi` next to the exe). If the service is
not running, it is started automatically via `node app.js`; if Node is missing the
source is simply skipped and the other three sources still work.

## Documentation

- [BUILD.md](BUILD.md) — Full build guide: dependencies, compile options, build combinations
- [BUILD_WINDOWS.md](BUILD_WINDOWS.md) — Windows build instructions
- [BUILD_UNIX.md](BUILD_UNIX.md) — Linux / macOS build instructions
- [ONLINE_READING.md](ONLINE_READING.md) — Online reading and Fanqie novel download

## Quick build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Add `-DENABLE_MUSIC=ON` for the music player, `-DEMBED_FANQIE=ON` for a single-file exe,
`-DSTATIC_LINK=ON` to link every third-party library statically (no DLLs needed).
See [BUILD.md](BUILD.md) for all options.

## Usage

```bash
./ebook_reader                    # open the library
./ebook_reader <file>            # open a file directly
./ebook_reader --fanqie          # standalone Fanqie download CLI
./ebook_reader --help            # show usage
```

## Controls

### Library

| Key | Action |
|---|---|
| Up/Down, PgUp/PgDn, Home/End | Navigate |
| Enter | Open book |
| `/` | Filter by name |
| `s` | Cycle sort (name / size / time) |
| `F` | Fanqie novel search |
| `M` | Music player (if built with `ENABLE_MUSIC`) |
| `a` / `d` | Add / remove library folder |
| `r` | Rescan |
| `U` | Settings (auto-update + so-novel) |
| `q` | Quit |

### Reader

| Key | Action |
|---|---|
| Right / Left, Space / b, PgDn / PgUp | Next / previous page |
| Down / Up, j / k | Scroll one line |
| g / G | First / last page |
| `t` | Table of contents |
| `/` | Search menu (in-book / Fanqie / so-novel sources) |
| `F` | Fanqie search |
| `n` / `N` | Next / previous match |
| `h`, `?` | Help |
| `q`, Esc | Back to library |

### Music (if built with `ENABLE_MUSIC`)

Library page:

| Key | Action |
|---|---|
| Up/Down, PgUp/PgDn | Navigate |
| Enter | Play (current filter becomes the playlist) |
| Space | Play / pause |
| `/` | Filter |
| `e` | Edit tags |
| `a` / `r` | Add scan folder / rescan |
| `f` | Toggle favorite |
| `p` | Add to playlist |
| `O` | Online search |
| `P` | Playlists |
| Tab | Player page |

Player page:

| Key | Action |
|---|---|
| Space | Play / pause |
| Left / Right | Seek -5s / +5s |
| Up / Down | Volume +/- 5% |
| n / p | Next / previous track |
| `m` | Cycle play mode |
| `l` | Toggle lyrics |
| `z` | Sleep timer |
| `O` | Online search |

Online search page:

| Key | Action |
|---|---|
| Enter | Play (downloads to cache first) |
| Space / `x` | Mark / unmark, auto-advance |
| `a` | Select all / none |
| `d` / `D` | Download current / all marked |
| `/` | New search |

## Runtime files

Created next to the executable:

| Path | Content |
|---|---|
| `downloads/` | Downloaded novels (txt) |
| `downloads/music/` | Downloaded music (mp3 + lrc + jpg) |
| `cache/` | Online playback cache |
| `ebook_reader.config.json` | Library settings |
| `music_store.json` | Playlists, favorites, history |

`downloads/` and `downloads/music/` are indexed automatically.

## License

GPL-3.0. The bundled Tomato-Novel-Downloader is a separate GPL-3.0 project.
