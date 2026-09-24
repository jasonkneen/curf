#pragma once

// Cluso Inspector: embedded, injected hidden into every main-frame document. The toolbar
// picker button shows/hides it; comments go to the relay on localhost (default :4747).
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "cluso.js.hpp"

namespace curf {

inline std::string jsString(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o + "\"";
}

// The relay token from ~/.cluso-inspector/config.json ({"token":"…"}), or "" if absent.
// Pages on non-localhost origins need it; localhost pages are trusted without it.
inline std::string clusoToken(const std::string& home) {
    std::ifstream f(home + "/.cluso-inspector/config.json");
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();
    auto k = j.find("\"token\"");
    if (k == std::string::npos) return "";
    auto a = j.find('"', j.find(':', k) + 1);
    auto b = a == std::string::npos ? a : j.find('"', a + 1);
    return b == std::string::npos ? "" : j.substr(a + 1, b - a - 1);
}

inline int clusoPort() {
    const char* p = std::getenv("CLUSO_INSPECTOR_PORT");
    int n = p ? std::atoi(p) : 0;
    return n > 0 ? n : 4747;
}

// Config + inspector + a hook that tells the host when the page hides the bar itself.
inline std::string clusoUserScript(const std::string& home) {
    const std::string tok = clusoToken(home);
    const std::string url = "http://localhost:" + std::to_string(clusoPort()) + "/annotations";
    std::string s = "window.CLUSO_INSPECTOR = Object.assign({ enabled: false, startCollapsed: true, transport: { type: 'fetch', url: " +
                    jsString(url) + ", headers: " +
                    (tok.empty() ? std::string("{}") : "{ Authorization: " + jsString("Bearer " + tok) + " }") +
                    " } }, window.CLUSO_INSPECTOR || {});\n";
    s.append(reinterpret_cast<const char*>(kClusoJs), kClusoJsLen);
    s += "\n;(function () { var i = window.ClusoInspector && ClusoInspector.instance; if (i && i.on) "
         "i.on('disable', function () { window.__curf && __curf.post({ type: 'cluso', on: false }); }); })();\n";
    return s;
}

}  // namespace curf
