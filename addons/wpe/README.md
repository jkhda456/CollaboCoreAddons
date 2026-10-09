# wpe add-on

A web browser for agents in the sandbox: **WPE WebKit 2.54.1** built for the guest (WebAssembly
Linux), shown on the guest's display (`gui`, [tools/gui](../../tools/gui)) and driven from the
command line by `web`. Pages run as in a desktop browser — HTML, CSS, JavaScript, web fonts,
cookies and storage, forms, redirects, HTTPS — and an agent reads them as text with every link,
button and field numbered, acts on them with real pointer and key events, and looks at them as
screenshots.

```
$ web open news.ycombinator.com
https://news.ycombinator.com/
Hacker News

$ web read | head
Hacker News
https://news.ycombinator.com/

 [1](link) <https://news.ycombinator.com/> [2]Hacker News <https://news.ycombinator.com/news> [3]new ...
...
$ web click 'new'                    # by text, [N] number or CSS selector; waits for the new page
$ web type 'webkit' 'input[name=q]'; web key Enter
$ web screenshot                     # /tmp/web.png (gui screenshot web)
```

| In the guest | |
|---|---|
| `/usr/bin/web` | the command agents use (C, 66 KB; `web help`) |
| `/usr/bin/wpe-browser` | the browser: WebKit's UI, web and network processes in one static program |
| `/usr/libexec/wpe-webkit-2.0/WPEWebProcess`, `WPENetworkProcess` | links to it, where WebKit starts its processes |
| `/etc/fonts`, `/usr/share/fonts` | fontconfig, DejaVu (Latin, Greek, Cyrillic), Nanum Gothic (Korean, Hanja, kana) |
| `/usr/share/mime/globs2` | file types by name for `file://` pages and downloads (`share/mime`, the web's common types) |

## Turning it on

```json
"addons": {"wpe": {}}
```

or `--addon-dir addons/wpe/out --addon wpe` on the engine's command line. It needs the tools image
(the display; on by default) to show pages and take screenshots; without it the browser still
starts (headless) and `web read`, `web eval` and the actions work. The sandbox's network policy
applies to the browser as to everything in the guest.

From the host, the Dart package wraps `web` as LLM tools: `WebTools` (`web_open`, `web_read`,
`web_act`, `web_eval`, `web_screenshot`, `web_info`), next to `SandboxTools` and `GuiTools`.

## Commands

`web [-n NAME] COMMAND`: each command talks to the browser named NAME (`web` by default), and
starts it first if it is not running. Several names are several browsers, each with its own
cookies and storage (`~/.local/share/collabo-web/NAME`).

| Command | |
|---|---|
| `open URL [TIMEOUT]` | load a page and wait for it (seconds, 60); a bare host gets `https://`, words become a search |
| `back`, `forward`, `reload` | history, waiting for the page |
| `info` | address, title, loading, history, size, the last error or dialog |
| `wait [load \| text STR \| selector CSS \| gone CSS] [TIMEOUT]` | until the page is so |
| `read [MAX] [--all]` | the page as text in reading order: headings `#`, lists `-`, links with their address, fields with their value; every visible link, button and field numbered `[N]` |
| `text`, `html [CSS]`, `links` | the page's text, HTML, links |
| `eval JS` | JavaScript in the page; the result as JSON |
| `screenshot [FILE] [--max-width N]` | PNG of the page (`gui screenshot`) |
| `click TARGET \| X Y` (`dblclick`, `rightclick`) | a TARGET is an `[N]` from `read`, a CSS selector, or a link's or button's text; scrolled into view, clicked where it is on top; a click that loads a page waits for it |
| `type TEXT [TARGET]` | types into TARGET (clicked first) or what has the focus: key events per character (any language) |
| `key KEY...` | `Enter`, `Tab`, `Escape`, `ArrowDown`, `Backspace`, `ctrl+a`, `shift+Tab`, … |
| `select TARGET VALUE`, `hover TARGET`, `scroll [down\|up\|left\|right [N] \| top \| bottom]` | |
| `list`, `resize WxH`, `close` | the browsers; the page's size; end one |

Exit status: 0 done, 1 failed, 2 usage, 3 timed out, 4 not found, 5 no browser. Dialogs
(`alert`, `confirm`, `prompt`) are accepted and their text kept for `info`; permission requests
(location, notifications, camera) are denied; what the page would open in a new window opens in
the same one; downloads go to `~/Downloads`. The browser says it is WebKit on desktop Linux
(`X11; Linux x86_64`: the guest's own `wasm` is a platform sites do not know); `WEB_USER_AGENT`
replaces that, and `WEB_BROWSER_ARGS` passes `--ephemeral`, `--ignore-tls-errors` or
`--size WxH` to a browser `web` starts.

The browser is also an ordinary program on the display: `gui list`, `gui view web`,
`gui click web X Y`, `gui logs web` (its console messages) all work.

## How it works

- **One program, three kinds of process.** WebKit keeps its process model: the UI process (the
  browser, `browser/browser.c`), the network process, a web process per site. The guest cannot
  load libraries, so `wpe-browser` is all of them, statically linked (`browser/main.cpp` picks by
  the name it is run as; the injected bundle is linked in too, patch 0008), and WebKit starts its
  processes with `posix_spawn` (GLib's, which needs no `fork`). The browser asks for WebKit's
  `DocumentBrowser` cache model: no cache of processes for sites left behind and nothing started
  ahead (patch 0009), the two pages before kept for `back` — at most three web processes.
- **Calls through pointers of another type.** C code built for GLib calls functions through
  pointers of other types all the time (a `gboolean` function as a `GChildWatchFunc`, an
  interface init without its second parameter, a `GDestroyNotify` as a `GClosureNotify`); native
  code gets away with it, WebAssembly traps. Three layers: the casts that can be fixed where they
  are, found with `tools/check-fpcasts.py` and `tools/fpcast-scan.py` (clang's
  `-Wcast-function-type-strict`, kept where the *wasm* signatures differ), are fixed (patches to
  GLib, libsoup, glib-networking, WebKit, WebKit's IPC generator); the places where GLib calls
  what a program registered (signal handlers, closures, weak and toggle notifies, `GTask`
  callbacks, `GSource` callbacks) call it through `ffi_wasm_call` of the add-on's libffi
  (`ffi/`), which reads the function's real type from the program's own table
  (`/proc/self/exe`) and calls it as that, leaving out what it does not take; and every GLib
  signal goes through the generic (libffi) marshaller.
- **Shared memory without shared memory.** Each guest process is a WebAssembly memory of its own:
  nothing can be mapped into two of them. A `SharedMemory` is a file in memory (`memfd_create` is
  emulated with an unlinked `/dev/shm` or `/tmp` file) whose mapping is a copy, written back when
  it is unmapped and when a handle is made for another process (patch 0003). That is right for
  everything WebKit shares once (IPC messages larger than 4 KB, images, resources); the frames of a
  page, which the web process draws into the same buffers again and again, are written to the file
  after each frame and read again by the UI process when it gets the frame.
- **Rendering on the CPU.** No GPU and no EGL: the web process makes no EGL display and renders
  without compositing (`NonCompositedFrameRenderer`: Skia on the CPU into shared-memory buffers,
  patch 0004).
- **The display.** A WPE platform backend of our own, `collabo` (patch 0005,
  `Source/WebKit/WPEPlatform/wpe/collabo`): each toplevel is a canvas of the display server,
  frames go there as their damaged rectangles, and the server's input comes back as WPE events —
  pointer, wheel, keys (X11 keysyms), the input method's composition and commit (Hangul with
  `gui type --ime`), focus, hover, cursors, the clipboard. A frame is rendered when the server
  says it is on the canvas, which paces the web process to what the display takes.
- **JavaScript.** JavaScriptCore with its C interpreter (CLoop; nothing can be compiled at run
  time), JSVALUE32_64, the system allocator. Its garbage collector scans the stack conservatively,
  but WebAssembly keeps a function's values in locals it cannot see, so a collection never starts
  inside an allocation: it is asked for there and runs from a timer of the run loop, with nothing
  but the run loop below it (patch 0002); only an allocation far past its budget without the run
  loop getting a turn collects on the spot.
- **Agents' input** is not a separate path: `web click` finds the element (`browser/target.js`),
  scrolls it into view and sends pointer events to the view as the display would; `type` and
  `key` send key events. WebKit's UI process queues key events (one at a time, each after the page
  took the one before) apart from pointer events, so a command waits until the page has seen its
  input (a script counts it at the window) before it answers or the next one acts: a click cannot
  overtake the keys typed before it.
- **Compiled once per computer.** The browser is 170 MB of WebAssembly; compiling it takes the host
  about 20 s. The engine keeps large compiled programs on disk (`addons/mod` 0012:
  `~/.cache/collabo-core/programs`), so that is paid once per computer and version of the
  browser, and later sandboxes start it in about a second.

What it takes (an aarch64 host, 6 cores; `tests/run.sh` and a few public sites):

| | |
|---|---|
| the browser's first start on a computer | ~20 s (compiling it), then ~1 s in every sandbox |
| a simple page | well under a second (`tests/run.sh`: 24 checks, ~30 s with the boot) |
| Hacker News, MDN, GitHub, X | 1–2 s; Korean Wikipedia, Naver, BBC, YouTube, Namuwiki 2–7 s; CNN ~25 s |
| the engine's memory | ~1 GB with the add-on booted, ~1.7 GB after a few sites (3 web processes) |

## Limits

- **Speed.** JavaScript is interpreted, in WebAssembly: script-heavy applications (editors, maps,
  large single-page apps) are slow, and some will time out.
- **No video or audio, no WebGL, no WebAssembly, no WebRTC**, no PDF viewer, no spell checking,
  no XSLT. Fonts: DejaVu and Nanum Gothic only (pages' web fonts load as usual). Sites behind a
  robot check that runs WebAssembly (AWS WAF: Amazon) stay on the check's empty page.
- **Pages that draw themselves later.** `open` answers when the document has loaded; a single-page
  application may still be filling itself in (Reddit: its splash for a few seconds more) —
  `web wait text …` or `web wait selector …` waits for what is wanted.
- **The clock.** Certificates are checked against the sandbox's clock (the host's). A host whose
  clock is days off fails on many HTTPS sites; the error says what time the sandbox thought it
  was. A stapled OCSP response outside its validity period is left out rather than failing the
  connection, as browsers do (glib-networking patch 0002).
- One page per browser (popups open in it); start more browsers with `-n NAME`.

Debugging: `WPE_BROWSER_DEBUG=1` (in the environment of the first `web` command, which starts the
browser) logs loads and waits to `gui logs web`; a crash of any of its processes is reported by
the engine on its stderr with the program's frames by name (`a guest program trapped`; an `abort`
traps too, so that it says where). `WPE_WASM_KEEP_NAMES=0 tools/browser.sh` builds without the
names (45 MB smaller).

## Building

```sh
./build.sh userspace python         # musl, compiler-rt; zlib, OpenSSL, sqlite for the guest
addons/wpe/build.sh                 # or python3 addons/build.py build wpe
```

The WebKit source is the release tarball unpacked at `../org/wpewebkit-2.54.1` (`WK_SRC`); it is
exported into `build/src/webkit` and never modified. The host needs clang-19 and lld-19, python3,
perl; the build makes its own ruby, gperf, meson, unifdef and a host GLib (`build/host`). A first
build takes hours on 6 cores and about 6 GB of `build/`.

| Stage | |
|---|---|
| `host` | the build's tools (`tools/host-tools.sh`) |
| `deps` | libc++, the compat library, libffi (`ffi/`), GLib, libsoup, glib-networking (OpenSSL), ICU, HarfBuzz, FreeType, fontconfig, libxml2, libpng/jpeg/webp, nghttp2, libpsl, brotli, libgcrypt, libepoxy, libxkbcommon for wasm32, static (`tools/deps.sh`, sources and hashes in `sources.lock`, patches in `patches/<name>/`) |
| `webkit` | WebKit with `patches/webkit/*` (`tools/webkit.sh`): CLoop, Skia, no JIT, no media, the collabo display |
| `browser` | `wpe-browser.wasm`, `web.wasm` (`tools/browser.sh`) |
| `cpio` | `out/wpe.cpio`, the fonts, `out/licenses` |

`tests/run.sh` boots the runtime with the add-on and runs `tests/guest.sh` against pages served
in the guest (`tests/site`): opening, reading, Korean text, typing, selecting, clicking,
following a link, history, keys, scrolling, a screenshot, a failed load, closing. The Dart
package's `web tools` test (`dart/collabo_core/test`) drives the same through `WebTools`, with a
runtime that has the add-on in `app/images/addons`.

## Patches

`patches/webkit/` (to the release; `tools/save-patches.sh` writes them from `build/src/webkit`):

| Patch | |
|---|---|
| `0001-build-for-wasm32` | WPE's libWPEWebKit as a static library on wasm32; double-conversion, SIMDe and Skia/skcms (no `musttail`) know wasm; `size_t` is `unsigned long` there (JSC's options, `RawHex`); xdgmime's globals that clash with GIO's copy renamed |
| `0002-jsc-collect-from-the-run-loop-on-wasm` | the heap asks for collections from allocations and runs them from a run-loop timer |
| `0003-shared-memory-between-processes-without-shared-memory` | `SharedMemory` as a memory file, written back when shared; `flushForSharing` / `reloadFromSharer` for the frames |
| `0004-cpu-rendering-without-egl` | no EGL library: no EGL display, no compositing, render on the CPU |
| `0005-wpe-platform-collabo-display` | the `collabo` WPE platform (display, toplevel, view, keymap, input method, clipboard) |
| `0006-callbacks-take-what-they-are-called-with` | interface init functions with the two parameters GLib passes; xdgmime's directory callback |
| `0007-ipc-complete-argument-types-before-taking-handler-addresses` | the IPC generator completes a message's argument types before the handler's address is taken (else clang gives the address no real signature and the call traps) |
| `0008-injected-bundle-linked-in` | the injected bundle's entry point when it is linked into the program |
| `0009-process-cache-and-prewarming-follow-the-cache-model` | the web process cache follows a cache model set after the start; no process started ahead below `PrimaryWebBrowser` |

