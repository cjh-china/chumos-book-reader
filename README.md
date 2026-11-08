# TUI Ebook Reader

A cross-platform command-line ebook reader with a built-in music player.

Runs on Windows, Linux and macOS. The default build is a pure terminal UI;
`-DENABLE_GUI=ON` optionally adds a mobile-style graphical reader without any
GUI toolkit dependency.

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
- Lyrics and cover are also embedded into the MP3 itself (ID3v2.3 USLT + APIC),
  so they survive copying the file elsewhere
- Download reports which metadata failed (lyrics / cover) instead of silently skipping

### NetEase cloud music (built in, no Node required)

The NetEase source talks to `interface.music.163.com` directly over the `eapi`
protocol, implemented natively in C++ (AES-128-ECB + MD5 via OpenSSL). No Node
runtime, no extra process. Requires `-DENABLE_MUSIC=ON`.

- **QR-code login**: press `L` in the online page, open the shown URL on your phone.
- **Cookie import**: press `I` on the account page and paste a browser Cookie header,
  or `POST /api/netease/login/cookie` with `{"cookie": "..."}`.
  `tools/netease_edge_cookie.py` can extract and decrypt an existing Edge session.
- **Session storage**: `%APPDATA%\ebook_reader\netease_session.json` on Windows,
  `$XDG_CONFIG_HOME/ebook_reader/` on Unix (override with `EBOOK_READER_HOME`).
  The cookie is DPAPI-protected on Windows and survives rebuilding/reinstalling.
  A legacy exe-local `netease_session.json` is migrated automatically on first use.
- **My playlists** (`G`): browse, play and download any playlist you own or follow.
- **Toplists** (`T`): 65 official charts, no login needed.
- **Daily recommendations** (`R`): personalised, requires login.
- **Translated lyrics**: both original and Chinese translation are fetched.
- **Hi-res audio**: after login the client requests `lossless` and falls back through
  `exhigh` → `higher` → `standard` → 128k MP3 when the account lacks rights.

### Graphical reader (`-DENABLE_GUI=ON`)

An optional build flag adds a graphical app laid out like **Koodo Reader**: a
left library sidebar (all books / store / music / me, plus TOC, bookmarks and
library folders), a top toolbar with search, sort, refresh, theme and import,
and a wide book grid. On windows narrower than 900 px it falls back to the
original phone-shaped, Fanqie-style layout with four bottom tabs.

The reader has immersive scrolling
text, tap-to-reveal toolbars, a searchable / reversible chapter drawer, a chapter
viewer that opens one specific chapter's full text with previous / next chapter
navigation, chapter jump buttons and in-chapter progress, a "next chapter" prompt
at chapter end, three-speed auto-scroll reading, four fonts / line-spacing /
four themes / brightness / page-turn animation controls, bookmarks, progress sync
and full-text search. Long-pressing a paragraph offers copy, highlight, read-from-
here (text-to-speech) and bookmark. **Listen to a book** via the browser speech
engine with speed, voice, sleep timer and automatic next-chapter playback.

The **Bookstore** tab searches the local library and (optionally) the Fanqie and
so-novel sources: hot searches, search history, "continue reading" and
recommendations, per-book details (intro, chapter count, word count, progress),
"read now" and "add to shelf" (downloads into `downloads/`). Fanqie details show
the real cover, chapter / word count, readers and the first & latest chapter; the
download sheet lets you pick **TXT / EPUB / PDF**, optionally generate an
**audiobook** (MP3 / WAV) and restrict the **chapter range**, then follows the job
with a per-chapter progress bar and a cancel button before copying the produced
file into `downloads/`. The shelf's **Check updates** action scans downloaded
books for new chapters and can refresh them in one click. The **Me** tab shows
reading statistics (today / total / streak / per-book time), bookshelf sorting and
grid-list switch, hidden-book restore, and shortcuts to reader / library / TTS
settings. Chapter detection now rejects body sentences such as "第二章第1段，……".

A **Music** tab is included as well: a local library player (with Range/seek
support), online search, NetEase daily recommendations / playlists / toplists, QR
and Cookie login, a mini player, a full-screen player with synced (and translated)
lyrics, queue, shuffle/repeat modes and volume. Online features require building
with `-DENABLE_MUSIC=ON`; with GUI alone the tab still plays files from
`downloads/music` and shows a hint for the rest.

The page is a single HTML file (`web/reader.html`) embedded into the executable
at compile time and served by the program's own loopback HTTP server — no Qt,
WebView2, Electron or runtime web assets are required. `ENABLE_GUI=ON`
automatically enables `ENABLE_API`, because the UI consumes those JSON endpoints.

```bash
# Full experience: reader + music (needs libcurl + OpenSSL)
cmake -S . -B build_all -DENABLE_GUI=ON -DENABLE_MUSIC=ON
cmake --build build_all
./build_all/ebook_reader.exe --gui          # opens a standalone app window

# Or reader only (Music tab falls back to the local library)
cmake -S . -B build_gui -DENABLE_GUI=ON
cmake --build build_gui
./build_gui/ebook_reader.exe --serve        # then open http://127.0.0.1:8686/ yourself
```

On Windows the window uses Edge (or Chrome) in `--app` mode with an isolated
profile at `%APPDATA%\ebook_reader\gui_profile`; other platforms fall back to the
default browser. Closing the window stops the server. With the flag off (the
default) the CLI, TUI and `--serve` behaviour are completely unchanged.

