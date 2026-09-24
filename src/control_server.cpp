#include "control_server.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using ssize_t = long long;
static void closeSocket(socket_t s) { closesocket(s); }
static bool validSocket(socket_t s) { return s != INVALID_SOCKET; }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
using socket_t = int;
static void closeSocket(socket_t s) { close(s); }
static bool validSocket(socket_t s) { return s >= 0; }
#endif

#include <cstdio>
#include <sstream>

#include "url_util.hpp"

namespace curf {

ControlServer::ControlServer(int port, Handler handler) : port_(port), handler_(std::move(handler)) {}

ControlServer::~ControlServer() {
    if (listenFd_ >= 0) closeSocket(static_cast<socket_t>(listenFd_));
    if (thread_.joinable()) thread_.detach();
}

bool ControlServer::start() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (!validSocket(s)) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 16) != 0) {
        perror("curf: control server");
        closeSocket(s);
        return false;
    }
    listenFd_ = static_cast<long long>(s);
    thread_ = std::thread(&ControlServer::acceptLoop, this);
    return true;
}

void ControlServer::acceptLoop() {
    while (listenFd_ >= 0) {
        socket_t fd = accept(static_cast<socket_t>(listenFd_), nullptr, nullptr);
        if (!validSocket(fd)) continue;
        std::thread(&ControlServer::serve, this, static_cast<long long>(fd)).detach();
    }
}

static const char* statusText(int s) {
    switch (s) {
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 504: return "Gateway Timeout";
        default: return "OK";
    }
}

void ControlServer::serve(long long rawFd) {
    socket_t fd = static_cast<socket_t>(rawFd);
    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || data.size() > (1 << 20)) { closeSocket(fd); return; }
        data.append(buf, static_cast<size_t>(n));
        headerEnd = data.find("\r\n\r\n");
    }

    Request req;
    std::istringstream head(data.substr(0, headerEnd));
    std::string target, version, line;
    head >> req.method >> target >> version;
    std::getline(head, line);
    size_t contentLength = 0;
    while (std::getline(head, line)) {
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string key = line.substr(0, c);
        for (auto& ch : key) ch = static_cast<char>(tolower(ch));
        if (key == "content-length") contentLength = std::stoul(trim(line.substr(c + 1)));
    }
    req.body = data.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.body.append(buf, static_cast<size_t>(n));
    }
    size_t qm = target.find('?');
    req.path = target.substr(0, qm);
    if (qm != std::string::npos) req.query = parseQuery(target.substr(qm + 1));

    Response res;
    try {
        res = handler_(req);
    } catch (const std::exception& e) {
        res = {500, std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}"};
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << res.status << ' ' << statusText(res.status) << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "Content-Length: " << res.body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << res.body;
    std::string s = out.str();
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = send(fd, s.data() + sent, static_cast<int>(s.size() - sent), 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    closeSocket(fd);
}

}  // namespace curf