The libraries': `glib/` 0001 (no libatomic on wasm32), 0002 (function pointer casts), 0003
(registered callbacks called as what they are: closures, notifies, `GTask`, signals through
libffi), 0004 (`GSource` callbacks likewise); `libsoup/` 0001 and `glib-networking/` 0001
(function pointer casts); `glib-networking/` 0002 (stale stapled OCSP responses left out);
`libgpg-error/` 0001 (the lock type of wasm32's musl: a lock of no size aborted every process
that used libgcrypt).

Outside the add-on, the browser relies on the engine and kernel changes of `addons/mod`: 0008
(futexes keyed by process: WebKit's processes are the same program), 0009 and 0010 (a trap in a
guest program — a failed `RELEASE_ASSERT`, a stack overflow, a call through a pointer of another
type — ends that program, not the machine), 0011 (`WebTools` in the Dart package) and 0012
(large programs compiled once per computer, and the compiler's memory given back).

## Licenses

WebKit: LGPL-2.1 and BSD (`out/licenses`); Skia: BSD; GLib, libsoup, glib-networking: LGPL-2.1;
the other libraries their own (copied to `out/licenses`); DejaVu fonts: Bitstream Vera license;
Nanum Gothic: SIL OFL 1.1. The add-on's own code (`browser/`, `compat/`, `ffi/`, `tools/`): as
collaboCore (GPL-3.0); the collabo WPE backend in patch 0005: BSD-2-Clause, as the WebKit files
around it, and the display's client library it compiles in (`tools/gui/client`): MIT.