### HTTP API test mode (`-DENABLE_API=ON`)

Build with `-DENABLE_API=ON` and run `ebook_reader --serve [--port 8686] [--host 127.0.0.1]`
to expose every feature over HTTP without starting the TUI. Intended for automated
testing; the server is single-threaded and binds to loopback by default.

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/health` | status + compiled-in capabilities |
| GET | `/api/library[?dir=&depth=]` | list books (each with `percent`, `lastLine`, `updatedAt` progress) |
| GET | `/api/library/detail?path=` | parse one book on demand: intro, word / chapter count, TOC preview and progress (does not change the open book) |
| GET | `/api/search?q=[&online=1]` | unified local-library search; `online=1` also embeds Fanqie / so-novel results |
| GET/POST | `/api/book/open` | open a book (`path`) |
| GET | `/api/book/info` | title, author, page count |
| GET | `/api/book/toc` | chapter list with `end`, `paragraphs`, `chars`, `minutes` per chapter |
| GET | `/api/book/chapter?index=N` | full text of one chapter; `line=N` selects the chapter containing that paragraph, `limit=` caps returned paragraphs |
| GET | `/api/book/content?page=N` | page of wrapped lines (or `line`/`count`; add `raw=1` for un-wrapped paragraphs) |
| GET | `/api/book/search?q=` | in-book search (`raw=1` returns original paragraph line numbers) |
| GET/POST | `/api/book/progress?path=` | read / save reading position (shared with TUI) |
| GET/POST | `/api/book/bookmarks?path=` | list / add / remove bookmarks |
| GET | `/api/fanqie/search?q=` | Fanqie search |
| POST | `/api/fanqie/download` | download a Fanqie book |
| GET | `/api/fanqie/status` | downloader version, save dir, current format / audiobook config |
| GET/POST | `/api/fanqie/config` | read / update output format (`txt`/`epub`/`pdf`), audiobook, bulk files |
| GET | `/api/fanqie/preview?bookId=` | cover, chapter count, word count, readers, first / last chapter |
| GET | `/api/fanqie/cover?bookId=` | JPEG cover proxy (cached 1 day) |
| POST | `/api/fanqie/job` | start an async download (`format`, `audiobook`, `audiobookFormat`, `rangeStart` / `rangeEnd`) |
| GET | `/api/fanqie/job?id=` | job snapshot: state, saved / total chapters, phase, pending choices |
| POST | `/api/fanqie/job/action?action=` | `cancel` a job, or answer its `format` / `bookname` prompt |
| POST | `/api/fanqie/import` | copy the produced file from the downloader into `downloads/` |
| GET | `/api/fanqie/updates[?start=1]` | scan downloaded books for new chapters |
| GET | `/api/fanqie/history?limit=` | download history recorded by the downloader |
| GET | `/api/fanqie/library[?path=&name=]` | browse the downloader save directory |
| GET | `/api/sonovel/search?q=` | so-novel aggregated search |
| POST | `/api/sonovel/download` | download via so-novel |
| GET | `/api/music/library[?dir=]` | local music library (available in GUI-only builds too) |
| GET | `/api/music/file?path=` | stream a local audio file with `Range` / 206 seek support; restricted to `downloads/music` |
| GET | `/api/online/sources` | available online sources |
| GET | `/api/online/search?q=&source=` | online music search |
| GET | `/api/online/resolve?id=&source=` | resolve a stream URL |
| POST | `/api/online/download?id=&source=&title=&artist=` | download a song (query string or JSON body) |
| GET | `/api/netease/account` | current NetEase login state |
| POST | `/api/netease/login/qr` | start QR login, returns `key` + `url` + inline `svg` |
| GET | `/api/netease/login/poll?key=` | poll QR state (801/802/800/803) |
| POST | `/api/netease/login/cookie` | import a browser Cookie header (`{"cookie":"..."}`) |
| POST | `/api/netease/logout` | clear NetEase session |
| GET | `/api/netease/playlists` | my playlists (login required) |
| GET | `/api/netease/playlist/songs?id=` | songs in a playlist |
| GET | `/api/netease/toplists` | 65 official charts |
| GET | `/api/netease/recommend` | daily recommendations (login required) |
| GET | `/api/netease/lyric?id=` | LRC + Chinese translation |
| GET/POST | `/api/config` | read / update settings |
| POST | `/api/config/dir/add`, `/api/config/dir/remove` | manage library folders |

Endpoints for Fanqie / so-novel / music are only registered when the corresponding
features were compiled in.

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
| `L` | NetEase account (QR login / logout) |
| `G` | My playlists (requires login) |
| `T` | Toplists (no login needed) |
| `R` | Daily recommendations (requires login) |

In a playlist or toplist, `Enter` plays the song and `d` downloads it.

## Runtime files

Created next to the executable:

| Path | Content |
|---|---|
| `downloads/` | Downloaded novels (txt) |
| `downloads/music/` | Downloaded music (mp3 + lrc + jpg) |
| `netease_session.json` | NetEase login cookie — now under the user data dir (see above), DPAPI-protected; git-ignored |
| `cache/` | Online playback cache |
| `ebook_reader.config.json` | Library settings |
| `music_store.json` | Playlists, favorites, history |

`downloads/` and `downloads/music/` are indexed automatically.

## License

GPL-3.0. The bundled Tomato-Novel-Downloader is a separate GPL-3.0 project.
