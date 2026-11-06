// =============================================================================
//  http_server.cpp —— 极简 HTTP 服务器实现（Winsock2）
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "http_server.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common.h"

namespace pc {
namespace {

constexpr size_t kMaxRequestBytes = 200u * 1024u * 1024u;  // 单次请求上限 200MB

// 状态码对应的原因短语
const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

// 把路径中的 %XX 转义还原成字节
std::string urlDecode(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            auto hexValue = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexValue(input[i + 1]);
            int lo = hexValue(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (input[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

// 发送一段数据（处理部分发送与中断）
bool sendAll(SOCKET sock, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int n = ::send(sock, data + sent, static_cast<int>(length - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

std::string HttpRequest::header(const std::string& name) const {
    std::string key = toLowerAscii(name);
    auto it = headers.find(key);
    return it == headers.end() ? std::string() : it->second;
}

void HttpResponse::setJson(const std::string& json) {
    contentType = "application/json; charset=utf-8";
    body = json;
}

void HttpResponse::setText(const std::string& text) {
    contentType = "text/plain; charset=utf-8";
    body = text;
}

HttpServer::HttpServer() : listenSocket_(static_cast<uintptr_t>(INVALID_SOCKET)), port_(0), running_(false) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(const std::string& host, int port, std::string& error) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        error = "WSAStartup 失败";
        return false;
    }

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        error = "创建监听套接字失败";
        WSACleanup();
        return false;
    }

    int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        error = "端口 " + std::to_string(port) + " 绑定失败（可能已被占用）";
        ::closesocket(sock);
        WSACleanup();
        return false;
    }

    if (::listen(sock, SOMAXCONN) == SOCKET_ERROR) {
        error = "监听失败";
        ::closesocket(sock);
        WSACleanup();
        return false;
    }

    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    listenSocket_ = static_cast<uintptr_t>(sock);
    running_ = true;
    return true;
}

void HttpServer::run(const HttpHandler& handler) {
    if (listenSocket_ == static_cast<uintptr_t>(INVALID_SOCKET)) return;

    while (running_) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET client = ::accept(static_cast<SOCKET>(listenSocket_),
                                 reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }

        // 接收超时，避免慢连接长期占用
        DWORD timeoutMs = 30000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                     sizeof(timeoutMs));

        std::string buffer;
        std::string headerBlock;
        size_t headerEnd = std::string::npos;
        char chunk[16384];

        while (true) {
            int n = ::recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buffer.append(chunk, static_cast<size_t>(n));
            headerEnd = buffer.find("\r\n\r\n");
            if (headerEnd != std::string::npos) break;
            if (buffer.size() > 64u * 1024u) break;  // 请求头过大
        }

        HttpResponse response;
        if (headerEnd == std::string::npos) {
            response.status = 400;
            response.setText("Bad Request");
        } else {
            headerBlock = buffer.substr(0, headerEnd);
            HttpRequest request;

            // ---- 解析请求行 ----
            size_t lineEnd = headerBlock.find("\r\n");
            std::string requestLine = headerBlock.substr(0, lineEnd == std::string::npos ? headerBlock.size() : lineEnd);
            size_t sp1 = requestLine.find(' ');
            size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
            if (sp1 == std::string::npos) {
                response.status = 400;
                response.setText("Bad Request");
            } else {
                request.method = requestLine.substr(0, sp1);
                std::string target = sp2 == std::string::npos
                                         ? requestLine.substr(sp1 + 1)
                                         : requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
                size_t q = target.find('?');
                request.path = urlDecode(q == std::string::npos ? target : target.substr(0, q));
                request.query = q == std::string::npos ? std::string() : target.substr(q + 1);

                // ---- 解析请求头 ----
                size_t pos = lineEnd == std::string::npos ? headerBlock.size() : lineEnd + 2;
                while (pos < headerBlock.size()) {
                    size_t next = headerBlock.find("\r\n", pos);
                    std::string line = headerBlock.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = toLowerAscii(trim(line.substr(0, colon)));
                        std::string value = trim(line.substr(colon + 1));
                        request.headers[key] = value;
                    }
                    if (next == std::string::npos) break;
                    pos = next + 2;
                }

                // ---- 读取正文 ----
                size_t contentLength = 0;
                std::string cl = request.header("content-length");
                if (!cl.empty()) contentLength = static_cast<size_t>(std::strtoull(cl.c_str(), nullptr, 10));
                if (contentLength > kMaxRequestBytes) {
                    response.status = 413;
                    response.setText("Payload Too Large");
                } else {
                    if (toLowerAscii(request.header("expect")) == "100-continue") {
                        const char* cont = "HTTP/1.1 100 Continue\r\n\r\n";
                        sendAll(client, cont, std::strlen(cont));
                    }
                    size_t bodyStart = headerEnd + 4;
                    std::string body = buffer.size() > bodyStart ? buffer.substr(bodyStart) : std::string();
                    while (body.size() < contentLength) {
                        int n = ::recv(client, chunk, sizeof(chunk), 0);
                        if (n <= 0) break;
                        body.append(chunk, static_cast<size_t>(n));
                        if (body.size() > kMaxRequestBytes) break;
                    }
                    if (body.size() > contentLength) body.resize(contentLength);
                    request.body = body;

                    if (contentLength > 0 && request.body.size() < contentLength) {
                        response.status = 400;
                        response.setText("Bad Request: incomplete body");
                    }
                }

                if (response.status == 200 && !request.method.empty()) {
                    // multipart 上传解析
                    std::string ctype = request.header("content-type");
                    if (!ctype.empty() && toLowerAscii(ctype).find("multipart/form-data") != std::string::npos &&
                        !request.body.empty()) {
                        std::string parseError;
                        if (!parseMultipartBody(ctype, request.body, request.files, parseError)) {
                            response.status = 400;
                            response.setJson("{\"ok\":false,\"error\":\"" + jsonEscape(parseError) + "\"}");
                        }
                    }
                    if (response.status == 200) {
                        try {
                            handler(request, response);
                        } catch (const std::exception& ex) {
                            response.status = 500;
                            response.setJson(std::string("{\"ok\":false,\"error\":\"") + jsonEscape(ex.what()) + "\"}");
                        } catch (...) {
                            response.status = 500;
                            response.setJson("{\"ok\":false,\"error\":\"服务器内部错误\"}");
                        }
                    }
                }
            }
        }

        // ---- 写响应 ----
        std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + reasonPhrase(response.status) + "\r\n";
        head += "Content-Type: " + response.contentType + "\r\n";
        head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        head += "Connection: close\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "Access-Control-Allow-Origin: *\r\n";
        head += "\r\n";
        sendAll(client, head.data(), head.size());
        if (!response.body.empty()) sendAll(client, response.body.data(), response.body.size());

        ::shutdown(client, SD_SEND);
        ::closesocket(client);
    }
}

void HttpServer::stop() {
    running_ = false;
    if (listenSocket_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        ::closesocket(static_cast<SOCKET>(listenSocket_));
        listenSocket_ = static_cast<uintptr_t>(INVALID_SOCKET);
        WSACleanup();
    }
}

bool parseMultipartBody(const std::string& contentType, const std::string& body,
                        std::vector<UploadedFile>& files, std::string& error) {
    files.clear();
    size_t boundaryPos = toLowerAscii(contentType).find("boundary=");
    if (boundaryPos == std::string::npos) {
        error = "multipart 请求缺少 boundary";
        return false;
    }
    std::string boundary = contentType.substr(boundaryPos + 9);
    if (!boundary.empty() && boundary.front() == '"') {
        size_t endQuote = boundary.find('"', 1);
        boundary = endQuote == std::string::npos ? boundary.substr(1) : boundary.substr(1, endQuote - 1);
    }
    size_t semi = boundary.find(';');
    if (semi != std::string::npos) boundary = boundary.substr(0, semi);
    boundary = trim(boundary);
    if (boundary.empty()) {
        error = "multipart 请求的 boundary 为空";
        return false;
    }

    const std::string delimiter = "--" + boundary;
    size_t pos = body.find(delimiter);
    if (pos == std::string::npos) {
        error = "multipart 请求体格式错误";
        return false;
    }
    pos += delimiter.size();

    while (pos < body.size()) {
        // 分隔符后应是 "--"（结束）或换行（下一段）
        if (pos + 2 <= body.size() && body.compare(pos, 2, "--") == 0) break;
        if (pos + 2 <= body.size() && body.compare(pos, 2, "\r\n") == 0) pos += 2;

        size_t headerEnd = body.find("\r\n\r\n", pos);
        if (headerEnd == std::string::npos) break;
        std::string headerText = body.substr(pos, headerEnd - pos);

        UploadedFile file;
        // 解析 Content-Disposition 中的 name / filename
        size_t namePos = headerText.find("name=\"");
        if (namePos != std::string::npos) {
            size_t begin = namePos + 6;
            size_t end = headerText.find('"', begin);
            if (end != std::string::npos) file.field = headerText.substr(begin, end - begin);
        }
        size_t filePos = headerText.find("filename=\"");
        if (filePos != std::string::npos) {
            size_t begin = filePos + 10;
            size_t end = headerText.find('"', begin);
            if (end != std::string::npos) file.filename = headerText.substr(begin, end - begin);
        }
        size_t typePos = toLowerAscii(headerText).find("content-type:");
        if (typePos != std::string::npos) {
            size_t begin = typePos + 13;
            size_t end = headerText.find("\r\n", begin);
            file.contentType = trim(headerText.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        }

        size_t dataBegin = headerEnd + 4;
        size_t nextDelimiter = body.find("\r\n" + delimiter, dataBegin);
        if (nextDelimiter == std::string::npos) {
            file.data = body.substr(dataBegin);
            files.push_back(file);
            break;
        }
        file.data = body.substr(dataBegin, nextDelimiter - dataBegin);
        files.push_back(file);
        pos = nextDelimiter + 2 + delimiter.size();
    }
    return true;
}

std::string contentTypeForPath(const std::string& path) {
    std::string lower = toLowerAscii(path);
    if (endsWithNoCase(lower, ".html") || endsWithNoCase(lower, ".htm")) return "text/html; charset=utf-8";
    if (endsWithNoCase(lower, ".css")) return "text/css; charset=utf-8";
    if (endsWithNoCase(lower, ".js")) return "application/javascript; charset=utf-8";
    if (endsWithNoCase(lower, ".json")) return "application/json; charset=utf-8";
    if (endsWithNoCase(lower, ".svg")) return "image/svg+xml";
    if (endsWithNoCase(lower, ".png")) return "image/png";
    if (endsWithNoCase(lower, ".jpg") || endsWithNoCase(lower, ".jpeg")) return "image/jpeg";
    if (endsWithNoCase(lower, ".ico")) return "image/x-icon";
    if (endsWithNoCase(lower, ".woff2")) return "font/woff2";
    if (endsWithNoCase(lower, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool serveStaticFile(const std::string& rootDir, const std::string& urlPath, HttpResponse& response,
                     std::string& error) {
    std::string relative = urlPath;
    if (relative == "/" || relative.empty()) relative = "/index.html";
    // 目录穿越防护：拒绝 ".." 片段
    if (relative.find("..") != std::string::npos) {
        response.status = 404;
        response.setText("Not Found");
        return false;
    }
    if (relative[0] == '/') relative = relative.substr(1);

    std::string fullPath = joinPath(rootDir, relative);
    std::string bytes;
    if (!readFileBytes(fullPath, bytes, error)) {
        response.status = 404;
        response.setText("Not Found: " + urlPath);
        return false;
    }
    response.status = 200;
    response.contentType = contentTypeForPath(fullPath);
    response.body = std::move(bytes);
    return true;
}

} // namespace pc
