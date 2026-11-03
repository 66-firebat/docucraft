#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "localsend_client.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path settingsPath() {
    wchar_t dir[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    std::filesystem::path base = n ? std::filesystem::path(dir)
                                   : std::filesystem::temp_directory_path();
    return base / L"Hardcopy" / L"gui-settings.json";
}

std::string winError(const char* where) {
    std::ostringstream os;
    os << where << " (Windows error " << GetLastError() << ")";
    return os.str();
}

bool request(const LocalSendPeer& peer, const wchar_t* method,
             const std::wstring& path, const std::string& contentType,
             const std::vector<unsigned char>& body, std::string& response,
             std::string& error, int timeoutMs = 5000) {
    HINTERNET session = WinHttpOpen(L"Hardcopy-GUI/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = winError("WinHttpOpen"); return false; }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring host = utf8ToWide(peer.ip);
    HINTERNET connect = WinHttpConnect(session, host.c_str(),
        static_cast<INTERNET_PORT>(peer.port), 0);
    if (!connect) {
        error = winError("WinHttpConnect");
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD flags = peer.useTls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        error = winError("WinHttpOpenRequest");
        WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        return false;
    }
    if (peer.useTls) {
        DWORD security = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &security, sizeof(security));
    }
    std::wstring headers;
    if (!contentType.empty()) headers = L"Content-Type: " + utf8ToWide(contentType);
    BOOL ok = WinHttpSendRequest(req,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1L),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<unsigned char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (!ok) error = winError("LocalSend request");

    DWORD status = 0, statusSize = sizeof(status);
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                nullptr, &status, &statusSize, nullptr);
    response.clear();
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), available, &read)) { ok = FALSE; break; }
        response.append(chunk.data(), read);
    }
    if (ok && (status < 200 || status >= 300)) {
        error = "LocalSend HTTP " + std::to_string(status) + ": " + response;
        ok = FALSE;
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
    return ok == TRUE;
}

std::string sha256(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, cb = 0, hashSize = 0;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                      sizeof(objectSize), &cb, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize),
                      sizeof(hashSize), &cb, 0);
    std::vector<unsigned char> object(objectSize), digest(hashSize);
    BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0);
    BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
    BCryptFinishHash(hash, digest.data(), hashSize, 0);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    std::ostringstream os;
    for (unsigned char b : digest) os << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return os.str();
}

std::string localFingerprint() {
    wchar_t name[256] = {};
    DWORD size = 256;
    GetComputerNameW(name, &size);
    std::string seed = wideToUtf8(name) + "-hardcopy-gui";
    return sha256(std::vector<unsigned char>(seed.begin(), seed.end()));
}

std::vector<std::string> localSubnets() {
    ULONG size = 0;
    GetAdaptersInfo(nullptr, &size);
    std::vector<unsigned char> buf(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    std::vector<std::string> result;
    if (GetAdaptersInfo(adapters, &size) != NO_ERROR) return result;
    for (auto* a = adapters; a; a = a->Next) {
        std::string gateway = a->GatewayList.IpAddress.String;
        if (gateway.empty() || gateway == "0.0.0.0") continue;
        for (auto* address = &a->IpAddressList; address; address = address->Next) {
            std::string value = address->IpAddress.String;
            auto dot = value.rfind('.');
            bool privateIp = value.rfind("10.", 0) == 0 || value.rfind("192.168.", 0) == 0;
            if (value.rfind("172.", 0) == 0) {
                int second = std::atoi(value.c_str() + 4);
                privateIp = second >= 16 && second <= 31;
            }
            if (privateIp && dot != std::string::npos) result.push_back(value.substr(0, dot));
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool tcpPortOpen(const std::string& ip, int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    fd_set writeSet, errorSet;
    FD_ZERO(&writeSet); FD_ZERO(&errorSet);
    FD_SET(s, &writeSet); FD_SET(s, &errorSet);
    timeval tv{0, 180000};
    int ready = select(0, nullptr, &writeSet, &errorSet, &tv);
    int socketError = 1;
    int errorSize = sizeof(socketError);
    if (ready > 0) getsockopt(s, SOL_SOCKET, SO_ERROR,
                              reinterpret_cast<char*>(&socketError), &errorSize);
    bool open = ready > 0 && FD_ISSET(s, &writeSet) && socketError == 0;
    closesocket(s);
    return open;
}

}

bool probeLocalSendPeer(const std::string& ip, int port, LocalSendPeer& peer, std::string& error) {
    for (bool tls : {true, false}) {
        LocalSendPeer candidate{"", ip, port, "", tls};
        std::string response;
        std::vector<unsigned char> empty;
        if (!request(candidate, L"GET", L"/api/localsend/v2/info", "", empty,
                     response, error, 650)) continue;
        try {
            auto j = json::parse(response);
            candidate.alias = j.value("alias", "LocalSend device");
            candidate.port = j.value("port", port);
            candidate.fingerprint = j.value("fingerprint", "");
            std::string protocol = j.value("protocol", tls ? "https" : "http");
            candidate.useTls = protocol != "http";
            if (!candidate.fingerprint.empty()) { peer = candidate; return true; }
        } catch (const std::exception& e) { error = e.what(); }
    }
    return false;
}

std::vector<LocalSendPeer> scanLocalSendPeers() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const auto subnets = localSubnets();
    std::vector<std::string> ips;
    ULONG tableSize = 0;
    GetIpNetTable(nullptr, &tableSize, FALSE);
    std::vector<unsigned char> tableBuf(tableSize);
    auto* table = reinterpret_cast<MIB_IPNETTABLE*>(tableBuf.data());
    if (GetIpNetTable(table, &tableSize, FALSE) == NO_ERROR) {
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            in_addr address{};
            address.s_addr = table->table[i].dwAddr;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &address, text, sizeof(text));
            std::string ip(text);
            if (std::any_of(subnets.begin(), subnets.end(), [&](const std::string& subnet) {
                    return ip.rfind(subnet + ".", 0) == 0;
                })) ips.push_back(ip);
        }
    }
    std::sort(ips.begin(), ips.end());
    ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
    std::atomic<size_t> next{0};
    std::mutex foundMutex;
    std::vector<LocalSendPeer> found;
    unsigned count = std::min<unsigned>(48, std::max<unsigned>(1, std::thread::hardware_concurrency() * 4));
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < count; ++t) workers.emplace_back([&] {
        for (;;) {
            size_t index = next.fetch_add(1);
            if (index >= ips.size()) break;
            if (!tcpPortOpen(ips[index], 53317)) continue;
            LocalSendPeer peer; std::string error;
            if (probeLocalSendPeer(ips[index], 53317, peer, error)) {
                std::lock_guard<std::mutex> lock(foundMutex);
                auto it = std::find_if(found.begin(), found.end(), [&](const auto& p) {
                    return p.fingerprint == peer.fingerprint;
                });
                if (it == found.end()) found.push_back(peer);
            }
        }
    });
    for (auto& w : workers) w.join();
    WSACleanup();
    return found;
}

