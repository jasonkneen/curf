---
name: curf-browser
description: Drives the curf scriptable web browser through its local HTTP API — launch it, navigate or search, run JavaScript, extract elements, click and type, annotate elements, read the DOM change log, and take screenshots. Use when the user mentions curf, asks to browse, scrape, or extract data from a live web page in a visible browser, automate a page, watch what changes on a page, or read/save element annotations.
---

# curf browser

curf is a desktop browser with a JSON API on `http://127.0.0.1:9333`. Everything goes through `scripts/curfctl.py` (Python 3 stdlib only). Run it; don't reimplement it.

```bash
CTL=scripts/curfctl.py          # path relative to this skill
python3 $CTL launch https://example.com   # starts curf if needed, waits for load
```

`launch` finds the binary via `$CURF_BIN`, `PATH`, or a curf checkout (`$CURF_HOME`, else `~/Documents/GitHub/curf`). Use `CURF_PORT` for a non-default port. If it reports the binary is missing, build it: `cmake -S . -B build && cmake --build build` (Qt 6 WebEngine) or `make` (macOS WebKit).

## Workflow

1. `launch [url]` → confirm `"ok": true` and the expected `url`.
2. Navigate: `navigate <input>`. Full domains open directly; anything else becomes a Google search. Navigation waits for load and returns state (`url`, `title`, `secure`).
3. Read the page: prefer `extract "<css>"` (returns `selector`, `tag`, `text`, `html`, `rect`, `href`, `value` per match) over `html`. Use `text` for readable content, `links` for anchors.
4. Act: `click "<css>"`, `type "<css>" "text" --submit`, then `wait "<css>"` for the result to appear.
5. Anything else: `eval "<js>"`.
6. Verify with `state` or `screenshot /tmp/x.png` (read the image to inspect it).

## Tracking changes

Every navigation, DOM mutation (batched ~400 ms), scripted click/type, and annotation gets a monotonically increasing `seq`. To see what an action changed:

```bash
last=$(python3 $CTL changes | python3 -c 'import json,sys; print(json.load(sys.stdin)["last"])')
python3 $CTL click "button.load-more"
sleep 1
python3 $CTL changes --since $last --type dom
```

Types: `navigation_started`, `navigation_finished`, `navigation_failed`, `dom`, `element_selected`, `annotation`, `script_click`, `script_type`, `certificate_error` (Qt build). The full log persists in `~/.curf/changes.jsonl`.

## Annotations

- The user picks elements with the cursor-arrow toolbar icon (⌘/Ctrl+E) and types a note; notes are appended to `~/.curf/annotations.jsonl`.
- `annotations` lists them (with `selector`, `url`, `text`, `html`, `note`, `time`). Use the saved `selector` with `extract`/`click` to act on what the user pointed at.
- `annotate "<css>" "note"` saves one programmatically; `pick` / `pick 0` toggles the picker for the user.

## Gotchas

- `eval` returns the last expression's value. With `await` plus multiple statements, end with `return`: `eval "await new Promise(r=>setTimeout(r,500)); return document.title"`.
- Values must be JSON-serialisable — return `el.outerHTML` or `__curf.describe(el)`, not DOM nodes.
- Rapid automated Google searches trigger Google's "unusual traffic" page (`/sorry/` in the URL). Navigate to the target site directly when you know it.
- Selectors from `extract`/annotations are positional (`:nth-of-type`) unless the element has an id; re-extract after big DOM changes.
- Most calls accept `timeout` (seconds, default 30). A `"error": "timeout"` means the page script never finished, not that curf died — check `state`.

## Reference

Full endpoint list and Python usage: [reference.md](reference.md).
