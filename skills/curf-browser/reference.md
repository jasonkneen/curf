# curf API reference

Base URL `http://127.0.0.1:9333`. All responses are JSON; failures have `"ok": false` and `"error"`.

| Method | Endpoint | Body | Returns |
|---|---|---|---|
| GET | `/state` | | `url, title, loading, progress, secure, canGoBack, canGoForward, picking` |
| GET | `/navigate?url=<input>&wait=1` | or input as body | state after load |
| GET | `/back`, `/forward`, `/reload` (`wait=1`) | | state after load |
| POST | `/eval` | JavaScript | `{ok, value}` |
| GET | `/html`, `/text`, `/title` | | `{ok, value}` |
| GET | `/links` | | `value: [{text, href}]` |
| GET | `/extract?selector=<css>&attr=<name>` | | `value: [{selector, tag, text, html, rect, href?, value?, attr?}]` |
| GET | `/click?selector=<css>` | | described element |
| POST | `/type?selector=<css>&submit=0\|1` | text | described element |
| GET | `/wait?selector=<css>&timeout=30` | | described element once present |
| GET | `/pick?on=1\|0` | | `{picking}` |
| POST | `/annotate?selector=<css>` | note | `{annotation}` |
| GET | `/annotations` | | `{file, annotations: [...]}` |
| GET | `/changes?since=<seq>&type=<type>` | | `{last, changes: [...], file}` |
| GET | `/screenshot?path=<file.png>` | | `{path}` (default `~/.curf/screenshot.png`) |

`dom` change items: `{kind: childList|attributes|characterData, target, added, removed, addedText?, attr?, old?, value?}`; each batch holds up to 50 items plus `dropped` for overflow.

## Python

```python
import sys; sys.path.insert(0, "scripts")
from curfctl import Curf

c = Curf()                      # Curf(port=9444) for another instance
c.launch("news.ycombinator.com")
rows = c.extract(".titleline > a")
top = [(r["text"], r["href"]) for r in rows[:10]]

mark = c.changes()["last"]
c.click(".morelink")
c.wait(".titleline")
new = c.changes(since=mark, type="dom")["changes"]

title = c.eval("document.title")    # raises RuntimeError on JS errors
```

Multiple instances: run each with `--port N` (`launch` passes the client's port).
