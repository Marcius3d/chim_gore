// CHIM Gore - SKSE plugin
//
// Watches deaths caused by the player or followers, asks Next-Gen Decapitations and
// Dismembering Framework what was severed, records each gory kill in CHIM's event log
// and, after combat has ended and a short delay has passed, asks one follower to react.
//
// No ESP/ESL is required. All integrations are optional: if a supported mod is missing,
// that part is skipped. Settings come from the CHIM web UI (chim_gore plugin page) when the
// server is reachable, otherwise from Data/SKSE/Plugins/CHIMGore.ini.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace logger = SKSE::log;
using namespace std::chrono_literals;

namespace
{
    constexpr const char* kPluginVersion = "0.2.1";
    constexpr const char* kNotifyPrefix = "CHIM-gore: ";
    constexpr const char* kNgdPlugin = "Next-Gen Decapitations.esp";
    constexpr const char* kDfPlugin = "Dismembering Framework.esm";
    constexpr const char* kChimScript = "AIAgentFunctions";
    constexpr float kUnitsPerMeter = 70.0f;  // Skyrim: ~1.43 cm per unit

    // ------------------------------------------------------------------------------------
    // Settings: INI defaults, overridden by the server plugin page when reachable.
    // Only read and written on the game thread.
    // ------------------------------------------------------------------------------------
    struct Settings
    {
        bool enabled{true};
        bool debug{false};
        bool useNgd{true};
        bool useDf{true};
        bool logEachEvent{true};
        bool reflectAfterCombat{true};
        bool includePlayerKills{true};
        float checkDelaySeconds{2.5f};
        float reflectDelayMinSeconds{8.0f};
        float reflectDelayMaxSeconds{20.0f};
        float followerRangeUnits{3000.0f};
        float headSearchRadiusUnits{2500.0f};
        int maxSummaryLines{3};
        int logLevel{2};  // 0 error, 1 warn, 2 info, 3 debug
    };

    struct ServerSettings
    {
        std::string host{"127.0.0.1"};
        int port{8081};
        std::string basePath{"/HerikaServer/ext/chim_gore"};
        bool fromIni{false};
    };

    Settings g_settings;
    ServerSettings g_server;
    std::atomic<bool> g_serverReachable{false};

    const char* kGoreIni = "Data\\SKSE\\Plugins\\CHIMGore.ini";

    std::string Trim(std::string s)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool ReadBool(const char* section, const char* key, bool def)
    {
        return GetPrivateProfileIntA(section, key, def ? 1 : 0, kGoreIni) != 0;
    }

    int ReadInt(const char* section, const char* key, int def)
    {
        return static_cast<int>(GetPrivateProfileIntA(section, key, def, kGoreIni));
    }

