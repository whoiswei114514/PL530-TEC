#pragma once
#include <Arduino.h>
#include <cctype>
#include <cstdlib>
#include <map>
enum HTTPMethod { HTTP_GET, HTTP_POST, HTTP_OPTIONS };
class WebServer {
 public:
  struct HeaderLess {
    bool operator()(const std::string& a, const std::string& b) const {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
          [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    }
  };
  struct Route { std::string path; HTTPMethod method; std::function<void()> handler; };
  std::map<std::string, String> args, responseHeaders;
  std::map<std::string, String, HeaderLess> headers;
  std::vector<Route> routes;
  std::vector<std::string> collectedHeaders;
  std::function<void()> notFound;
  String requestUri, requestHost, body, contentType;
  int code = 0;
  int clientCalls = 0;
  std::function<void()> clientHook;
  explicit WebServer(int) {}
  void resetRequest() {
    args.clear(); headers.clear(); responseHeaders.clear();
    body.clear(); contentType.clear(); code = 0;
  }
  bool hasArg(const char* key) const { return args.count(key) != 0; }
  String arg(const char* key) const {
    auto it = args.find(key); return it == args.end() ? String("") : it->second;
  }
  String header(const char* key) const {
    auto it = headers.find(key); return it == headers.end() ? String("") : it->second;
  }
  bool hasHeader(const char* key) const { return header(key).length() > 0; }
  String hostHeader() const { return requestHost; }
  // Match Parsing.cpp::urlDecode, including preservation of decoded NUL bytes.
  static String urlDecode(const String& text) {
    String decoded;
    for (unsigned int i = 0; i < text.length();) {
      char c = text.charAt(i++);
      if (c == '%' && i + 1 < text.length()) {
        char hex[] = {'0', 'x', text.charAt(i), text.charAt(i + 1), '\0'};
        i += 2;
        c = static_cast<char>(std::strtol(hex, nullptr, 16));
      } else if (c == '+') {
        c = ' ';
      }
      decoded += c;
    }
    return decoded;
  }
  void setHost(const String& host) {
    requestHost = host;
    headers["Host"] = String(host.c_str());
  }
  // Model only the pinned Parsing.cpp header boundary, not a full HTTP parser.
  // Collected fields reset and truncate at NUL; _hostHeader retains full length
  // and is not reset when the next request omits Host. Duplicate fields are last-wins.
  void parseHeaders(HTTPMethod method, const std::vector<std::string>& lines) {
    resetRequest();
    for (const auto& line : lines) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) break;
      const std::string name = line.substr(0, colon);
      const auto start = std::min(line.size(), colon + (method == HTTP_POST ? 1u : 2u));
      std::string value = line.substr(start);
      if (method == HTTP_POST) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
          value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
          value.pop_back();
        }
      }
      const HeaderLess less;
      const auto sameName = [&](const std::string& key) {
        return !less(name, key) && !less(key, name);
      };
      if (sameName("Host")) requestHost = value;
      if (sameName("Authorization") ||
          std::any_of(collectedHeaders.begin(), collectedHeaders.end(), sameName)) {
        headers[name] = String(value.c_str());
      }
    }
  }
  void sendHeader(const char* key, const String& value, bool = false) {
    responseHeaders[key] = value;
  }
  void send(int status, const char* type, const String& text) {
    code = status; contentType = type; body = text;
  }
  void send_P(int status, const char* type, const char* text) { send(status, type, text); }
  void collectHeaders(const char** names, size_t count) {
    collectedHeaders.assign(names, names + count);
  }
  void on(const char* path, HTTPMethod method, std::function<void()> handler) {
    routes.push_back({path, method, handler});
  }
  void onNotFound(std::function<void()> handler) { notFound = handler; }
  String uri() const { return requestUri; }
  void dispatch(const char* path, HTTPMethod method) {
    requestUri = path;
    for (const auto& route : routes) {
      if (route.path == path && route.method == method) { route.handler(); return; }
    }
    if (notFound) notFound();
  }
  void begin() {}
  void handleClient() {
    ++clientCalls;
    if (clientHook) clientHook();
  }
};
