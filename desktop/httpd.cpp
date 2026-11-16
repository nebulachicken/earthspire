#include "httpd.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t kBadSock = INVALID_SOCKET;
static void CloseSock(sock_t s) { closesocket(s); }
#else
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int sock_t;
static const sock_t kBadSock = -1;
static void CloseSock(sock_t s) { close(s); }
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

namespace sv {
namespace {

constexpr size_t kMaxHeader = 64 * 1024;
constexpr size_t kMaxBody = 1024 * 1024;

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() + 0 && HexVal(s[i + 1]) >= 0 && HexVal(s[i + 2]) >= 0) {
            out += static_cast<char>(HexVal(s[i + 1]) * 16 + HexVal(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

const char* Reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

bool SendAll(sock_t s, const char* data, size_t len) {
    while (len > 0) {
        const int n = static_cast<int>(send(s, data, static_cast<int>(std::min<size_t>(len, 1 << 20)), MSG_NOSIGNAL));
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

void SetRecvTimeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

// Read one request. Returns false on malformed/oversized/timeout.
bool ReadRequest(sock_t s, HttpRequest& rq) {
    std::string buf;
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = static_cast<int>(recv(s, chunk, sizeof chunk, 0));
        if (n <= 0) return false;
        buf.append(chunk, static_cast<size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd == std::string::npos && buf.size() > kMaxHeader) return false;
    }

    const std::string head = buf.substr(0, headerEnd);
    size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    rq.method = requestLine.substr(0, sp1);
    const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    rq.path = target.substr(0, q);
    rq.query = q == std::string::npos ? std::string() : target.substr(q + 1);

    while (lineEnd != std::string::npos && lineEnd < head.size()) {
        const size_t start = lineEnd + 2;
        lineEnd = head.find("\r\n", start);
        const std::string line = head.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        rq.headers[name] = Trim(line.substr(colon + 1));
    }

    size_t want = 0;
    auto it = rq.headers.find("content-length");
    if (it != rq.headers.end()) {
        want = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
        if (want > kMaxBody) return false;
    }
    rq.body = buf.substr(headerEnd + 4);
    while (rq.body.size() < want) {
        const int n = static_cast<int>(recv(s, chunk, sizeof chunk, 0));
        if (n <= 0) return false;
        rq.body.append(chunk, static_cast<size_t>(n));
    }
    rq.body.resize(want);
    return true;
}

void WriteResponse(sock_t s, const HttpResponse& rs) {
    std::string head = "HTTP/1.1 " + std::to_string(rs.status) + " " + Reason(rs.status) + "\r\n";
    head += "Content-Type: " + rs.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(rs.body.size()) + "\r\n";
    head += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    for (const auto& h : rs.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    if (SendAll(s, head.data(), head.size())) SendAll(s, rs.body.data(), rs.body.size());
}

}  // namespace

std::map<std::string, std::string> ParseForm(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t amp = text.find('&', pos);
        if (amp == std::string::npos) amp = text.size();
        const std::string pair = text.substr(pos, amp - pos);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq == std::string::npos) out[UrlDecode(pair)] = "";
            else out[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
        }
        pos = amp + 1;
    }
    return out;
}

HttpServer::~HttpServer() {
    if (listenSock_ != -1) CloseSock(static_cast<sock_t>(listenSock_));
#ifdef _WIN32
    if (started_) WSACleanup();
#endif
}

bool HttpServer::Listen(int port, std::string& error) {
#ifdef _WIN32
    if (!started_) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "WSAStartup failed"; return false; }
        started_ = true;
    }
#endif
    if (listenSock_ != -1) { CloseSock(static_cast<sock_t>(listenSock_)); listenSock_ = -1; }
    const sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSock) { error = "could not create socket"; return false; }
#ifndef _WIN32
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        error = "port " + std::to_string(port) + " is in use";
        CloseSock(s);
        return false;
    }
    if (listen(s, 8) != 0) { error = "listen failed"; CloseSock(s); return false; }
    listenSock_ = static_cast<std::intptr_t>(s);
    return true;
}

void HttpServer::Serve(const HttpHandler& handler) {
    const sock_t ls = static_cast<sock_t>(listenSock_);
    while (!stop_) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        const int n = select(static_cast<int>(ls) + 1, &rf, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        const sock_t c = accept(ls, nullptr, nullptr);
        if (c == kBadSock) continue;
        SetRecvTimeout(c, 5000);
        HttpRequest rq;
        if (ReadRequest(c, rq)) {
            HttpResponse rs;
            try {
                rs = handler(rq);
            } catch (const std::exception& e) {
                rs.status = 500;
                rs.body = std::string("internal error: ") + e.what();
            }
            WriteResponse(c, rs);
        } else {
            HttpResponse bad;
            bad.status = 400;
            bad.body = "bad request";
            WriteResponse(c, bad);
        }
        CloseSock(c);
    }
}

}  // namespace sv
