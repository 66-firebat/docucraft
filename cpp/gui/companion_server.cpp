#define WIN32_LEAN_AND_MEAN
#include "companion_server.h"

#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <netfw.h>
#include <oleauto.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kApiBase = "/api/v1";
constexpr std::uint64_t kMaxUploadBytes = 1024ull * 1024ull * 1024ull;

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

std::string randomHex(size_t bytes) {
    std::random_device source;
    std::uniform_int_distribution<int> dist(0, 255);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes; ++i) out << std::setw(2) << dist(source);
    return out.str();
}

fs::path localAppDataPath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (size > 0 && size < MAX_PATH) return fs::path(buffer) / L"Hardcopy" / L"Companion";
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    return fs::path(temp) / L"Hardcopy" / L"Companion";
}

fs::path inboxPath() {
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH);
    if (size > 0 && size < MAX_PATH) return fs::path(buffer) / L"Downloads" / L"Hardcopy";
    return localAppDataPath() / L"Inbox";
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out(1, L'\"');
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'\"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'\"');
    return out;
}

constexpr const wchar_t* kFwTcpName = L"Hardcopy Companion TCP 53421";
constexpr const wchar_t* kFwUdpName = L"Hardcopy Companion UDP 53421";

std::wstring modulePath() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return self;
}

bool processIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

bool firewallRuleIsAllow(INetFwRules* rules, const wchar_t* name) {
    if (!rules || !name) return false;
    INetFwRule* rule = nullptr;
    BSTR key = SysAllocString(name);
    const HRESULT hr = rules->Item(key, &rule);
    SysFreeString(key);
    if (FAILED(hr) || !rule) return false;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
    rule->get_Enabled(&enabled);
    rule->get_Action(&action);
    rule->Release();
    return enabled == VARIANT_TRUE && action == NET_FW_ACTION_ALLOW;
}

void removeMatchingFirewallRules(INetFwRules* rules, const wchar_t* exePath) {
    if (!rules || !exePath) return;
    IUnknown* enumeratorUnknown = nullptr;
    if (FAILED(rules->get__NewEnum(&enumeratorUnknown)) || !enumeratorUnknown) return;
    IEnumVARIANT* enumerator = nullptr;
    enumeratorUnknown->QueryInterface(IID_IEnumVARIANT, reinterpret_cast<void**>(&enumerator));
    enumeratorUnknown->Release();
    if (!enumerator) return;
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        INetFwRule* rule = nullptr;
        if (item.vt == VT_DISPATCH && item.pdispVal)
            item.pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
        VariantClear(&item);
        if (!rule) continue;
        BSTR app = nullptr;
        BSTR name = nullptr;
        NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
        rule->get_ApplicationName(&app);
        rule->get_Name(&name);
        rule->get_Action(&action);
        const bool blockOurs = app && name && action == NET_FW_ACTION_BLOCK &&
            _wcsicmp(app, exePath) == 0;
        if (blockOurs) rules->Remove(name);
        if (app) SysFreeString(app);
        if (name) SysFreeString(name);
        rule->Release();
    }
    enumerator->Release();
}

HRESULT addPortRule(INetFwRules* rules, const wchar_t* name, NET_FW_IP_PROTOCOL protocol,
                    const wchar_t* exePath) {
    INetFwRule* rule = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwRule), reinterpret_cast<void**>(&rule));
    if (FAILED(hr) || !rule) return hr;
    BSTR nameB = SysAllocString(name);
    BSTR exeB = SysAllocString(exePath);
    BSTR portB = SysAllocString(L"53421");
    BSTR descB = SysAllocString(L"Allow the Supernote Hardcopy plugin to reach this PC");
    rule->put_Name(nameB);
    rule->put_Description(descB);
    rule->put_ApplicationName(exeB);
    rule->put_Protocol(protocol);
    rule->put_LocalPorts(portB);
    rule->put_Direction(NET_FW_RULE_DIR_IN);
    rule->put_Action(NET_FW_ACTION_ALLOW);
    rule->put_Enabled(VARIANT_TRUE);
    rule->put_Profiles(NET_FW_PROFILE2_ALL);
    rules->Remove(nameB);
    hr = rules->Add(rule);
    SysFreeString(nameB);
    SysFreeString(exeB);
    SysFreeString(portB);
    SysFreeString(descB);
    rule->Release();
    return hr;
}

bool installCompanionFirewallRules() {
    INetFwPolicy2* policy = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy));
    if (FAILED(hr) || !policy) return false;
    INetFwRules* rules = nullptr;
    hr = policy->get_Rules(&rules);
    if (FAILED(hr) || !rules) {
        policy->Release();
        return false;
    }
    const std::wstring exe = modulePath();
    removeMatchingFirewallRules(rules, exe.c_str());
    const HRESULT tcp = addPortRule(rules, kFwTcpName, NET_FW_IP_PROTOCOL_TCP, exe.c_str());
    const HRESULT udp = addPortRule(rules, kFwUdpName, NET_FW_IP_PROTOCOL_UDP, exe.c_str());
    const bool ok = SUCCEEDED(tcp) && SUCCEEDED(udp) &&
        firewallRuleIsAllow(rules, kFwTcpName) && firewallRuleIsAllow(rules, kFwUdpName);
    rules->Release();
    policy->Release();
    return ok;
}

bool companionFirewallReady() {
    INetFwPolicy2* policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy))) || !policy)
        return false;
    INetFwRules* rules = nullptr;
    const HRESULT hr = policy->get_Rules(&rules);
    const bool ok = SUCCEEDED(hr) && rules &&
        firewallRuleIsAllow(rules, kFwTcpName) && firewallRuleIsAllow(rules, kFwUdpName);
    if (rules) rules->Release();
    policy->Release();
    return ok;
}

