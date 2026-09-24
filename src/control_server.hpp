#pragma once
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace curf {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::string body;
};

struct Response {
    int status = 200;
    std::string body;  // JSON
};

using Handler = std::function<Response(const Request&)>;

// Minimal HTTP/1.1 server bound to 127.0.0.1 used to drive the browser from scripts.
// Each connection is served on its own thread so long waits never block other clients.
class ControlServer {
public:
    ControlServer(int port, Handler handler);
    ~ControlServer();
    bool start();
    int port() const { return port_; }

private:
    void acceptLoop();
    void serve(long long fd);

    int port_;
    long long listenFd_ = -1;
    Handler handler_;
    std::thread thread_;
};

}  // namespace curf
