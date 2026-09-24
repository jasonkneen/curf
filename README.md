# curf

A scriptable macOS web browser written in C++/Objective-C++ on top of the system WebKit engine.

## Build & run

There are two front ends over the same C++ core (`url_util`, `control_server`, `inject.js.hpp`):

| Build | Engine | Platforms | Size |
|---|---|---|---|
| CMake + Qt 6 (`src/qt/main.cpp`) | Chromium (Qt WebEngine) | Windows, Linux, macOS | ~200 MB deployed |
| Makefile (`src/main.mm`) | System WebKit | macOS only | ~250 KB |

### Cross-platform (Qt 6)

Requires Qt 6.5+ with the WebEngine module and CMake 3.21+.

```sh
# macOS:   brew install qtwebengine cmake
# Ubuntu:  sudo apt install qt6-webengine-dev qt6-base-dev cmake g++
# Windows: install Qt 6 (MSVC kit + "Qt WebEngine") with the Qt online installer, use a VS Developer prompt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # add -DCMAKE_PREFIX_PATH=<Qt dir> if Qt isn't found
cmake --build build --config Release
cmake --install build --prefix "$PWD/dist" --config Release   # absolute prefix required; bundles the Qt runtime next to the app
```

Run `build/curf` (Linux), `build\Release\curf.exe` (Windows), or `build/curf.app` (macOS).

### macOS native (WebKit)

```sh
make                                   # builds curf.app
./curf.app/Contents/MacOS/curf [--port 9333] [url-or-search]
```

## Features

- Toolbar with icon buttons: back, forward, reload/stop, security/site-info, element picker.
- Address bar: full domains (`example.com`, `localhost:3000`, IPs) open directly; anything else is searched on Google.
- Security icon: green lock (HTTPS), orange warning lock (HTTPS with mixed content), red open lock (HTTP). Click it for site info and the certificate chain.
- Element picker (⌘E or the cursor icon): hover to highlight, click to select, type a note, and it is saved to `~/.curf/annotations.jsonl`.
- Change log: navigations, DOM mutations, annotations, and scripted actions, kept in memory and written to `~/.curf/changes.jsonl`.
- Shortcuts: ⌘L address bar, ⌘R reload, ⌘[ / ⌘] back/forward, ⌘I site info.

## Scripting

An HTTP JSON API listens on `127.0.0.1:9333` (`GET /` lists endpoints). `curfctl.py` wraps it:

```sh
./curfctl.py navigate example.com
./curfctl.py extract "h1"
./curfctl.py eval "document.title"
./curfctl.py eval "await new Promise(r => setTimeout(r, 500)); return location.href"
./curfctl.py type "textarea[name=q]" "webkit" --submit
./curfctl.py annotate "h1" "headline copy needs review"
./curfctl.py changes --since 0 --type dom
./curfctl.py screenshot /tmp/page.png
```

```python
from curfctl import Curf
c = Curf()
c.navigate("news.ycombinator.com")
titles = c.extract(".titleline > a")
print(c.changes(since=0)["changes"])
```

When a script uses `await` together with multiple statements, end it with `return`.

## License

MIT, see [LICENSE.md](LICENSE.md).
