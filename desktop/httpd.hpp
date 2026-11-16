// Minimal single-threaded HTTP/1.1 server for the local control panel.
// Listens on 127.0.0.1 only. No dependencies besides the OS socket API.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sv {

struct HttpRequest {
    std::string method;                              // "GET", "POST"
    std::string path;                                // "/api/status"
    std::string query;                               // text after '?', not decoded
    std::map<std::string, std::string> headers;      // lower-case names
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// Decode "a=1&b=hello%20world" (also '+' as space) into a map.
std::map<std::string, std::string> ParseForm(const std::string& text);

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool Listen(int port, std::string& error);       // 127.0.0.1:port
    void Serve(const HttpHandler& handler);          // blocks until Stop()
    void Stop() { stop_ = true; }

private:
    std::intptr_t listenSock_ = -1;
    std::atomic<bool> stop_{false};
    bool started_ = false;
};

}  // namespace sv
