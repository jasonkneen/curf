#!/usr/bin/env python3
"""Drive the curf browser from scripts or the command line.

  ./curfctl.py navigate "rust lang"          # search Google
  ./curfctl.py navigate example.com
  ./curfctl.py eval "document.title"
  ./curfctl.py extract "h1"
  ./curfctl.py click "a"
  ./curfctl.py type "input[name=q]" "hello" --submit
  ./curfctl.py annotate "h1" "headline is wrong"
  ./curfctl.py changes --since 0 --type dom
  ./curfctl.py annotations
  ./curfctl.py screenshot /tmp/page.png

Or import it:  from curfctl import Curf; c = Curf(); c.navigate("example.com")
"""
import json
import sys
import urllib.parse
import urllib.request


class Curf:
    def __init__(self, host="127.0.0.1", port=9333, timeout=30):
        self.base = f"http://{host}:{port}"
        self.timeout = timeout

    def call(self, endpoint, body=None, **query):
        query = {k: v for k, v in query.items() if v is not None}
        url = self.base + endpoint + ("?" + urllib.parse.urlencode(query) if query else "")
        data = body.encode() if isinstance(body, str) else body
        req = urllib.request.Request(url, data=data, method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout + 5) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            return json.load(e)

    def state(self): return self.call("/state")
    def navigate(self, url, wait=True): return self.call("/navigate", url=url, wait=int(wait))
    def back(self): return self.call("/back")
    def forward(self): return self.call("/forward")
    def reload(self): return self.call("/reload")
    def eval(self, js):
        r = self.call("/eval", js)
        if not r.get("ok"):
            raise RuntimeError(r.get("error"))
        return r.get("value")
    def html(self): return self.call("/html").get("value")
    def text(self): return self.call("/text").get("value")
    def links(self): return self.call("/links").get("value")
    def extract(self, selector, attr=None): return self.call("/extract", selector=selector, attr=attr).get("value")
    def click(self, selector): return self.call("/click", selector=selector)
    def type(self, selector, text, submit=False): return self.call("/type", text, selector=selector, submit=int(submit))
    def wait(self, selector, timeout=30): return self.call("/wait", selector=selector, timeout=timeout)
    def pick(self, on=True): return self.call("/pick", on=int(on))
    def annotate(self, selector, note): return self.call("/annotate", note, selector=selector)
    def annotations(self): return self.call("/annotations").get("annotations", [])
    def changes(self, since=0, type=None): return self.call("/changes", since=since, type=type)
    def screenshot(self, path=None): return self.call("/screenshot", path=path)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    c, cmd, args = Curf(), argv[1], argv[2:]
    flags = {a for a in args if a.startswith("--")}
    pos = [a for a in args if not a.startswith("--")]

    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else default

    if cmd == "changes":
        out = c.changes(since=opt("--since", 0), type=opt("--type"))
    elif cmd == "type":
        out = c.type(pos[0], pos[1], submit="--submit" in flags)
    elif hasattr(c, cmd):
        out = getattr(c, cmd)(*pos)
    else:
        out = c.call("/" + cmd)
    print(json.dumps(out, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
