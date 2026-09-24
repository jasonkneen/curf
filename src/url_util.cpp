#include "url_util.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace curf {

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
                   std::isxdigit((unsigned char)s[i + 2])) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        std::string pair = q.substr(pos, amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos)
                m[urlDecode(pair)] = "";
            else
                m[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return m;
}

static bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
}

std::string resolveInput(const std::string& input) {
    std::string s = trim(input);
    if (s.empty()) return "";
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (const char* scheme : {"http://", "https://", "file://", "about:", "data:"})
        if (startsWith(lower, scheme)) return s;

    if (s.find(' ') == std::string::npos) {
        std::string host = lower.substr(0, lower.find_first_of("/?#"));
        static const std::regex localhost(R"(^localhost(:\d+)?$)");
        static const std::regex ipv4(R"(^(\d{1,3}\.){3}\d{1,3}(:\d+)?$)");
        static const std::regex domain(R"(^([a-z0-9]([a-z0-9-]*[a-z0-9])?\.)+[a-z]{2,63}(:\d+)?$)");
        if (std::regex_match(host, localhost) || std::regex_match(host, ipv4)) return "http://" + s;
        if (std::regex_match(host, domain)) return "https://" + s;
    }
    return "https://www.google.com/search?q=" + urlEncode(s);
}

}  // namespace curf