bool sendLocalSendFile(const LocalSendPeer& peer, const std::string& filePath, std::string& error) {
    std::ifstream in(std::filesystem::path(utf8ToWide(filePath)), std::ios::binary);
    if (!in) { error = "Cannot open output file"; return false; }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    std::string fileName = wideToUtf8(std::filesystem::path(utf8ToWide(filePath)).filename().wstring());
    std::string fileId = "file-" + localFingerprint().substr(0, 8);
    std::string hash = sha256(data);
    std::string mime = std::filesystem::path(utf8ToWide(filePath)).extension() == L".pdf"
        ? "application/pdf" : "application/octet-stream";
    json prepare = {
        {"info", {{"alias", "Hardcopy GUI"}, {"version", "2.0"},
                  {"deviceModel", "Windows"}, {"deviceType", "desktop"},
                  {"fingerprint", localFingerprint()}, {"port", 53317},
                  {"protocol", peer.useTls ? "https" : "http"}, {"download", false}}},
        {"files", {{fileId, {{"id", fileId}, {"fileName", fileName},
                             {"size", data.size()}, {"fileType", mime},
                             {"hash", hash}, {"sha256", hash}}}}}
    };
    std::string body = prepare.dump(), response;
    std::vector<unsigned char> bodyBytes(body.begin(), body.end());
    if (!request(peer, L"POST", L"/api/localsend/v2/prepare-upload",
                 "application/json; charset=utf-8", bodyBytes, response, error, 30000)) return false;
    try {
        auto reply = json::parse(response);
        std::string session = reply.value("sessionId", "");
        if (session.empty()) return true;
        std::string token;
        if (reply.contains("files") && reply["files"].contains(fileId))
            token = reply["files"][fileId].get<std::string>();
        if (token.empty()) return true;
        std::wstring path = L"/api/localsend/v2/upload?sessionId=" + utf8ToWide(session) +
                            L"&fileId=" + utf8ToWide(fileId) + L"&token=" + utf8ToWide(token);
        return request(peer, L"POST", path, "application/octet-stream", data,
                       response, error, 120000);
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool loadBoundLocalSendPeer(LocalSendPeer& peer) {
    try {
        std::ifstream in(settingsPath());
        if (!in) return false;
        json root; in >> root;
        auto j = root.at("localSend");
        peer.alias = j.value("alias", "LocalSend device");
        peer.ip = j.value("ip", ""); peer.port = j.value("port", 53317);
        peer.fingerprint = j.value("fingerprint", "");
        peer.useTls = j.value("useTls", true);
        return !peer.ip.empty() && !peer.fingerprint.empty();
    } catch (...) { return false; }
}

bool saveBoundLocalSendPeer(const LocalSendPeer& peer) {
    try {
        auto path = settingsPath();
        std::filesystem::create_directories(path.parent_path());
        json root;
        std::ifstream in(path);
        if (in) { try { in >> root; } catch (...) { root = json::object(); } }
        root["localSend"] = {{"alias", peer.alias}, {"ip", peer.ip},
            {"port", peer.port}, {"fingerprint", peer.fingerprint}, {"useTls", peer.useTls}};
        std::ofstream out(path); out << root.dump(2);
        return bool(out);
    } catch (...) { return false; }
}