bool launchElevatedFirewallSetup() {
    const std::wstring exe = modulePath();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = L"--setup-firewall";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return false;
    if (!info.hProcess) return false;
    WaitForSingleObject(info.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0;
}

fs::path executableDirectory() {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    return fs::path(self).parent_path();
}

fs::path locateCli() {
    const fs::path base = executableDirectory();
    const fs::path candidates[] = {
        base / L"hardcopy-cli.exe",
        base / L".." / L"cli" / L"hardcopy-cli.exe",
        base.parent_path() / L"cli" / L"hardcopy-cli.exe",
    };
    std::error_code ec;
    for (const auto& item : candidates) {
        if (fs::exists(item, ec)) return fs::absolute(item, ec);
    }
    return L"hardcopy-cli.exe";
}

std::string safeFileName(const std::string& value, const std::string& fallback) {
    fs::path candidate(utf8ToWide(value));
    std::wstring name = candidate.filename().wstring();
    if (name.empty() || name == L"." || name == L"..") name = utf8ToWide(fallback);
    static constexpr wchar_t forbidden[] = L"<>:\"/\\|?*";
    for (wchar_t& ch : name) {
        if (ch < 32 || std::wcschr(forbidden, ch)) ch = L'_';
    }
    return wideToUtf8(name);
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        std::string item = value.substr(start, end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.erase(item.begin());
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.pop_back();
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool validAssetSlot(const std::string& slot) {
    return slot == "font" || slot == "fontSans" || slot == "fontKai" ||
           slot == "fontFallback" || slot == "background";
}

fs::path windowsCjkFont() {
    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) return {};
    const fs::path fonts = fs::path(winDir) / L"Fonts";
    static const wchar_t* names[] = {
        L"msyh.ttc", L"msyh.ttf", L"simsun.ttc", L"simsun.ttf",
        L"simfang.ttf", L"simhei.ttf", L"msyhbd.ttc",
    };
    std::error_code ec;
    for (const wchar_t* name : names) {
        const fs::path candidate = fonts / name;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string pendingBody;
    std::uint64_t contentLength = 0;
};

bool sendAll(SOCKET socket, const char* data, size_t size) {
    while (size > 0) {
        const int chunk = static_cast<int>(std::min<size_t>(size, 1 << 20));
        const int sent = send(socket, data, chunk, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void sendResponse(SOCKET socket, int status, const char* reason,
                  const std::string& contentType, const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << contentType << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n";
    for (const auto& item : extra) header << item.first << ": " << item.second << "\r\n";
    header << "\r\n";
    const std::string head = header.str();
    sendAll(socket, head.data(), head.size());
    if (!body.empty()) sendAll(socket, body.data(), body.size());
}

void sendJson(SOCKET socket, int status, const json& body) {
    const char* reason = status == 200 ? "OK" : status == 201 ? "Created" :
                         status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
                         status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" :
                         status == 404 ? "Not Found" : status == 409 ? "Conflict" :
                         status == 413 ? "Payload Too Large" : "Server Error";
    sendResponse(socket, status, reason, "application/json; charset=utf-8", body.dump());
}

bool readRequestHead(SOCKET socket, HttpRequest& request) {
    std::string data;
    std::array<char, 8192> chunk{};
    while (data.find("\r\n\r\n") == std::string::npos) {
        const int got = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (got <= 0) return false;
        data.append(chunk.data(), static_cast<size_t>(got));
        if (data.size() > 128 * 1024) return false;
    }
    const size_t split = data.find("\r\n\r\n");
    request.pendingBody = data.substr(split + 4);
    std::istringstream lines(data.substr(0, split));
    std::string first;
    if (!std::getline(lines, first)) return false;
    if (!first.empty() && first.back() == '\r') first.pop_back();
    std::istringstream firstLine(first);
    std::string version;
    if (!(firstLine >> request.method >> request.path >> version)) return false;
    const size_t query = request.path.find('?');
    if (query != std::string::npos) request.path.resize(query);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
        request.headers[key] = value;
    }
    auto length = request.headers.find("content-length");
    if (length != request.headers.end()) {
        try { request.contentLength = std::stoull(length->second); } catch (...) { return false; }
    }
    return true;
}

bool readBody(SOCKET socket, const HttpRequest& request, std::string& body) {
    if (request.contentLength > 8 * 1024 * 1024) return false;
    body = request.pendingBody;
    std::array<char, 64 * 1024> chunk{};
    while (body.size() < request.contentLength) {
        const size_t remaining = static_cast<size_t>(request.contentLength - body.size());
        const int got = recv(socket, chunk.data(), static_cast<int>(std::min(remaining, chunk.size())), 0);
        if (got <= 0) return false;
        body.append(chunk.data(), static_cast<size_t>(got));
    }
    if (body.size() > request.contentLength) body.resize(static_cast<size_t>(request.contentLength));
    return true;
}

std::vector<std::string> pathParts(const std::string& path) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') ++start;
        if (start >= path.size()) break;
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        result.push_back(path.substr(start, end - start));
        start = end;
    }
    return result;
}

}

struct CompanionServer::Impl {
    struct Job {
        std::string id;
        std::string type;
        std::string fileName;
        std::string outputFormat;
        json options = json::object();
        fs::path directory;
        fs::path inputPath;
        fs::path outputPath;
        std::unordered_map<std::string, fs::path> assets;
        std::string status = "queued";
        std::string stage = "queued";
        std::string message;
        int progress = 0;
        std::uint64_t receivedBytes = 0;
        std::uint64_t outputBytes = 0;
        std::string batchId;
        int batchIndex = 0;
        int batchCount = 0;
        std::uint64_t seq = 0;
        bool cancelled = false;
        HANDLE process = nullptr;
        mutable std::mutex mutex;
    };

    std::atomic<bool> running{false};
    std::atomic<std::int64_t> pairingUntil{0};
    std::atomic<int> activeJobs{0};
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET discoverySocket = INVALID_SOCKET;
    std::thread acceptThread;
    std::thread discoveryThread;
    std::mutex clientThreadsMutex;
    std::vector<std::thread> clientThreads;
    std::mutex workerThreadsMutex;
    std::vector<std::thread> workerThreads;
    mutable std::mutex stateMutex;
    mutable std::mutex logMutex;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    std::uint64_t nextSeq = 1;
    std::set<std::string> cancelledBatches;
    static constexpr int kConvertSlots = 2;
    std::mutex slotMutex;
    std::condition_variable slotCv;
    int slotsUsed = 0;
    std::mutex pcSettingsMutex;
    CompanionServer::PcSettingsCommand pcSettingsCommand;
    std::vector<std::string> recent;
    std::vector<std::string> tokens;
    fs::path root = localAppDataPath();
    std::string id;
    std::string name;
    std::string statusText = "Companion initializing";
    std::string firewallText = "Checking Windows Firewall";
    bool firewallOk = false;
    bool firewallElevateTried = false;

    Impl() {
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(computer, &length)) name = "Hardcopy PC · " + wideToUtf8(computer);
        if (name.empty()) name = "Hardcopy PC";
        loadSettings();
    }

    ~Impl() { stop(); }

    static std::int64_t nowSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool isPairingEnabled() const { return pairingUntil.load() > nowSeconds(); }

    void loadSettings() {
        std::error_code ec;
        fs::create_directories(root / L"jobs", ec);
        const fs::path path = root / L"settings.json";
        try {
            std::ifstream input(path, std::ios::binary);
            if (input) {
                json settings;
                input >> settings;
                id = settings.value("deviceId", "");
                tokens = settings.value("tokens", std::vector<std::string>{});
                firewallElevateTried = settings.value("firewallElevateTried", false);
            }
        } catch (...) {}
        if (id.empty()) {
            id = randomHex(16);
            saveSettings();
        }
        if (tokens.empty()) pairingUntil.store(nowSeconds() + 120);
    }

    void saveSettings() {
        std::error_code ec;
        fs::create_directories(root, ec);
        const fs::path temp = root / L"settings.json.tmp";
        const fs::path target = root / L"settings.json";
        try {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            output << json{{"deviceId", id}, {"tokens", tokens},
                           {"firewallElevateTried", firewallElevateTried}}.dump(2);
            output.close();
            fs::remove(target, ec);
            fs::rename(temp, target, ec);
        } catch (...) {}
    }

    bool authorized(const HttpRequest& request) const {
        auto header = request.headers.find("authorization");
        if (header == request.headers.end()) return false;
        constexpr const char* prefix = "Bearer ";
        if (header->second.rfind(prefix, 0) != 0) return false;
        const std::string token = header->second.substr(std::strlen(prefix));
        std::lock_guard<std::mutex> lock(stateMutex);
        return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
    }

    static std::string winError(DWORD code) {
        char* message = nullptr;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<char*>(&message), 0, nullptr);
        std::string result = message ? message : std::to_string(code);
        if (message) LocalFree(message);
        return result;
    }

    bool receiveUpload(SOCKET socket, const HttpRequest& request, const std::shared_ptr<Job>& job,
                       const fs::path& dest, const char* kind, bool updateProgress) {
        HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            diagnostic(std::string(kind) + " CreateFile error=" + winError(GetLastError()) +
                       " path=" + wideToUtf8(dest.wstring()));
            return false;
        }
        diagnostic(std::string(kind) + " start job=" + job->id +
                   " bytes=" + std::to_string(request.contentLength) +
                   " path=" + wideToUtf8(dest.wstring()));
        auto writeChunk = [&](const char* data, size_t size) {
            while (size > 0) {
                DWORD writtenNow = 0;
                const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1 << 20));
                if (!WriteFile(file, data, chunk, &writtenNow, nullptr) || writtenNow == 0) return false;
                data += writtenNow;
                size -= writtenNow;
            }
            return true;
        };
        std::uint64_t written = 0;
        if (!request.pendingBody.empty()) {
            const size_t count = static_cast<size_t>(std::min<std::uint64_t>(
                request.pendingBody.size(), request.contentLength));
            if (!writeChunk(request.pendingBody.data(), count)) {
                diagnostic("upload pending WriteFile error=" + winError(GetLastError()));
                CloseHandle(file);
                return false;
            }
            written += count;
        }
        std::vector<char> chunk(64 * 1024);
        while (written < request.contentLength) {
            const size_t remaining = static_cast<size_t>(request.contentLength - written);
            const int got = recv(socket, chunk.data(), static_cast<int>(std::min(remaining, chunk.size())), 0);
            if (got <= 0 || !writeChunk(chunk.data(), static_cast<size_t>(got))) {
                diagnostic("upload recv/write error=" + winError(WSAGetLastError()));
                CloseHandle(file);
                return false;
            }
            written += static_cast<std::uint64_t>(got);
            if (updateProgress) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->progress = request.contentLength > 0
                    ? static_cast<int>(written * 100 / request.contentLength) : 100;
                job->receivedBytes = written;
            }
        }
        FlushFileBuffers(file);
        CloseHandle(file);
        diagnostic(std::string(kind) + " done job=" + job->id + " bytes=" + std::to_string(written));
        return true;
    }

    json infoJson() const {
        return {
            {"service", "hardcopy-companion"},
            {"name", name},
            {"version", "1.0"},
            {"protocolVersion", 1},
            {"port", CompanionServer::kPort},
            {"deviceId", id},
            {"pairingEnabled", isPairingEnabled()},
            {"capabilities", {"file-receive", "pdf-reflow", "pdf-raster",
                              "epub-convert", "mobi-convert", "cbz-convert",
                              "job-assets"}},
        };
    }

    json jobJson(const std::shared_ptr<Job>& job) const {
        std::lock_guard<std::mutex> lock(job->mutex);
        return {
            {"id", job->id}, {"type", job->type}, {"status", job->status},
            {"stage", job->stage}, {"progress", job->progress},
            {"message", job->message}, {"receivedBytes", job->receivedBytes},
            {"outputBytes", job->outputBytes},
            {"outputReady", job->status == "completed" && !job->outputPath.empty()},
            {"cancelled", job->cancelled},
        };
    }

    std::shared_ptr<Job> findJob(const std::string& jobId) const {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = jobs.find(jobId);
        return found == jobs.end() ? nullptr : found->second;
    }

    void addRecent(const std::string& text) {
        std::lock_guard<std::mutex> lock(stateMutex);
        recent.insert(recent.begin(), text);
        if (recent.size() > 20) recent.resize(20);
    }

    static std::string batchTag(const Job& job) {
        const int count = job.options.value("batchCount", 0);
        if (count <= 1) return {};
        const int index = job.options.value("batchIndex", 0);
        return "[" + std::to_string(index + 1) + "/" + std::to_string(count) + "] ";
    }

    void diagnostic(const std::string& text) const {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
        std::tm local{};
        localtime_s(&local, &t);
        char stamp[40];
        std::snprintf(stamp, sizeof(stamp),
                      "%04d-%02d-%02d %02d:%02d:%02d.%03d ",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec, (int)ms);
        const std::string line = std::string(stamp) + text;
        OutputDebugStringA((line + "\n").c_str());
        std::lock_guard<std::mutex> lock(logMutex);
        std::error_code ec;
        fs::create_directories(root, ec);
        std::ofstream output(root / L"server.log", std::ios::binary | std::ios::app);
        output << line << '\n';
    }

    static std::string optionKeys(const json& options) {
        if (!options.is_object()) return {};
        std::string keys;
        for (auto it = options.begin(); it != options.end(); ++it) {
            if (!keys.empty()) keys += ",";
            keys += it.key();
        }
        return keys;
    }

    static std::string peerHost(SOCKET socket) {
        sockaddr_in address{};
        int length = sizeof(address);
        if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return "?";
        char host[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
        return std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
    }

    void setJob(const std::shared_ptr<Job>& job, const std::string& status,
                const std::string& stage, int progress, const std::string& message = {}) {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->cancelled && stage != "cancelled") return;
        job->status = status;
        job->stage = stage;
        job->progress = std::clamp(progress, 0, 100);
        job->message = message;
    }

    bool cancelJob(const std::shared_ptr<Job>& job, const std::string& reason) {
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->status == "completed" || job->status == "failed" || job->cancelled) return false;
            job->cancelled = true;
            job->status = "failed";
            job->stage = "cancelled";
            job->message = reason;
            if (job->process) TerminateProcess(job->process, 8);
        }
        slotCv.notify_all();
        diagnostic("job cancel id=" + job->id + " reason=" + reason);
        addRecent("Cancelled · " + batchTag(*job) + safeFileName(job->fileName, "document"));
        return true;
    }

    bool jobCancelled(const std::shared_ptr<Job>& job) const {
        std::lock_guard<std::mutex> lock(job->mutex);
        return job->cancelled;
    }

    void cancelBatch(const std::string& batchId) {
        if (batchId.empty()) return;
        std::vector<std::shared_ptr<Job>> members;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            cancelledBatches.insert(batchId);
            for (const auto& entry : jobs)
                if (entry.second->batchId == batchId) members.push_back(entry.second);
        }
        for (const auto& job : members) cancelJob(job, "Batch cancelled on PC");
    }

    bool acquireSlots(const std::shared_ptr<Job>& job, int need) {
        std::unique_lock<std::mutex> lock(slotMutex);
        slotCv.wait(lock, [&] { return slotsUsed + need <= kConvertSlots || jobCancelled(job); });
        if (jobCancelled(job)) return false;
        slotsUsed += need;
        return true;
    }

    void releaseSlots(int need) {
        {
            std::lock_guard<std::mutex> lock(slotMutex);
            slotsUsed -= need;
        }
        slotCv.notify_all();
    }

    std::wstring buildConvertCommand(const std::shared_ptr<Job>& job) {
        {
            std::lock_guard<std::mutex> lock(pcSettingsMutex);
            if (pcSettingsCommand) {
                const json options = job->options.is_object() ? job->options : json::object();
                CompanionServer::PhoneJob phone;
                phone.cli = locateCli().wstring();
                phone.input = wideToUtf8(job->inputPath.wstring());
                phone.output = wideToUtf8(job->outputPath.wstring());
                phone.comic = options.value("comicMode", false) || job->outputFormat == "cbz";
                phone.pdfRaster = options.value("pdfMode", std::string("reflow")) == "raster";
                phone.pageWidth = options.value("pageWidth", 1920);
                phone.pageHeight = options.value("pageHeight", 2560);
                std::wstring command = pcSettingsCommand(phone);
                if (!command.empty()) {
                    diagnostic("convert uses PC settings job=" + job->id);
                    return command;
                }
            }
        }
        const json options = job->options.is_object() ? job->options : json::object();
        std::wstring command = quoteArg(locateCli().wstring());
        auto add = [&](const wchar_t* flag) { command.append(L" ").append(flag); };
        auto addString = [&](const wchar_t* flag, const std::string& value) {
            if (value.empty()) return;
            add(flag); command.push_back(L' '); command.append(quoteArg(utf8ToWide(value)));
        };
        auto addInt = [&](const wchar_t* flag, int value) {
            add(flag); command.push_back(L' '); command.append(std::to_wstring(value));
        };
        auto addFloat = [&](const wchar_t* flag, double value) {
            std::wostringstream text; text << std::fixed << std::setprecision(3) << value;
            add(flag); command.push_back(L' '); command.append(text.str());
        };
        addString(L"--input", wideToUtf8(job->inputPath.wstring()));
        addString(L"--output", wideToUtf8(job->outputPath.wstring()));
        const bool comic = options.value("comicMode", false) || job->outputFormat == "cbz";
        if (comic) {
            add(L"--comic");
            const int dither = options.value("ditherAlgo", 0);
            if (dither == 1) addString(L"--dither", "bayer");
            if (dither == 2) addString(L"--dither", "fs");
            addInt(L"--dither-levels", options.value("ditherLevels", 16));
            addInt(L"--quality", options.value("jpegQuality", 90));
            const int tone = options.value("comicTone", 0);
            if (tone != 0) addInt(L"--comic-tone", tone);
            if (!options.value("comicSkipBlank", true)) add(L"--comic-keep-blank");
            const int margin = options.value("comicMarginPercent", 0);
            if (margin > 0) addInt(L"--comic-margin", std::min(margin, 25));
            const int spread = options.value("comicSpread", 0);
            if (spread == 1) addString(L"--comic-spread", "ltr");
            if (spread == 2) addString(L"--comic-spread", "rtl");
            const int width = options.value("comicResizeW", 0);
            const int height = options.value("comicResizeH", 0);
            if (width > 0 && height > 0)
                addString(L"--resize", std::to_string(width) + "x" + std::to_string(height));
            return command;
        }
        auto localAsset = [&](const char* slot) -> std::string {
            std::lock_guard<std::mutex> lock(job->mutex);
            auto found = job->assets.find(slot);
            if (found == job->assets.end()) return {};
            std::error_code ec;
            if (!fs::is_regular_file(found->second, ec) || fs::file_size(found->second, ec) == 0)
                return {};
            return wideToUtf8(found->second.wstring());
        };
        const std::string font = localAsset("font");
        const std::string fontSans = localAsset("fontSans");
        const std::string fontKai = localAsset("fontKai");
        const std::string fontFallback = localAsset("fontFallback");
        const std::string background = localAsset("background");
        if (!font.empty()) {
            addString(L"--font", font);
        } else {
            const fs::path winFont = windowsCjkFont();
            if (!winFont.empty()) addString(L"--font", wideToUtf8(winFont.wstring()));
            else addString(L"--font-family", "Microsoft YaHei");
        }
        if (!fontSans.empty()) addString(L"--font-sans", fontSans);
        if (!fontKai.empty()) addString(L"--font-kai", fontKai);
        const std::string monoPath = options.value("fontPathMono", std::string());
        if (!fontKai.empty() && !monoPath.empty()
            && monoPath == options.value("fontPathKai", std::string()))
            addString(L"--font-mono", fontKai);
        if (!fontFallback.empty()) addString(L"--font-fallback", fontFallback);
        if (!background.empty()) addString(L"--background", background);
        diagnostic(std::string("convert assets font=") + (font.empty() ? "win-fallback" : font) +
                   " sans=" + (fontSans.empty() ? "-" : fontSans) +
                   " kai=" + (fontKai.empty() ? "-" : fontKai) +
                   " fallback=" + (fontFallback.empty() ? "-" : fontFallback) +
                   " bg=" + (background.empty() ? "-" : background));
        addInt(L"--page-width", options.value("pageWidth", 1920));
        addInt(L"--page-height", options.value("pageHeight", 2560));
        addFloat(L"--font-size", options.value("fontSize", 44.0));
        addInt(L"--quality", options.value("jpegQuality", 90));
        const std::string preset = options.value("preset", "publish");
        addString(L"--preset", preset);
        if (options.value("orientation", "horizontal") == "vertical-rtl") add(L"--vertical");
        if (options.contains("marginLeftU")) addFloat(L"--margin-left", options.value("marginLeftU", 13.4));
        if (options.contains("marginRightU")) addFloat(L"--margin-right", options.value("marginRightU", 13.4));
        if (options.contains("marginTopU")) addFloat(L"--margin-top", options.value("marginTopU", 12.0));
        if (options.contains("marginBottomU")) addFloat(L"--margin-bottom", options.value("marginBottomU", 12.0));
        if (options.contains("marginLeft")) addInt(L"--margin-left-px", options.value("marginLeft", 134));
        if (options.contains("marginRight")) addInt(L"--margin-right-px", options.value("marginRight", 134));
        if (options.contains("marginTop")) addInt(L"--margin-top-px", options.value("marginTop", 120));
        if (options.contains("marginBottom")) addInt(L"--margin-bottom-px", options.value("marginBottom", 120));
        if (options.contains("lineHeightMul")) addFloat(L"--line-height", options.value("lineHeightMul", 1.6));
        if (options.contains("paragraphSpacing")) {
            const double fontSize = std::max(1.0, options.value("fontSize", 44.0));
            addFloat(L"--paragraph-spacing", options.value("paragraphSpacing", 0.0) / fontSize);
        }
        if (options.contains("firstLineIndentChars"))
            addFloat(L"--indent", options.value("firstLineIndentChars", 2.0));
        if (options.value("whiteText", false)) add(L"--white-text");
        if (!options.value("embedBookmarks", true)) add(L"--no-bookmarks");
        if (options.value("splitLandscape", false)) add(L"--split-landscape");
        if (options.value("reversePages", false)) add(L"--reverse-pages");
        if (options.contains("rulingStyle"))
            addString(L"--ruling-style", options.value("rulingStyle", std::string("dotted")));
        if (!options.value("drawRuling", true)) add(L"--no-ruling");
        if (options.value("syntheticBold", false)) add(L"--bold");
        if (options.value("footerProgress", false)) add(L"--footer");
        addFloat(L"--contrast", options.value("contrastBoost", 0.1));
        addFloat(L"--stroke", options.value("strokeExpand", 0.0));
        addFloat(L"--image-contrast", options.value("imageContrastBoost", 0.0));
        const std::string zh = options.value("zhConvert", std::string("none"));
        if (zh == "s2t" || zh == "t2s") addString(L"--zh-convert", zh);
        add(options.value("pdfTrim", true) ? L"--pdf-trim" : L"--pdf-no-trim");
        addInt(L"--pdf-header-cut", options.value("pdfHeaderCutPercent", -1));
        addInt(L"--pdf-footer-cut", options.value("pdfFooterCutPercent", -1));
        addInt(L"--pdf-max-output-pages", options.value("pdfMaxOutputPages", 0));
        addInt(L"--pdf-watermark", options.value("pdfWatermarkThreshold", 0));
        addFloat(L"--pdf-gamma", options.value("pdfGamma", 0.0));
        addFloat(L"--pdf-stroke", options.value("pdfStroke", 0.0));
        addFloat(L"--pdf-image-contrast", options.value("pdfImageContrast", 0.0));
        addFloat(L"--pdf-fill-tone", options.value("pdfFillTone", 0.0));
        if (options.value("pdfColumns", 0) > 0) addString(L"--pdf-columns", "auto");
        if (options.value("pdfOcr", false)) add(L"--pdf-ocr");
        if (options.value("pdfIgnoreText", false)) add(L"--pdf-ignore-text");
        const std::string pdfPreset = options.value("pdfPreset", std::string());
        if (pdfPreset == "web" || pdfPreset == "publish" || pdfPreset == "fresh" || pdfPreset == "note")
            addString(L"--pdf-preset", pdfPreset);
        addFloat(L"--pdf-contrast", options.value("pdfContrast", 1.0));
        if (options.value("pdfMode", "reflow") == "raster") add(L"--pdf-raster");
        else add(L"--pdf-reflow");
        return command;
    }

    int runConvert(const std::shared_ptr<Job>& job) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
            diagnostic("convert CreatePipe failed " + winError(GetLastError()));
            return -1;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        const fs::path cli = locateCli();
        std::error_code existsEc;
        const bool cliExists = fs::exists(cli, existsEc);
        diagnostic("convert cli=" + wideToUtf8(cli.wstring()) +
                   " exists=" + (cliExists ? "1" : "0") +
                   " keys=" + optionKeys(job->options));
        std::wstring command = buildConvertCommand(job);
        diagnostic("convert cmd=" + wideToUtf8(command));
        PROCESS_INFORMATION process{};
        BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!created) {
            const DWORD err = GetLastError();
            diagnostic("CreateProcess failed err=" + std::to_string(err) + " " + winError(err));
            CloseHandle(readPipe);
            return -2;
        }
        diagnostic("convert pid=" + std::to_string(process.dwProcessId) + " job=" + job->id);
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->process = process.hProcess;
            if (job->cancelled) TerminateProcess(process.hProcess, 8);
        }
        std::string buffer;
        std::array<char, 2048> chunk{};
        DWORD got = 0;
        while (ReadFile(readPipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
            buffer.append(chunk.data(), got);
            for (;;) {
                const size_t newline = buffer.find('\n');
                if (newline == std::string::npos) break;
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                const size_t close = line.find(']');
                if (!line.empty()) diagnostic("cli " + line);
                if (!line.empty() && line.front() == '[' && close != std::string::npos) {
                    std::istringstream parsed(line.substr(close + 1));
                    std::string stage;
                    std::string percent;
                    parsed >> stage >> percent;
                    if (!percent.empty() && percent.back() == '%') percent.pop_back();
                    int value = 0;
                    try { value = std::stoi(percent); } catch (...) {}
                    setJob(job, "processing", stage.empty() ? "processing" : stage, value);
                }
            }
        }
        CloseHandle(readPipe);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->process = nullptr;
        }
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        diagnostic("convert exit=" + std::to_string(code) + " job=" + job->id);
        return static_cast<int>(code);
    }

    void processJob(const std::shared_ptr<Job>& job) {
        activeJobs.fetch_add(1);
        if (job->type == "receive") {
            setJob(job, "processing", "saving", 50);
            std::error_code ec;
            fs::create_directories(inboxPath(), ec);
            fs::path destination = inboxPath() / utf8ToWide(safeFileName(job->fileName, "hardcopy.pdf"));
            if (fs::exists(destination, ec)) {
                destination = destination.parent_path() /
                    (destination.stem().wstring() + L"_" + utf8ToWide(job->id.substr(0, 8)) + destination.extension().wstring());
            }
            fs::copy_file(job->inputPath, destination, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->outputPath = destination;
                    job->outputBytes = fs::file_size(destination, ec);
                }
                setJob(job, "completed", "done", 100, wideToUtf8(destination.wstring()));
                addRecent("Received · " + safeFileName(job->fileName, "hardcopy.pdf"));
            } else {
                setJob(job, "failed", "saving", 100, "Save operation completed with a filesystem message");
                addRecent("Receive · filesystem message");
            }
        } else if (job->type == "convert" &&
                   !acquireSlots(job, job->options.value("pdfOcr", false) ? kConvertSlots : 1)) {
            diagnostic("convert skipped job=" + job->id + ": cancelled while queued");
        } else if (job->type == "convert") {
            const int need = job->options.value("pdfOcr", false) ? kConvertSlots : 1;
            setJob(job, "processing", "parse", 0);
            const int code = runConvert(job);
            releaseSlots(need);
            std::error_code ec;
            if (jobCancelled(job)) {
                diagnostic("convert cancelled job=" + job->id + " exit=" + std::to_string(code));
            } else if (code == 0 && fs::is_regular_file(job->outputPath, ec) && fs::file_size(job->outputPath, ec) > 0) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->outputBytes = fs::file_size(job->outputPath, ec);
                }
                diagnostic("convert ok job=" + job->id +
                           " outputBytes=" + std::to_string(job->outputBytes) +
                           " path=" + wideToUtf8(job->outputPath.wstring()));
                setJob(job, "completed", "done", 100);
                addRecent("Converted · " + batchTag(*job) + safeFileName(job->fileName, "document"));
            } else {
                diagnostic("convert fail job=" + job->id +
                           " exit=" + std::to_string(code) +
                           " output=" + wideToUtf8(job->outputPath.wstring()));
                setJob(job, "failed", "convert", 100, "Converter exit " + std::to_string(code));
                addRecent("Convert · " + batchTag(*job) + safeFileName(job->fileName, "document") +
                          " · exit " + std::to_string(code));
            }
        } else {
            setJob(job, "failed", "job", 100, "Job type requires a newer protocol capability");
        }
        activeJobs.fetch_sub(1);
    }

    void launchJob(const std::shared_ptr<Job>& job) {
        std::lock_guard<std::mutex> lock(workerThreadsMutex);
        workerThreads.emplace_back([this, job] { processJob(job); });
    }

    void handleClient(SOCKET socket) {
        DWORD timeout = 120000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        HttpRequest request;
        if (!readRequestHead(socket, request)) {
            diagnostic("http bad head from " + peerHost(socket));
            closesocket(socket);
            return;
        }
        const bool quietPoll = request.method == "GET" &&
            request.path.rfind(std::string(kApiBase) + "/jobs/", 0) == 0 &&
            request.path.find("/input") == std::string::npos &&
            request.path.find("/output") == std::string::npos &&
            request.path.find("/assets") == std::string::npos;
        if (!quietPoll) {
            diagnostic("http " + request.method + " " + request.path +
                       " from " + peerHost(socket) +
                       " len=" + std::to_string(request.contentLength));
        }
        try {
            if (request.method == "GET" && request.path == std::string(kApiBase) + "/info") {
                sendJson(socket, 200, infoJson());
                closesocket(socket);
                return;
            }
            if (request.method == "POST" && request.path == std::string(kApiBase) + "/pair") {
                if (!isPairingEnabled()) {
                    diagnostic("pair rejected: pairing window closed");
                    sendJson(socket, 403, {{"message", "请先在 Hardcopy PC 端开启配对"}});
                    closesocket(socket);
                    return;
                }
                std::string body;
                if (!readBody(socket, request, body)) throw std::runtime_error("pair body");
                const json client = body.empty() ? json::object() : json::parse(body);
                const std::string token = randomHex(32);
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    tokens.push_back(token);
                    if (tokens.size() > 16) tokens.erase(tokens.begin());
                    saveSettings();
                }
                pairingUntil.store(0);
                addRecent("Paired · " + client.value("name", "Supernote Hardcopy"));
                const std::string deviceId = client.value("deviceId", "");
                diagnostic("pair ok name=" + client.value("name", "Supernote Hardcopy") +
                           " deviceId=" + (deviceId.size() > 8 ? deviceId.substr(0, 8) : deviceId) +
                           " tokenLen=" + std::to_string(token.size()));
                json response = infoJson();
                response["token"] = token;
                sendJson(socket, 200, response);
                closesocket(socket);
                return;
            }
            if (!authorized(request)) {
                diagnostic("http 401 " + request.method + " " + request.path +
                           " from " + peerHost(socket) +
                           " hasAuth=" + (request.headers.count("authorization") ? "1" : "0"));
                sendJson(socket, 401, {{"message", "请在 Hardcopy 插件中重新绑定 PC"}});
                closesocket(socket);
                return;
            }
            if (request.method == "POST" && request.path == std::string(kApiBase) + "/jobs") {
                std::string body;
                if (!readBody(socket, request, body)) throw std::runtime_error("job body");
                const json spec = json::parse(body);
                const std::string type = spec.value("type", "");
                if (type != "receive" && type != "convert") {
                    sendJson(socket, 400, {{"message", "Supported job types: receive, convert"}});
                    closesocket(socket);
                    return;
                }
                auto job = std::make_shared<Job>();
                job->id = randomHex(16);
                job->type = type;
                job->fileName = safeFileName(spec.value("fileName", "input.bin"), "input.bin");
                job->outputFormat = lower(spec.value("outputFormat", type == "convert" ? "pdf" : "bin"));
                if (job->outputFormat != "cbz") job->outputFormat = "pdf";
                job->options = spec.value("options", json::object());
                if (job->options.is_object()) {
                    job->batchId = job->options.value("batchId", std::string());
                    job->batchIndex = job->options.value("batchIndex", 0);
                    job->batchCount = job->options.value("batchCount", 0);
                }
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (!job->batchId.empty() && cancelledBatches.count(job->batchId)) {
                        sendJson(socket, 409, {{"message", "Batch cancelled on PC"},
                                               {"code", "batch_cancelled"}});
                        closesocket(socket);
                        return;
                    }
                    job->seq = nextSeq++;
                }
                job->directory = root / L"jobs" / utf8ToWide(job->id);
                std::error_code ec;
                fs::create_directories(job->directory, ec);
                const std::wstring extension = fs::path(utf8ToWide(job->fileName)).extension().wstring();
                job->inputPath = job->directory / (L"input" + (extension.empty() ? L".bin" : extension));
                if (type == "convert") job->outputPath = job->directory /
                    (job->outputFormat == "cbz" ? L"output.cbz" : L"output.pdf");
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    jobs[job->id] = job;
                }
                diagnostic("job create id=" + job->id + " type=" + type +
                           " file=" + job->fileName + " fmt=" + job->outputFormat +
                           " keys=" + optionKeys(job->options));
                sendJson(socket, 201, {{"id", job->id}, {"status", "queued"},
                                      {"uploadPath", std::string(kApiBase) + "/jobs/" + job->id + "/input"}});
                closesocket(socket);
                return;
            }
            const std::vector<std::string> parts = pathParts(request.path);
            if (parts.size() >= 4 && parts[0] == "api" && parts[1] == "v1" && parts[2] == "jobs") {
                auto job = findJob(parts[3]);
                if (!job) {
                    sendJson(socket, 404, {{"message", "Job id is ready for a fresh request"}});
                    closesocket(socket);
                    return;
                }
                if (request.method == "GET" && parts.size() == 4) {
                    sendJson(socket, 200, jobJson(job));
                    closesocket(socket);
                    return;
                }
                if (request.method == "POST" && parts.size() == 5 && parts[4] == "cancel") {
                    std::string body;
                    readBody(socket, request, body);
                    bool wholeBatch = false;
                    try { wholeBatch = json::parse(body.empty() ? "{}" : body).value("batch", false); } catch (...) {}
                    if (wholeBatch && !job->batchId.empty()) cancelBatch(job->batchId);
                    else cancelJob(job, "Cancelled on Supernote");
                    sendJson(socket, 200, jobJson(job));
                    closesocket(socket);
                    return;
                }
                if (request.method == "POST" && parts.size() == 6 && parts[4] == "assets") {
                    const std::string slot = parts[5];
                    if (!validAssetSlot(slot)) {
                        sendJson(socket, 400, {{"message", "Unknown asset slot"}});
                        closesocket(socket);
                        return;
                    }
                    if (request.contentLength == 0 || request.contentLength > kMaxUploadBytes) {
                        sendJson(socket, 413, {{"message", "Asset size is outside the accepted range"}});
                        closesocket(socket);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        if (job->status != "queued") {
                            sendJson(socket, 409, {{"message", "Job assets must arrive before the document"}});
                            closesocket(socket);
                            return;
                        }
                    }
                    std::string rawName;
                    auto nameHeader = request.headers.find("x-file-name");
                    if (nameHeader != request.headers.end()) rawName = nameHeader->second;
                    const std::string safe = safeFileName(rawName, slot + ".bin");
                    std::wstring ext = fs::path(utf8ToWide(safe)).extension().wstring();
                    if (ext.empty() || ext.size() > 8) ext = L".bin";
                    const fs::path dest = job->directory / (utf8ToWide(slot) + ext);
                    const bool stored = receiveUpload(socket, request, job, dest, "asset", false);
                    if (!stored) {
                        sendJson(socket, 400, {{"message", "Asset upload did not finish"}});
                        closesocket(socket);
                        return;
                    }
                    std::vector<std::string> aliases{slot};
                    std::string alsoList;
                    auto alsoHeader = request.headers.find("x-asset-also");
                    if (alsoHeader != request.headers.end()) {
                        alsoList = alsoHeader->second;
                        for (const std::string& extra : splitCsv(alsoList)) {
                            if (validAssetSlot(extra) && extra != slot) aliases.push_back(extra);
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        for (const std::string& name : aliases) job->assets[name] = dest;
                    }
                    diagnostic("asset stored job=" + job->id + " slot=" + slot +
                               " also=" + alsoList +
                               " bytes=" + std::to_string(request.contentLength) +
                               " path=" + wideToUtf8(dest.wstring()));
                    sendJson(socket, 200, {{"ok", true}, {"slot", slot},
                                           {"bytes", request.contentLength}});
                    closesocket(socket);
                    return;
                }
                if (request.method == "POST" && parts.size() == 5 && parts[4] == "input") {
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        if (job->cancelled) {
                            sendJson(socket, 409, {{"message", job->message}, {"code", "cancelled"}});
                            closesocket(socket);
                            return;
                        }
                        if (job->status != "queued") {
                            sendJson(socket, 409, {{"message", "Job input is already available"}});
                            closesocket(socket);
                            return;
                        }
                        job->status = "receiving";
                        job->stage = "upload";
                    }
                    const bool stored = receiveUpload(socket, request, job, job->inputPath, "upload", true);
                    if (!stored) {
                        setJob(job, "failed", "upload", 100, "Upload stream completed with a transport message");
                        sendJson(socket, 400, jobJson(job));
                        closesocket(socket);
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->receivedBytes = request.contentLength;
                    }
                    setJob(job, "processing", "queued", 0);
                    launchJob(job);
                    sendJson(socket, 202, jobJson(job));
                    closesocket(socket);
                    return;
                }
                if (request.method == "GET" && parts.size() == 5 && parts[4] == "output") {
                    fs::path output;
                    {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        if (job->status != "completed" || job->outputPath.empty()) {
                            sendJson(socket, 409, {{"message", "Output is being prepared"}});
                            closesocket(socket);
                            return;
                        }
                        output = job->outputPath;
                    }
                    std::ifstream input(output, std::ios::binary);
                    if (!input) throw std::runtime_error("output open");
                    input.seekg(0, std::ios::end);
                    const std::streamsize size = input.tellg();
                    input.seekg(0, std::ios::beg);
                    std::ostringstream header;
                    header << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                           << "Content-Length: " << size << "\r\nConnection: close\r\n\r\n";
                    const std::string text = header.str();
                    sendAll(socket, text.data(), text.size());
                    std::array<char, 1024 * 1024> chunk{};
                    while (input) {
                        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                        const std::streamsize got = input.gcount();
                        if (got > 0 && !sendAll(socket, chunk.data(), static_cast<size_t>(got))) break;
                    }
                    closesocket(socket);
                    return;
                }
            }
            diagnostic("http 404 " + request.method + " " + request.path);
            sendJson(socket, 404, {{"message", "Hardcopy Companion API v1"}});
        } catch (const std::exception& error) {
            diagnostic(std::string("http 500 ") + request.method + " " + request.path +
                       " err=" + error.what());
            sendJson(socket, 500, {{"message", std::string("Companion request: ") + error.what()}});
        }
        closesocket(socket);
    }

    void acceptLoop() {
        while (running.load()) {
            sockaddr_in address{};
            int length = sizeof(address);
            SOCKET client = accept(listenSocket, reinterpret_cast<sockaddr*>(&address), &length);
            if (client == INVALID_SOCKET) {
                if (running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::lock_guard<std::mutex> lock(clientThreadsMutex);
            clientThreads.emplace_back([this, client] { handleClient(client); });
        }
    }

    void discoveryLoop() {
        std::array<char, 2048> buffer{};
        while (running.load()) {
            sockaddr_in sender{};
            int senderLength = sizeof(sender);
            const int got = recvfrom(discoverySocket, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                                     reinterpret_cast<sockaddr*>(&sender), &senderLength);
            if (got <= 0) continue;
            const std::string request(buffer.data(), static_cast<size_t>(got));
            if (request != "HARDCOPY_DISCOVER_V1") continue;
            char from[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sender.sin_addr, from, sizeof(from));
            diagnostic(std::string("discover from ") + from + ":" +
                       std::to_string(ntohs(sender.sin_port)) +
                       " pairing=" + (isPairingEnabled() ? "1" : "0"));
            const std::string reply = infoJson().dump();
            sendto(discoverySocket, reply.data(), static_cast<int>(reply.size()), 0,
                   reinterpret_cast<sockaddr*>(&sender), senderLength);
        }
    }

    bool ensureFirewall(bool promptElevation) {
        if (companionFirewallReady()) {
            firewallOk = true;
            firewallText = "Windows Firewall allows LAN TCP/UDP 53421";
            return true;
        }
        if (installCompanionFirewallRules()) {
            firewallOk = true;
            firewallText = "Windows Firewall rule added for LAN access";
            diagnostic("firewall rules added without elevation");
            return true;
        }
        if (promptElevation && !firewallElevateTried) {
            firewallElevateTried = true;
            saveSettings();
            diagnostic("firewall elevate prompt");
            if (launchElevatedFirewallSetup() && companionFirewallReady()) {
                firewallOk = true;
                firewallText = "Windows Firewall rule added for LAN access";
                diagnostic("firewall rules added after elevation");
                return true;
            }
        }
        firewallOk = false;
        firewallText = promptElevation
            ? "Windows Firewall is still blocking LAN access; use Allow LAN access"
            : "Windows Firewall is still blocking LAN access";
        diagnostic("firewall not ready elevated=" + std::string(processIsElevated() ? "1" : "0"));
        return false;
    }

    bool start() {
        if (running.load()) return true;
        ensureFirewall(true);
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
            statusText = "Companion network initialization message";
            diagnostic("listen WSAStartup failed " + winError(WSAGetLastError()));
            return false;
        }
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) { WSACleanup(); return false; }
        BOOL reuse = TRUE;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(CompanionServer::kPort);
        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
            diagnostic("listen bind/listen 53421 failed " + winError(WSAGetLastError()));
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
            WSACleanup();
            statusText = "Port 53421 is ready after the current listener releases it";
            return false;
        }
        discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (discoverySocket != INVALID_SOCKET) {
            setsockopt(discoverySocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            DWORD timeout = 500;
            setsockopt(discoverySocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            if (bind(discoverySocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
                diagnostic("discover udp bind failed " + winError(WSAGetLastError()));
                closesocket(discoverySocket);
                discoverySocket = INVALID_SOCKET;
            }
        }
        running.store(true);
        statusText = "Listening on TCP/UDP 53421";
        diagnostic(std::string("listen ok tcp=53421 udp=") +
                   (discoverySocket != INVALID_SOCKET ? "53421" : "fail") +
                   " id=" + id.substr(0, 8) +
                   " pairing=" + (isPairingEnabled() ? "1" : "0") +
                   " tokens=" + std::to_string(tokens.size()));
        acceptThread = std::thread([this] { acceptLoop(); });
        if (discoverySocket != INVALID_SOCKET)
            discoveryThread = std::thread([this] { discoveryLoop(); });
        return true;
    }

    void stop() {
        if (!running.exchange(false)) return;
        if (listenSocket != INVALID_SOCKET) {
            shutdown(listenSocket, SD_BOTH);
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }
        if (discoverySocket != INVALID_SOCKET) {
            closesocket(discoverySocket);
            discoverySocket = INVALID_SOCKET;
        }
        if (acceptThread.joinable()) acceptThread.join();
        if (discoveryThread.joinable()) discoveryThread.join();
        {
            std::lock_guard<std::mutex> lock(clientThreadsMutex);
            for (auto& thread : clientThreads) if (thread.joinable()) thread.join();
            clientThreads.clear();
        }
        {
            std::lock_guard<std::mutex> lock(workerThreadsMutex);
            for (auto& thread : workerThreads) if (thread.joinable()) thread.join();
            workerThreads.clear();
        }
        WSACleanup();
    }
};

CompanionServer::CompanionServer() : impl_(std::make_unique<Impl>()) {}
CompanionServer::~CompanionServer() = default;
bool CompanionServer::start() { return impl_->start(); }
void CompanionServer::stop() { impl_->stop(); }
void CompanionServer::enablePairing(int seconds) { impl_->pairingUntil.store(Impl::nowSeconds() + std::max(10, seconds)); }
bool CompanionServer::ensureFirewall(bool promptElevation) {
    impl_->firewallElevateTried = false;
    return impl_->ensureFirewall(promptElevation);
}
bool CompanionServer::installFirewallRules() { return installCompanionFirewallRules(); }
bool CompanionServer::running() const { return impl_->running.load(); }
bool CompanionServer::pairingEnabled() const { return impl_->isPairingEnabled(); }
bool CompanionServer::firewallReady() const { return impl_->firewallOk; }
int CompanionServer::activeJobs() const { return impl_->activeJobs.load(); }
std::string CompanionServer::deviceName() const { return impl_->name; }
std::string CompanionServer::deviceId() const { return impl_->id; }
std::string CompanionServer::status() const { return impl_->statusText; }
std::string CompanionServer::firewallStatus() const { return impl_->firewallText; }
void CompanionServer::setPcSettingsCommand(PcSettingsCommand command) {
    std::lock_guard<std::mutex> lock(impl_->pcSettingsMutex);
    impl_->pcSettingsCommand = std::move(command);
}

std::vector<CompanionServer::JobView> CompanionServer::jobViews() const {
    std::vector<std::shared_ptr<Impl::Job>> list;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        for (const auto& entry : impl_->jobs)
            if (entry.second->type == "convert") list.push_back(entry.second);
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a->seq > b->seq; });
    if (list.size() > 60) list.resize(60);
    std::vector<JobView> views;
    for (const auto& job : list) {
        std::lock_guard<std::mutex> lock(job->mutex);
        JobView v;
        v.id = job->id;
        v.fileName = job->fileName;
        v.batchId = job->batchId;
        v.batchIndex = job->batchIndex;
        v.batchCount = job->batchCount;
        v.status = job->status;
        v.stage = job->stage;
        v.message = job->message;
        v.progress = job->progress;
        v.cancelled = job->cancelled;
        views.push_back(std::move(v));
    }
    return views;
}

void CompanionServer::cancelJob(const std::string& jobId) {
    if (auto job = impl_->findJob(jobId)) impl_->cancelJob(job, "Cancelled on PC");
}

void CompanionServer::cancelBatch(const std::string& batchId) {
    impl_->cancelBatch(batchId);
}

std::vector<std::string> CompanionServer::recentJobs() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->recent;
}
