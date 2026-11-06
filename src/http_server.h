// =============================================================================
//  http_server.h —— 极简 HTTP/1.1 服务器（Winsock）
//
//  只实现本项目需要的功能：
//    * 解析请求行 / 请求头 / Content-Length 正文；
//    * 解析 multipart/form-data 上传（前端一次性上传多篇文档）；
//    * 静态文件响应 + JSON 响应。
//  不使用第三方库，便于在 Windows + MinGW 环境下直接编译。
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pc {

// 上传的文件片段
struct UploadedFile {
    std::string field;        // 表单字段名
    std::string filename;     // 客户端文件名（UTF-8）
    std::string contentType;  // MIME 类型
    std::string data;         // 文件字节
};

// 一次 HTTP 请求的解析结果
struct HttpRequest {
    std::string method;
    std::string path;   // 不含查询串
    std::string query;  // 查询串
    std::map<std::string, std::string> headers;  // 键为小写
    std::string body;
    std::vector<UploadedFile> files;  // multipart 解析结果

    // 取请求头（键大小写不敏感）
    std::string header(const std::string& name) const;
};

// 响应
struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;

    void setJson(const std::string& json);
    void setText(const std::string& text);
};

using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// 极简 HTTP 服务器：单线程 accept + 逐连接处理
class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // 绑定端口并开始监听；port 传 0 表示由系统分配（可通过 port() 查询实际端口）
    bool start(const std::string& host, int port, std::string& error);

    // 阻塞运行：循环接受连接并交给 handler
    void run(const HttpHandler& handler);

    // 请求服务器退出
    void stop();

    // 实际监听端口
    int port() const { return port_; }

private:
    uintptr_t listenSocket_;  // SOCKET（避免头文件污染，这里用整型保存）
    int port_;
    bool running_;
};

// 读取并解析一个 multipart/form-data 请求体；boundary 从 Content-Type 中提取
bool parseMultipartBody(const std::string& contentType, const std::string& body,
                        std::vector<UploadedFile>& files, std::string& error);

// 静态文件响应：把 root 目录下的 urlPath 读出来（带简单的目录穿越防护）
bool serveStaticFile(const std::string& rootDir, const std::string& urlPath, HttpResponse& response,
                     std::string& error);

// 根据扩展名返回 Content-Type
std::string contentTypeForPath(const std::string& path);

} // namespace pc
