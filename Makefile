CXX      ?= clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wno-deprecated-declarations
OBJCFLAGS := -fobjc-arc
FRAMEWORKS := -framework Cocoa -framework WebKit -framework Security
SRC := src/main.mm src/control_server.cpp src/url_util.cpp
HDR := $(wildcard src/*.hpp)

all: curf.app

curf: $(SRC) $(HDR) Info.plist
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) $(SRC) $(FRAMEWORKS) \
		-Wl,-sectcreate,__TEXT,__info_plist,Info.plist -o $@

curf.app: curf assets/curf.icns
	mkdir -p curf.app/Contents/MacOS curf.app/Contents/Resources
	cp Info.plist curf.app/Contents/Info.plist
	cp assets/curf.icns curf.app/Contents/Resources/curf.icns
	cp curf curf.app/Contents/MacOS/curf

run: curf.app
	./curf.app/Contents/MacOS/curf https://example.com

clean:
	rm -rf curf curf.app

.PHONY: all run clean

skill:
	cp curfctl.py skills/curf-browser/scripts/curfctl.py
	for d in ~/.cursor/skills ~/.claude/skills; do mkdir -p $$d && rm -rf $$d/curf-browser && cp -R skills/curf-browser $$d/; done

.PHONY: skill

# Re-embed Cluso Inspector (minified) from its own checkout, then `make` rebuilds.
CLUSO_SRC ?= ../cluso-inspector/cluso-inspector.js
cluso:
	python3 scripts/embed-cluso.py $(CLUSO_SRC)

.PHONY: cluso