    float ReadFloat(const char* section, const char* key, float def)
    {
        char buffer[64]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), kGoreIni);
        try {
            return buffer[0] ? std::stof(buffer) : def;
        } catch (...) {
            return def;
        }
    }

    std::string ReadString(const char* section, const char* key)
    {
        char buffer[256]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), kGoreIni);
        return Trim(buffer);
    }

    void ClampSettings(Settings& s)
    {
        s.logLevel = std::clamp(s.logLevel, 0, 3);
        s.checkDelaySeconds = std::clamp(s.checkDelaySeconds, 0.5f, 10.0f);
        s.reflectDelayMinSeconds = std::clamp(s.reflectDelayMinSeconds, 0.0f, 600.0f);
        s.reflectDelayMaxSeconds = std::clamp(s.reflectDelayMaxSeconds, s.reflectDelayMinSeconds, 600.0f);
        s.followerRangeUnits = std::clamp(s.followerRangeUnits, 200.0f, 20000.0f);
        s.headSearchRadiusUnits = std::clamp(s.headSearchRadiusUnits, 200.0f, 8000.0f);
        s.maxSummaryLines = std::clamp(s.maxSummaryLines, 1, 6);
    }

    void LoadIniSettings()
    {
        auto& s = g_settings;
        s.enabled = ReadBool("General", "bEnabled", s.enabled);
        s.debug = ReadBool("General", "bDebug", ReadBool("General", "bDebugNotifications", s.debug));
        s.logEachEvent = ReadBool("General", "bLogEachEvent", s.logEachEvent);
        s.reflectAfterCombat = ReadBool("General", "bReflectAfterCombat", s.reflectAfterCombat);
        s.includePlayerKills = ReadBool("General", "bIncludePlayerKills", s.includePlayerKills);
        s.logLevel = ReadInt("General", "iLogLevel", s.logLevel);
        s.useNgd = ReadBool("Mods", "bUseNextGenDecapitations", s.useNgd);
        s.useDf = ReadBool("Mods", "bUseDismemberingFramework", s.useDf);
        s.checkDelaySeconds = ReadFloat("Timing", "fCheckDelaySeconds", s.checkDelaySeconds);
        s.reflectDelayMinSeconds = ReadFloat("Timing", "fReflectDelayMinSeconds", s.reflectDelayMinSeconds);
        s.reflectDelayMaxSeconds = ReadFloat("Timing", "fReflectDelayMaxSeconds", s.reflectDelayMaxSeconds);
        s.followerRangeUnits = ReadFloat("Range", "fFollowerRangeUnits", s.followerRangeUnits);
        s.headSearchRadiusUnits = ReadFloat("Range", "fHeadSearchRadiusUnits", s.headSearchRadiusUnits);
        s.maxSummaryLines = ReadInt("Text", "iMaxSummaryLines", s.maxSummaryLines);
        ClampSettings(s);

        const auto host = ReadString("Server", "sServer");
        const int port = ReadInt("Server", "iPort", 0);
        if (!host.empty()) {
            g_server.host = host;
            g_server.fromIni = true;
        }
        if (port > 0 && port <= 65535) {
            g_server.port = port;
        }
    }

    // ------------------------------------------------------------------------------------
    // Logging: Documents/My Games/Skyrim Special Edition/SKSE/chim-gore.log, plus an in-memory
    // tail that is sent to the server for the diagnostics page.
    // ------------------------------------------------------------------------------------
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_logTail;
    std::mutex g_logSentMutex;
    std::set<std::string> g_logSent;  // lines already delivered (bounded below)

    void SetupLog()
    {
        auto dist = std::make_shared<spdlog::sinks::dist_sink_mt>();
        if (auto dir = logger::log_directory()) {
            dist->add_sink(std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "chim-gore.log").string(), true));
        }
        g_logTail = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(300);
        dist->add_sink(g_logTail);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(dist));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    }

    void ApplyLogLevel()
    {
        static constexpr spdlog::level::level_enum levels[] = {
            spdlog::level::err, spdlog::level::warn, spdlog::level::info, spdlog::level::debug
        };
        const auto level = g_settings.debug ? spdlog::level::debug : levels[g_settings.logLevel];
        spdlog::set_level(level);
        spdlog::flush_on(level);
    }

    // ------------------------------------------------------------------------------------
    // On-screen notifications (shown where the HUD puts them; with SkyHUD top right).
    // ------------------------------------------------------------------------------------
    void Notify(const std::string& text, bool always = false)
    {
        auto show = [text, always]() {
            if (always || g_settings.debug) {
                RE::DebugNotification((std::string(kNotifyPrefix) + text).c_str());
            }
        };
        if (auto tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(show);
        }
    }

    // ------------------------------------------------------------------------------------
    // Tiny scheduler: runs callbacks on the game thread after a delay.
    // ------------------------------------------------------------------------------------
    class Scheduler
    {
    public:
        static Scheduler& Get()
        {
            static Scheduler instance;
            return instance;
        }

        void After(std::chrono::milliseconds delay, std::function<void()> fn)
        {
            {
                std::lock_guard lock(_mutex);
                _jobs.push_back({ std::chrono::steady_clock::now() + delay, std::move(fn) });
            }
            _cv.notify_one();
        }

        void Start()
        {
            if (_running.exchange(true)) {
                return;
            }
            std::thread([this]() { Loop(); }).detach();
        }

    private:
        struct Job
        {
            std::chrono::steady_clock::time_point due;
            std::function<void()> fn;
        };

        void Loop()
        {
            std::unique_lock lock(_mutex);
            while (_running) {
                _cv.wait_for(lock, 250ms);
                const auto now = std::chrono::steady_clock::now();
                std::vector<std::function<void()>> ready;
                for (auto it = _jobs.begin(); it != _jobs.end();) {
                    if (it->due <= now) {
                        ready.push_back(std::move(it->fn));
                        it = _jobs.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (ready.empty()) {
                    continue;
                }
                lock.unlock();
                if (auto tasks = SKSE::GetTaskInterface()) {
                    for (auto& fn : ready) {
                        tasks->AddTask([fn = std::move(fn)]() {
                            try {
                                fn();
                            } catch (const std::exception& e) {
                                logger::error("Scheduled task failed: {}", e.what());
                            }
                        });
                    }
                }
                lock.lock();
            }
        }

        std::mutex _mutex;
        std::condition_variable _cv;
        std::vector<Job> _jobs;
        std::atomic<bool> _running{false};
    };

    // ------------------------------------------------------------------------------------
    // Papyrus static calls with a result callback.
    // ------------------------------------------------------------------------------------
    class ResultCallback : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        explicit ResultCallback(std::function<void(const RE::BSScript::Variable&)> fn) :
            _fn(std::move(fn))
        {}

        void operator()(RE::BSScript::Variable a_result) override
        {
            if (_fn) {
                _fn(a_result);
            }
        }

        bool CanSave() const override { return false; }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        std::function<void(const RE::BSScript::Variable&)> _fn;
    };

    template <class... Args>
    bool CallStatic(const char* scriptName, const char* functionName,
        std::function<void(const RE::BSScript::Variable&)> onResult, Args&&... args)
    {
        auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        if (onResult) {
            callback.reset(new ResultCallback(std::move(onResult)));
        }
        auto arguments = RE::MakeFunctionArguments(std::forward<Args>(args)...);
        const bool ok = vm->DispatchStaticCall(scriptName, functionName, arguments, callback);
        if (!ok) {
            logger::warn("Papyrus call {}.{} could not be dispatched", scriptName, functionName);
        }
        return ok;
    }

    // ------------------------------------------------------------------------------------
    // Optional mod integration
    // ------------------------------------------------------------------------------------
    struct LimbNode
    {
        std::string node;
        std::string label;
        bool isHead{false};
    };

    struct Integrations
    {
        bool ngdLoaded{false};
        bool dfLoaded{false};
        std::string ngdVersion;
        std::string dfVersion;
        RE::TESObjectACTI* ngdRefActivator{nullptr};
        RE::BGSKeyword* ngdActorKeyword{nullptr};
        RE::BGSKeyword* ngdHeadKeyword{nullptr};
        std::vector<LimbNode> dfNodes;
    };

    Integrations g_mods;

    bool UseNgd() { return g_mods.ngdLoaded && g_settings.useNgd; }
    bool UseDf() { return g_mods.dfLoaded && g_settings.useDf; }

    std::string FileVersion(const char* path)
    {
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeA(path, &handle);
        if (size == 0) {
            return {};
        }
        std::vector<char> data(size);
        if (!GetFileVersionInfoA(path, 0, size, data.data())) {
            return {};
        }
        VS_FIXEDFILEINFO* info = nullptr;
        UINT len = 0;
        if (!VerQueryValueA(data.data(), "\\", reinterpret_cast<void**>(&info), &len) || !info) {
            return {};
        }
        return std::format("{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS));
    }

    std::string HumanizeNode(const std::string& node)
    {
        static const std::map<std::string, std::string> known = {
            { "NPC L Forearm [LLar]", "left forearm" },
            { "NPC R Forearm [RLar]", "right forearm" },
            { "NPC L Calf [LClf]", "left leg" },
            { "NPC R Calf [RClf]", "right leg" },
            { "NPC L UpperArm [LUar]", "left arm" },
            { "NPC R UpperArm [RUar]", "right arm" },
            { "NPC L Thigh [LThg]", "left thigh" },
            { "NPC R Thigh [RThg]", "right thigh" },
            { "NPC L Hand [LHnd]", "left hand" },
            { "NPC R Hand [RHnd]", "right hand" },
            { "NPC L Foot [Lft ]", "left foot" },
            { "NPC R Foot [Rft ]", "right foot" },
            { "TailBone02", "tail" },
        };
        if (auto it = known.find(node); it != known.end()) {
            return it->second;
        }
        std::string label = node;
        if (label.rfind("NPC ", 0) == 0) {
            label = label.substr(4);
        }
        if (auto bracket = label.find(" ["); bracket != std::string::npos) {
            label = label.substr(0, bracket);
        }
        if (label.rfind("L ", 0) == 0) {
            label = "left " + label.substr(2);
        } else if (label.rfind("R ", 0) == 0) {
            label = "right " + label.substr(2);
        }
        label = std::regex_replace(label, std::regex("([a-z])([A-Z0-9])"), "$1 $2");
        return ToLower(Trim(label));
    }

    // Collect the limb nodes used by installed Dismembering Framework packs (humanoid, creature, ...).
    void LoadDfNodes()
    {
        std::set<std::string> nodes = { "NPC L Forearm [LLar]", "NPC R Forearm [RLar]", "NPC L Calf [LClf]",
            "NPC R Calf [RClf]", "NPC Neck [Neck]", "TailBone02" };
        const std::filesystem::path dir = "Data/SKSE/DismemberingFramework";
        std::error_code ec;
        if (std::filesystem::is_directory(dir, ec)) {
            static const std::regex keyBeforeArray("\"([^\"]{2,64})\"\\s*:\\s*\\[");
            static const std::set<std::string> ignored = { "Conditions", "Translation", "Rotation", "Keywords" };
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                const auto name = entry.path().filename().string();
                if (entry.path().extension() != ".json" || name.find("_DATA") == std::string::npos ||
                    name.find("TEMPLATES") != std::string::npos) {
                    continue;
                }
                std::ifstream file(entry.path(), std::ios::binary);
                std::stringstream buffer;
                buffer << file.rdbuf();
                const std::string text = buffer.str();
                for (std::sregex_iterator it(text.begin(), text.end(), keyBeforeArray), end; it != end; ++it) {
                    const std::string key = (*it)[1].str();
                    if (!ignored.contains(key) && nodes.size() < 48) {
                        nodes.insert(key);
                    }
                }
            }
        }
        g_mods.dfNodes.clear();
        for (const auto& node : nodes) {
            const auto lower = ToLower(node);
            const bool head = lower.find("neck") != std::string::npos || lower.find("head") != std::string::npos;
            g_mods.dfNodes.push_back({ node, head ? "head" : HumanizeNode(node), head });
        }
        logger::info("Dismembering Framework: {} limb node(s) to check", g_mods.dfNodes.size());
    }

    // Regular and light (ESL-flagged) plugins live in separate lists; check both.
    bool IsPluginLoaded(RE::TESDataHandler* data, const char* name)
    {
        return data->LookupLoadedModByName(name) != nullptr || data->LookupLoadedLightModByName(name) != nullptr;
    }

    void DetectIntegrations()
    {
        auto data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            return;
        }
        // The Papyrus API comes from the SKSE DLLs, so a loaded DLL also counts.
        const bool ngdPlugin = IsPluginLoaded(data, kNgdPlugin);
        const bool ngdDll = GetModuleHandleA("NextGenDecapitations.dll") != nullptr;
        const bool dfPlugin = IsPluginLoaded(data, kDfPlugin);
        const bool dfDll = GetModuleHandleA("DismemberingFramework.dll") != nullptr;
        logger::info("Detection: {} plugin={} dll={}; {} plugin={} dll={}", kNgdPlugin, ngdPlugin, ngdDll, kDfPlugin, dfPlugin, dfDll);
        g_mods.ngdLoaded = ngdPlugin || ngdDll;
        g_mods.dfLoaded = dfPlugin || dfDll;
        if (g_mods.ngdLoaded) {
            g_mods.ngdActorKeyword = data->LookupForm<RE::BGSKeyword>(0x800, kNgdPlugin);
            g_mods.ngdHeadKeyword = data->LookupForm<RE::BGSKeyword>(0x801, kNgdPlugin);
            g_mods.ngdRefActivator = data->LookupForm<RE::TESObjectACTI>(0x802, kNgdPlugin);
            g_mods.ngdVersion = FileVersion("Data\\SKSE\\Plugins\\NextGenDecapitations.dll");
        }
        if (g_mods.dfLoaded) {
            g_mods.dfVersion = FileVersion("Data\\SKSE\\Plugins\\DismemberingFramework.dll");
            LoadDfNodes();
        }
        logger::info("Next-Gen Decapitations: {} {} (head forms {})", g_mods.ngdLoaded ? "found" : "not found", g_mods.ngdVersion,
            g_mods.ngdActorKeyword && g_mods.ngdHeadKeyword && g_mods.ngdRefActivator ? "ok" : "missing");
        logger::info("Dismembering Framework: {} {}", g_mods.dfLoaded ? "found" : "not found", g_mods.dfVersion);
    }

    std::string ActiveModsText()
    {
        std::vector<std::string> names;
        if (UseNgd()) {
            names.emplace_back("Next-Gen Decapitations");
        }
        if (UseDf()) {
            names.emplace_back("Dismembering Framework");
        }
        if (names.empty()) {
            return {};
        }
        return names.size() == 1 ? names[0] : names[0] + " + " + names[1];
    }

    // ------------------------------------------------------------------------------------
    // HTTP to the chim_gore server plugin (settings in, status and log tail out).
    // ------------------------------------------------------------------------------------
    std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (unsigned char c : in) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += std::format("\\u{:04x}", c);
                } else {
                    out += static_cast<char>(c);
                }
            }
        }
        return out;
    }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) {
            return {};
        }
        const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }

    bool HttpRequest(const std::string& host, int port, const std::string& method, const std::string& path,
        const std::string& body, std::string* response, int timeoutMs = 3000)
    {
        bool ok = false;
        HINTERNET session = WinHttpOpen(L"CHIMGore/0.2.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            return false;
        }
        WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
        HINTERNET connect = WinHttpConnect(session, Widen(host).c_str(), static_cast<INTERNET_PORT>(port), 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, Widen(method).c_str(), Widen(path).c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                                    : nullptr;
        if (request) {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            const BOOL sent = WinHttpSendRequest(request, body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers,
                body.empty() ? 0 : static_cast<DWORD>(-1L), body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
            if (sent && WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
                std::string data;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(request, &available) && available > 0 && data.size() < 65536) {
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                        break;
                    }
                    data.append(chunk.data(), read);
                }
                ok = status >= 200 && status < 300;
                if (response) {
                    *response = std::move(data);
                }
            }
            WinHttpCloseHandle(request);
        }
        if (connect) {
            WinHttpCloseHandle(connect);
        }
        WinHttpCloseHandle(session);
        return ok;
    }

    // Same discovery order as CHIM: AIAgent.ini, then CHIM.exe proxy on 7135, then 127.0.0.1:8081.
    void DiscoverServer()
    {
        if (g_server.fromIni) {
            logger::info("Server from CHIMGore.ini: {}:{}", g_server.host, g_server.port);
            return;
        }
        std::ifstream aiagent("Data/SKSE/Plugins/AIAgent.ini");
        if (aiagent.is_open()) {
            std::string line;
            while (std::getline(aiagent, line)) {
                const auto eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) {
                    continue;
                }
                const auto key = Trim(line.substr(0, eq));
                const auto value = Trim(line.substr(eq + 1));
                if (key == "SERVER" && !value.empty()) {
                    g_server.host = value;
                } else if (key == "PORT") {
                    try {
                        g_server.port = std::stoi(value);
                    } catch (...) {
                    }
                }
            }
            logger::info("Server from AIAgent.ini: {}:{}", g_server.host, g_server.port);
            return;
        }
        std::string body;
        if (HttpRequest("127.0.0.1", 7135, "GET", "/discover", {}, &body, 1500)) {
            body = Trim(body);
            const auto colon = body.find(':');
            if (colon != std::string::npos) {
                g_server.host = body.substr(0, colon);
                try {
                    g_server.port = std::stoi(body.substr(colon + 1));
                } catch (...) {
                }
                logger::info("Server from CHIM launcher discovery: {}:{}", g_server.host, g_server.port);
                return;
            }
        }
        logger::info("Server: default {}:{}", g_server.host, g_server.port);
    }

    // Counters reported to the server (atomic: touched from several threads).
    std::atomic<int> g_statKills{0};
    std::atomic<int> g_statGore{0};
    std::atomic<int> g_statLogged{0};
    std::atomic<int> g_statFailures{0};
    std::atomic<int> g_statReactions{0};
    std::mutex g_lastErrorMutex;
    std::string g_lastError;

    void SetLastError(const std::string& error)
    {
        std::lock_guard lock(g_lastErrorMutex);
        g_lastError = error;
        ++g_statFailures;
    }

    void ApplyServerConfig(const std::string& text)
    {
        std::map<std::string, std::string> values;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) {
                values[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
            }
        }
        if (values["plugin"] != "chim_gore") {
            return;
        }
        auto b = [&](const char* key, bool& target) {
            if (values.contains(key)) target = values[key] == "1";
        };
        auto f = [&](const char* key, float& target) {
            try {
                if (values.contains(key)) target = std::stof(values[key]);
            } catch (...) {
            }
        };
        auto i = [&](const char* key, int& target) {
            try {
                if (values.contains(key)) target = std::stoi(values[key]);
            } catch (...) {
            }
        };
        Settings s = g_settings;
        b("enabled", s.enabled);
        b("debug", s.debug);
        b("use_ngd", s.useNgd);
        b("use_df", s.useDf);
        b("log_each_event", s.logEachEvent);
        b("reflect_after_combat", s.reflectAfterCombat);
        b("include_player_kills", s.includePlayerKills);
        f("check_delay_seconds", s.checkDelaySeconds);
        f("reflect_delay_min_seconds", s.reflectDelayMinSeconds);
        f("reflect_delay_max_seconds", s.reflectDelayMaxSeconds);
        f("follower_range_units", s.followerRangeUnits);
        i("max_summary_lines", s.maxSummaryLines);
        ClampSettings(s);
        const bool debugChanged = s.debug != g_settings.debug;
        g_settings = s;
        if (debugChanged) {
            ApplyLogLevel();
            logger::info("Debug mode {}", s.debug ? "on" : "off");
        }
    }

    std::string BuildStatusJson()
    {
        std::vector<std::string> fresh;
        if (g_logTail) {
            std::lock_guard lock(g_logSentMutex);
            for (auto& line : g_logTail->last_formatted()) {
                auto trimmed = Trim(line);
                if (!trimmed.empty() && !g_logSent.contains(trimmed)) {
                    fresh.push_back(trimmed);
                }
            }
        }
        std::string lines;
        for (auto& line : fresh) {
            if (!lines.empty()) {
                lines += ",";
            }
            lines += "\"" + JsonEscape(line.substr(0, 500)) + "\"";
        }
        std::string lastError;
        {
            std::lock_guard lock(g_lastErrorMutex);
            lastError = g_lastError;
        }
        const auto runtime = REL::Module::get().version().string(".");
        return std::format(
            "{{\"plugin\":\"chim_gore\",\"version\":\"{}\",\"game_version\":\"{}\",\"server_from\":\"{}\","
            "\"ngd_loaded\":{},\"ngd_version\":\"{}\",\"ngd_forms_ok\":{},\"df_loaded\":{},\"df_version\":\"{}\",\"df_nodes\":{},"
            "\"enabled\":{},\"debug\":{},\"kills_seen\":{},\"gore_events\":{},\"logged_to_chim\":{},\"failures\":{},"
            "\"reactions_requested\":{},\"last_error\":\"{}\",\"log_lines\":[{}]}}",
            kPluginVersion, JsonEscape(runtime), g_server.fromIni ? "CHIMGore.ini" : "auto",
            g_mods.ngdLoaded, JsonEscape(g_mods.ngdVersion),
            g_mods.ngdActorKeyword && g_mods.ngdHeadKeyword && g_mods.ngdRefActivator, g_mods.dfLoaded,
            JsonEscape(g_mods.dfVersion), g_mods.dfNodes.size(), g_settings.enabled, g_settings.debug,
            g_statKills.load(), g_statGore.load(), g_statLogged.load(), g_statFailures.load(), g_statReactions.load(),
            JsonEscape(lastError), lines);
    }

    void MarkLinesSent(const std::string& json)
    {
        if (!g_logTail) {
            return;
        }
        std::lock_guard lock(g_logSentMutex);
        for (auto& line : g_logTail->last_formatted()) {
            auto trimmed = Trim(line);
            if (json.find(JsonEscape(trimmed.substr(0, 500))) != std::string::npos) {
                g_logSent.insert(trimmed);
            }
        }
        if (g_logSent.size() > 2000) {
            g_logSent.clear();  // ring buffer is only 300 lines; old entries can be forgotten
            for (auto& line : g_logTail->last_formatted()) {
                g_logSent.insert(Trim(line));
            }
        }
    }

    class NetWorker
    {
    public:
        static NetWorker& Get()
        {
            static NetWorker instance;
            return instance;
        }

        void Start()
        {
            if (_running.exchange(true)) {
                return;
            }
            std::thread([this]() { Loop(); }).detach();
        }

        void Wake()
        {
            _wake = true;
            _cv.notify_one();
        }

    private:
        void Loop()
        {
            DiscoverServer();
            while (_running) {
                SyncOnce();
                std::unique_lock lock(_mutex);
                _cv.wait_for(lock, 30s, [this]() { return _wake.load(); });
                _wake = false;
            }
        }

        void SyncOnce()
        {
            std::string config;
            const bool configOk = HttpRequest(g_server.host, g_server.port, "GET", g_server.basePath + "/api/config.php", {}, &config);
            if (configOk) {
                if (auto tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([config]() { ApplyServerConfig(config); });
                }
            }
            if (configOk != g_serverReachable.exchange(configOk)) {
                if (configOk) {
                    logger::info("chim_gore server plugin reachable at {}:{}", g_server.host, g_server.port);
                } else {
                    logger::info("chim_gore server plugin not reachable at {}:{} (using CHIMGore.ini settings)", g_server.host, g_server.port);
                }
            }
            if (!configOk) {
                return;
            }
            // Status is built on the game thread so settings and integrations are read safely.
            auto promise = std::make_shared<std::promise<std::string>>();
            auto future = promise->get_future();
            if (auto tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([promise]() { promise->set_value(BuildStatusJson()); });
            } else {
                return;
            }
            if (future.wait_for(5s) != std::future_status::ready) {
                return;
            }
            const auto json = future.get();
            if (HttpRequest(g_server.host, g_server.port, "POST", g_server.basePath + "/api/status.php", json, nullptr)) {
                MarkLinesSent(json);
            }
        }

        std::mutex _mutex;
        std::condition_variable _cv;
        std::atomic<bool> _running{false};
        std::atomic<bool> _wake{false};
    };

    // ------------------------------------------------------------------------------------
    // Gore events
    // ------------------------------------------------------------------------------------
    struct GoreEvent
    {
        RE::ActorHandle killer;
        std::string killerName;
        bool killerIsPlayer{false};
        std::string victimName;
        bool victimUnique{false};
        std::string weaponName;
        bool killMove{false};
        bool decapitated{false};
        float headDistanceMeters{-1.0f};
        std::vector<std::string> limbs;
        int score{0};
        std::string sentence;
    };

    std::string VictimPhrase(const GoreEvent& e)
    {
        return e.victimUnique ? e.victimName : "the " + e.victimName;
    }

    std::string Possessive(const std::string& name)
    {
        if (!name.empty() && (name.back() == 's' || name.back() == 'S')) {
            return name + "'";
        }
        return name + "'s";
    }

    std::string JoinList(const std::vector<std::string>& items)
    {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) {
                out += (i + 1 == items.size()) ? " and " : ", ";
            }
            out += items[i];
        }
        return out;
    }

    void BuildSentence(GoreEvent& e)
    {
        const std::string victim = VictimPhrase(e);
        std::string how;
        if (!e.weaponName.empty()) {
            how += " with " + e.weaponName;
        }
        if (e.killMove) {
            how += " in a finishing move";
        }

        std::vector<std::string> parts;
        if (e.decapitated) {
            std::string s = std::format("{} cut off {} head{}", e.killerName, Possessive(victim), how);
            if (e.headDistanceMeters >= 1.0f) {
                s += std::format("; the severed head landed about {} meters away", static_cast<int>(std::lround(e.headDistanceMeters)));
            } else if (e.headDistanceMeters >= 0.0f) {
                s += "; the severed head dropped right beside the body";
            }
            parts.push_back(s);
        }
        if (!e.limbs.empty()) {
            if (e.decapitated) {
                parts.push_back(std::format("the body also lost its {}", JoinList(e.limbs)));
            } else {
                parts.push_back(std::format("{} severed {} {}{}", e.killerName, Possessive(victim), JoinList(e.limbs), how));
            }
        }
        e.sentence.clear();
        for (size_t i = 0; i < parts.size(); ++i) {
            e.sentence += (i == 0 ? "" : ", and ") + parts[i];
        }
        if (!e.sentence.empty()) {
            e.sentence += ".";
        }
    }

    void ScoreEvent(GoreEvent& e)
    {
        int score = 0;
        if (e.decapitated) {
            score += 6;
            if (e.headDistanceMeters > 0.0f) {
                score += static_cast<int>(std::min(e.headDistanceMeters, 10.0f) / 2.0f);
            }
        }
        score += 2 * static_cast<int>(e.limbs.size());
        if (e.killMove) {
            score += 1;
        }
        e.score = score;
    }

    // Collected during one fight, consumed after combat.
    struct FightState
    {
        std::vector<GoreEvent> events;
        bool watching{false};
        bool quietScheduled{false};
        std::uint64_t generation{0};
    };

    std::mutex g_fightMutex;
    FightState g_fight;
    std::atomic<std::uint64_t> g_sessionGeneration{0};

    std::mutex g_seenMutex;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_seenVictims;

    std::mt19937& Rng()
    {
        static std::mt19937 rng{ std::random_device{}() };
        return rng;
    }

    // ------------------------------------------------------------------------------------
    // Sending to CHIM
    // ------------------------------------------------------------------------------------
    void LogToChim(const GoreEvent& event)
    {
        if (!g_settings.logEachEvent) {
            Notify("request successful (event memory disabled)");
            return;
        }
        // "info_*" events are stored in CHIM's event log and shown to nearby NPCs as context.
        const std::string victim = event.victimName;
        const bool dispatched = CallStatic(kChimScript, "logMessage",
            [victim](const RE::BSScript::Variable& result) {
                if (result.IsInt() && result.GetSInt() == 0) {
                    ++g_statLogged;
                    logger::info("CHIM accepted the event for {}", victim);
                    Notify("request successful");
                } else {
                    SetLastError("CHIM logMessage returned an unexpected result");
                    logger::warn("CHIM logMessage returned an unexpected result for {}", victim);
                    Notify("request failed (CHIM did not accept the event)");
                }
            },
            std::string(event.sentence), std::string("info_gore"));
        if (!dispatched) {
            SetLastError("CHIM not reachable (AIAgentFunctions.logMessage)");
            Notify("request failed (CHIM not reachable)");
        }
    }

    float DistanceToPlayer(RE::Actor* actor)
    {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !actor) {
            return 1e9f;
        }
        return player->GetPosition().GetDistance(actor->GetPosition());
    }

    bool IsUsableFollower(RE::Actor* actor)
    {
        return actor && !actor->IsPlayerRef() && actor->IsPlayerTeammate() && !actor->IsDead() &&
               !actor->IsDisabled() && actor->Is3DLoaded() &&
               DistanceToPlayer(actor) <= g_settings.followerRangeUnits;
    }

    std::vector<std::string> PickCandidates(const std::vector<GoreEvent>& events)
    {
        std::vector<std::string> names;
        std::unordered_set<RE::FormID> used;

        // 1) Followers who did the cutting, best moment first.
        std::vector<const GoreEvent*> sorted;
        for (auto& e : events) {
            sorted.push_back(&e);
        }
        std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return a->score > b->score; });
        for (auto* e : sorted) {
            auto killer = e->killer.get();
            if (killer && IsUsableFollower(killer.get()) && used.insert(killer->GetFormID()).second) {
                names.emplace_back(killer->GetDisplayFullName());
            }
        }

        // 2) Any other nearby follower, nearest first.
        std::vector<std::pair<float, RE::Actor*>> others;
        if (auto lists = RE::ProcessLists::GetSingleton()) {
            for (auto& handle : lists->highActorHandles) {
                auto actor = handle.get();
                if (actor && IsUsableFollower(actor.get()) && !used.contains(actor->GetFormID())) {
                    others.emplace_back(DistanceToPlayer(actor.get()), actor.get());
                }
            }
        }
        std::sort(others.begin(), others.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (auto& [distance, actor] : others) {
            if (used.insert(actor->GetFormID()).second) {
                names.emplace_back(actor->GetDisplayFullName());
            }
        }

        if (names.size() > 3) {
            names.resize(3);
        }
        return names;
    }

    std::string BuildReflectMessage(std::vector<GoreEvent> events)
    {
        std::sort(events.begin(), events.end(), [](auto& a, auto& b) { return a.score > b.score; });
        int heads = 0;
        int limbs = 0;
        int best = 0;
        for (auto& e : events) {
            heads += e.decapitated ? 1 : 0;
            limbs += static_cast<int>(e.limbs.size());
            best = std::max(best, e.score);
        }
        std::string lines;
        const int count = std::min<int>(g_settings.maxSummaryLines, static_cast<int>(events.size()));
        for (int i = 0; i < count; ++i) {
            if (!lines.empty()) {
                lines += " ";
            }
            lines += events[i].sentence;
        }
        // The bracketed marker lets the chim_gore server plugin apply its chance/cooldown policy and
        // rewrite this into the configured instruction. Without the server plugin it still reads fine.
        return std::format(
            "[chim_gore score={} heads={} limbs={} events={}] The fight is over. Briefly react to the most gruesome moment of it, in character. What happened: {}",
            best, heads, limbs, static_cast<int>(events.size()), lines);
    }

    void TryReflect(std::shared_ptr<std::vector<std::string>> candidates, size_t index, std::string message)
    {
        if (index >= candidates->size()) {
            logger::info("No follower accepted the reaction request");
            Notify("reaction request failed (no eligible follower)");
            return;
        }
        const std::string name = (*candidates)[index];
        logger::info("Asking {} to react to the fight", name);
        Notify(std::format("reaction request sent ({})", name));
        const bool dispatched = CallStatic(
            kChimScript, "requestMessageForEligibleActor",
            [candidates, index, message, name](const RE::BSScript::Variable& result) {
                const int code = result.IsInt() ? result.GetSInt() : -99;
                if (code == 1) {
                    ++g_statReactions;
                    logger::info("CHIM queued the reaction for {}", name);
                    Notify("reaction request successful (the server decides if it is spoken)");
                    return;
                }
                logger::info("CHIM did not queue the reaction for {} (code {}), trying next", name, code);
                if (auto tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([candidates, index, message]() { TryReflect(candidates, index + 1, message); });
                }
            },
            std::string(message), std::string("instruction"), std::string(name));
        if (!dispatched) {
            SetLastError("CHIM not reachable (requestMessageForEligibleActor)");
            Notify("reaction request failed (CHIM not reachable)");
        }
    }

    void FinishFight(std::uint64_t generation)
    {
        std::vector<GoreEvent> events;
        {
            std::lock_guard lock(g_fightMutex);
            if (generation != g_fight.generation || g_fight.events.empty()) {
                return;
            }
            events = std::move(g_fight.events);
            g_fight.events.clear();
            g_fight.watching = false;
            g_fight.quietScheduled = false;
            ++g_fight.generation;
        }
        if (!g_settings.enabled || !g_settings.reflectAfterCombat) {
            return;
        }
        auto candidates = std::make_shared<std::vector<std::string>>(PickCandidates(events));
        if (candidates->empty()) {
            logger::info("Fight ended with {} gore event(s) but no follower is nearby", events.size());
            Notify("fight over, no follower nearby to react");
            return;
        }
        TryReflect(candidates, 0, BuildReflectMessage(std::move(events)));
    }

    void WatchCombat(std::uint64_t generation);

    void ScheduleWatch(std::uint64_t generation, std::chrono::milliseconds delay)
    {
        Scheduler::Get().After(delay, [generation]() { WatchCombat(generation); });
    }

    // Poll the player's combat state once per second while a fight has gore events.
    void WatchCombat(std::uint64_t generation)
    {
        if (generation != g_sessionGeneration.load()) {
            return;
        }
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        std::uint64_t fightGeneration = 0;
        bool scheduleQuiet = false;
        {
            std::lock_guard lock(g_fightMutex);
            if (g_fight.events.empty()) {
                g_fight.watching = false;
                return;
            }
            if (player->IsInCombat()) {
                if (g_fight.quietScheduled) {
                    ++g_fight.generation;  // combat resumed: the pending timer becomes stale
                    g_fight.quietScheduled = false;
                }
            } else if (!g_fight.quietScheduled) {
                g_fight.quietScheduled = true;
                fightGeneration = g_fight.generation;
                scheduleQuiet = true;
            }
        }
        if (scheduleQuiet) {
            std::uniform_real_distribution<float> dist(g_settings.reflectDelayMinSeconds, g_settings.reflectDelayMaxSeconds);
            const auto delay = std::chrono::milliseconds(static_cast<int>(dist(Rng()) * 1000.0f));
            logger::info("Combat ended; reaction in {} ms unless combat resumes", delay.count());
            Scheduler::Get().After(delay, [generation, fightGeneration]() {
                if (generation != g_sessionGeneration.load()) {
                    return;
                }
                auto player = RE::PlayerCharacter::GetSingleton();
                if (player && player->IsInCombat()) {
                    return;  // the watcher resets and reschedules
                }
                FinishFight(fightGeneration);
            });
        }
        ScheduleWatch(generation, 1000ms);
    }

    void AddEventToFight(GoreEvent event)
    {
        bool startWatch = false;
        {
            std::lock_guard lock(g_fightMutex);
            g_fight.events.push_back(std::move(event));
            if (g_fight.events.size() > 30) {
                g_fight.events.erase(g_fight.events.begin());
            }
            if (!g_fight.watching) {
                g_fight.watching = true;
                startWatch = true;
            }
        }
        if (startWatch) {
            ScheduleWatch(g_sessionGeneration.load(), 1000ms);
        }
    }

    // ------------------------------------------------------------------------------------
    // Inspecting a fresh corpse
    // ------------------------------------------------------------------------------------
    float FindHeadDistanceMeters(RE::Actor* victim)
    {
        if (!g_mods.ngdRefActivator || !g_mods.ngdActorKeyword || !g_mods.ngdHeadKeyword) {
            return -1.0f;
        }
        auto tes = RE::TES::GetSingleton();
        if (!tes) {
            return -1.0f;
        }
        float meters = -1.0f;
        tes->ForEachReferenceInRange(victim, g_settings.headSearchRadiusUnits, [&](RE::TESObjectREFR& candidate) {
            RE::TESObjectREFR* ref = &candidate;
            if (ref->GetBaseObject() != g_mods.ngdRefActivator) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (ref->GetLinkedRef(g_mods.ngdActorKeyword) != victim) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (auto head = ref->GetLinkedRef(g_mods.ngdHeadKeyword)) {
                meters = head->GetPosition().GetDistance(victim->GetPosition()) / kUnitsPerMeter;
            }
            return RE::BSContainer::ForEachResult::kStop;
        });
        return meters;
    }

    struct PendingInspection
    {
        std::mutex mutex;
        GoreEvent event;
        int outstanding{0};
        bool finalized{false};
        bool papyrusDecapitated{false};
        std::vector<std::string> failures;
        std::uint64_t generation{0};
        RE::ActorHandle victim;
    };

    void PublishEvent(GoreEvent event)
    {
        ScoreEvent(event);
        BuildSentence(event);
        ++g_statGore;
        logger::info("Gore event (score {}): {}", event.score, event.sentence);
        LogToChim(event);
        AddEventToFight(std::move(event));
    }

    // The head may still be rolling: sample its distance a few times and keep the largest.
    void SampleHead(RE::ActorHandle victimHandle, GoreEvent event, int samplesLeft, std::uint64_t generation)
    {
        if (generation != g_sessionGeneration.load()) {
            return;
        }
        if (auto victim = victimHandle.get()) {
            event.headDistanceMeters = std::max(event.headDistanceMeters, FindHeadDistanceMeters(victim.get()));
        }
        if (samplesLeft > 0) {
            Scheduler::Get().After(1200ms, [victimHandle, event, samplesLeft, generation]() {
                SampleHead(victimHandle, event, samplesLeft - 1, generation);
            });
            return;
        }
        if (event.headDistanceMeters >= 0.0f) {
            event.decapitated = true;
        }
        if (!event.decapitated && event.limbs.empty()) {
            logger::debug("{} died without dismemberment", event.victimName);
            Notify("nothing severed");
            return;
        }
        PublishEvent(std::move(event));
    }

    void FinalizeInspection(const std::shared_ptr<PendingInspection>& pending, bool timedOut)
    {
        GoreEvent event;
        std::vector<std::string> failures;
        {
            std::lock_guard lock(pending->mutex);
            if (pending->finalized) {
                return;
            }
            pending->finalized = true;
            event = pending->event;
            event.decapitated = event.decapitated || pending->papyrusDecapitated;
            failures = pending->failures;
        }
        if (pending->generation != g_sessionGeneration.load()) {
            return;
        }
        if (timedOut) {
            failures.emplace_back("no answer from Papyrus");
        }
        if (!failures.empty()) {
            const auto reason = JoinList(failures);
            SetLastError(reason);
            logger::warn("Request for {} partly failed: {}", event.victimName, reason);
            Notify(std::format("request failed ({})", reason));
        }

        const bool headPossible = UseNgd() && (event.decapitated || g_mods.ngdRefActivator);
        if (headPossible) {
            SampleHead(pending->victim, std::move(event), 2, pending->generation);
            return;
        }
        if (!event.decapitated && event.limbs.empty()) {
            logger::debug("{} died without dismemberment", event.victimName);
            Notify("nothing severed");
            return;
        }
        PublishEvent(std::move(event));
    }

    void CompleteOne(const std::shared_ptr<PendingInspection>& pending)
    {
        bool done = false;
        {
            std::lock_guard lock(pending->mutex);
            done = --pending->outstanding == 0;
        }
        if (done) {
            if (auto tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([pending]() { FinalizeInspection(pending, false); });
            }
        }
    }

    void InspectVictim(RE::ActorHandle victimHandle, GoreEvent base, std::uint64_t generation)
    {
        if (generation != g_sessionGeneration.load()) {
            return;
        }
        auto victim = victimHandle.get();
        if (!victim) {
            return;
        }
        auto pending = std::make_shared<PendingInspection>();
        pending->event = std::move(base);
        pending->generation = generation;
        pending->victim = victimHandle;
        pending->outstanding = 1;  // dispatch slot, released at the end

        auto add = [&](const std::string& what, auto&& dispatch) {
            {
                std::lock_guard lock(pending->mutex);
                ++pending->outstanding;
            }
            if (!dispatch()) {
                {
                    std::lock_guard lock(pending->mutex);
                    pending->failures.push_back(what + " not reachable");
                }
                CompleteOne(pending);
            }
        };

        std::vector<std::string> asked;
        if (UseNgd()) {
            asked.emplace_back("Next-Gen Decapitations");
            RE::Actor* target = victim.get();
            add("Next-Gen Decapitations", [&]() {
                return CallStatic("NGDecapitations", "IsDecapitated",
                    [pending](const RE::BSScript::Variable& r) {
                        if (r.IsBool() && r.GetBool()) {
                            std::lock_guard lock(pending->mutex);
                            pending->papyrusDecapitated = true;
                        }
                        CompleteOne(pending);
                    },
                    std::move(target));
            });
        }
        if (UseDf()) {
            asked.emplace_back("Dismembering Framework");
            for (const auto& limb : g_mods.dfNodes) {
                RE::Actor* target = victim.get();
                const LimbNode node = limb;
                add("Dismembering Framework", [&]() {
                    return CallStatic("DismemberingFramework", "IsDismemberedNode",
                        [pending, node](const RE::BSScript::Variable& r) {
                            if (r.IsBool() && r.GetBool()) {
                                std::lock_guard lock(pending->mutex);
                                if (node.isHead) {
                                    pending->event.decapitated = true;
                                } else if (std::find(pending->event.limbs.begin(), pending->event.limbs.end(), node.label) == pending->event.limbs.end()) {
                                    pending->event.limbs.push_back(node.label);
                                }
                            }
                            CompleteOne(pending);
                        },
                        std::move(target), std::string(node.node));
                });
            }
        }
        logger::debug("Checking {} with {}", pending->event.victimName, JoinList(asked));
        Notify(std::format("request sent ({})", JoinList(asked)));

        // Watchdog: if Papyrus never answers, report it instead of waiting forever.
        Scheduler::Get().After(5000ms, [pending]() { FinalizeInspection(pending, true); });
        CompleteOne(pending);  // release the dispatch slot
    }

    bool AlreadySeen(RE::FormID id)
    {
        std::lock_guard lock(g_seenMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = g_seenVictims.begin(); it != g_seenVictims.end();) {
            it = (now - it->second > 5min) ? g_seenVictims.erase(it) : std::next(it);
        }
        return !g_seenVictims.emplace(id, now).second;
    }

    class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static DeathSink* Get()
        {
            static DeathSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            if (!a_event || !g_settings.enabled || (!UseNgd() && !UseDf())) {
                return RE::BSEventNotifyControl::kContinue;
            }
            auto victim = a_event->actorDying ? a_event->actorDying->As<RE::Actor>() : nullptr;
            auto killer = a_event->actorKiller ? a_event->actorKiller->As<RE::Actor>() : nullptr;
            if (!victim || !killer || victim->IsPlayerRef() || victim == killer) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const bool killerIsPlayer = killer->IsPlayerRef();
            if (killerIsPlayer ? !g_settings.includePlayerKills : !killer->IsPlayerTeammate()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (AlreadySeen(victim->GetFormID())) {
                return RE::BSEventNotifyControl::kContinue;
            }
            ++g_statKills;

            GoreEvent base;
            base.killer = killer->GetHandle();
            base.killerName = killer->GetDisplayFullName();
            base.killerIsPlayer = killerIsPlayer;
            base.victimName = victim->GetDisplayFullName();
            if (auto npc = victim->GetActorBase()) {
                base.victimUnique = npc->IsUnique();
            }
            if (auto weapon = killer->GetEquippedObject(false)) {
                if (auto w = weapon->As<RE::TESObjectWEAP>()) {
                    const char* name = w->GetName();
                    if (name && name[0] != '\0') {
                        base.weaponName = name;
                    }
                }
            }
            base.killMove = victim->IsInKillMove() || killer->IsInKillMove();
            logger::debug("{} killed {}", base.killerName, base.victimName);

            const auto generation = g_sessionGeneration.load();
            auto victimHandle = victim->GetHandle();
            const auto delay = std::chrono::milliseconds(static_cast<int>(g_settings.checkDelaySeconds * 1000.0f));
            Scheduler::Get().After(delay, [victimHandle, base, generation]() {
                InspectVictim(victimHandle, base, generation);
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void ResetSession()
    {
        ++g_sessionGeneration;
        {
            std::lock_guard lock(g_fightMutex);
            g_fight = FightState{};
        }
        {
            std::lock_guard lock(g_seenMutex);
            g_seenVictims.clear();
        }
    }

    void AnnounceActivation()
    {
        if (!g_settings.enabled) {
            Notify("disabled", true);
            return;
        }
        const auto mods = ActiveModsText();
        if (mods.empty()) {
            Notify("activated, but no supported gore mod is active", true);
        } else {
            Notify(std::format("activated ({})", mods), true);
        }
        logger::info("Activated for this session: {}", mods.empty() ? "no gore mod" : mods);
        NetWorker::Get().Wake();
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            DetectIntegrations();
            if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESDeathEvent>(DeathSink::Get());
            }
            Scheduler::Get().Start();
            NetWorker::Get().Start();
            logger::info("Ready");
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ResetSession();
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
            Scheduler::Get().After(3000ms, []() { AnnounceActivation(); });
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            Scheduler::Get().After(3000ms, []() { AnnounceActivation(); });
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse);
    SetupLog();
    LoadIniSettings();
    ApplyLogLevel();
    logger::info("CHIM Gore {} loading (enabled={}, debug={})", kPluginVersion, g_settings.enabled, g_settings.debug);
    if (auto messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }
    return true;
}
