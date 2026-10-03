// CHIM Gore - SKSE plugin
//
// Watches deaths caused by the player or followers, asks Next-Gen Decapitations and
// Dismembering Framework what was severed, records each gory kill in CHIM's event log
// and, after combat has ended and a short delay has passed, asks one follower to react.
//
// No ESP/ESL is required. All integrations are optional: if a supported mod is missing,
// that part is skipped silently.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
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

namespace logger = SKSE::log;
using namespace std::chrono_literals;

namespace
{
    constexpr const char* kPluginName = "CHIMGore";
    constexpr const char* kNgdPlugin = "Next-Gen Decapitations.esp";
    constexpr const char* kDfPlugin = "Dismembering Framework.esm";
    constexpr const char* kChimScript = "AIAgentFunctions";
    constexpr float kUnitsPerMeter = 70.0f;  // Skyrim: ~1.43 cm per unit

    // ------------------------------------------------------------------------------------
    // Settings (Data/SKSE/Plugins/CHIMGore.ini)
    // ------------------------------------------------------------------------------------
    struct Settings
    {
        bool enabled{true};
        bool logEachEvent{true};
        bool reflectAfterCombat{true};
        bool includePlayerKills{true};
        bool debugNotifications{false};
        float checkDelaySeconds{2.5f};
        float reflectDelayMinSeconds{8.0f};
        float reflectDelayMaxSeconds{20.0f};
        float followerRangeUnits{3000.0f};
        float headSearchRadiusUnits{2500.0f};
        int maxSummaryLines{3};
        int logLevel{2};  // 0 error, 1 warn, 2 info, 3 debug
    };

    Settings g_settings;

    std::string IniPath()
    {
        return "Data\\SKSE\\Plugins\\CHIMGore.ini";
    }

    bool ReadBool(const char* section, const char* key, bool def)
    {
        return GetPrivateProfileIntA(section, key, def ? 1 : 0, IniPath().c_str()) != 0;
    }

    int ReadInt(const char* section, const char* key, int def)
    {
        return static_cast<int>(GetPrivateProfileIntA(section, key, def, IniPath().c_str()));
    }

    float ReadFloat(const char* section, const char* key, float def)
    {
        char buffer[64]{};
        GetPrivateProfileStringA(section, key, "", buffer, sizeof(buffer), IniPath().c_str());
        if (buffer[0] == '\0') {
            return def;
        }
        try {
            return std::stof(buffer);
        } catch (...) {
            return def;
        }
    }

    void LoadSettings()
    {
        auto& s = g_settings;
        s.enabled = ReadBool("General", "bEnabled", s.enabled);
        s.logEachEvent = ReadBool("General", "bLogEachEvent", s.logEachEvent);
        s.reflectAfterCombat = ReadBool("General", "bReflectAfterCombat", s.reflectAfterCombat);
        s.includePlayerKills = ReadBool("General", "bIncludePlayerKills", s.includePlayerKills);
        s.debugNotifications = ReadBool("General", "bDebugNotifications", s.debugNotifications);
        s.logLevel = std::clamp(ReadInt("General", "iLogLevel", s.logLevel), 0, 3);

        s.checkDelaySeconds = std::clamp(ReadFloat("Timing", "fCheckDelaySeconds", s.checkDelaySeconds), 0.5f, 10.0f);
        s.reflectDelayMinSeconds = std::clamp(ReadFloat("Timing", "fReflectDelayMinSeconds", s.reflectDelayMinSeconds), 0.0f, 600.0f);
        s.reflectDelayMaxSeconds = std::clamp(ReadFloat("Timing", "fReflectDelayMaxSeconds", s.reflectDelayMaxSeconds), s.reflectDelayMinSeconds, 600.0f);

        s.followerRangeUnits = std::clamp(ReadFloat("Range", "fFollowerRangeUnits", s.followerRangeUnits), 200.0f, 20000.0f);
        s.headSearchRadiusUnits = std::clamp(ReadFloat("Range", "fHeadSearchRadiusUnits", s.headSearchRadiusUnits), 200.0f, 8000.0f);
        s.maxSummaryLines = std::clamp(ReadInt("Text", "iMaxSummaryLines", s.maxSummaryLines), 1, 6);
    }

    void SetupLog()
    {
        auto dir = logger::log_directory();
        if (!dir) {
            return;
        }
        auto path = *dir / std::format("{}.log", kPluginName);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
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
        spdlog::set_level(levels[g_settings.logLevel]);
        spdlog::flush_on(levels[g_settings.logLevel]);
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
            logger::debug("DispatchStaticCall {}.{} failed", scriptName, functionName);
        }
        return ok;
    }

    // ------------------------------------------------------------------------------------
    // Optional mod integration
    // ------------------------------------------------------------------------------------
    struct Integrations
    {
        bool ngd{false};
        bool df{false};
        RE::TESObjectACTI* ngdRefActivator{nullptr};
        RE::BGSKeyword* ngdActorKeyword{nullptr};
        RE::BGSKeyword* ngdHeadKeyword{nullptr};
    };

    Integrations g_mods;

    void DetectIntegrations()
    {
        auto data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            return;
        }
        g_mods.ngd = data->LookupLoadedModByName(kNgdPlugin) != nullptr;
        g_mods.df = data->LookupLoadedModByName(kDfPlugin) != nullptr;
        if (g_mods.ngd) {
            g_mods.ngdActorKeyword = data->LookupForm<RE::BGSKeyword>(0x800, kNgdPlugin);
            g_mods.ngdHeadKeyword = data->LookupForm<RE::BGSKeyword>(0x801, kNgdPlugin);
            g_mods.ngdRefActivator = data->LookupForm<RE::TESObjectACTI>(0x802, kNgdPlugin);
        }
        logger::info("Integrations: Next-Gen Decapitations={} (forms {}), Dismembering Framework={}",
            g_mods.ngd,
            g_mods.ngdActorKeyword && g_mods.ngdHeadKeyword && g_mods.ngdRefActivator ? "ok" : "missing",
            g_mods.df);
    }

    struct LimbNode
    {
        const char* node;
        const char* label;
    };

    // Nodes used by Dismembering Framework's official humanoid pack.
    constexpr LimbNode kLimbNodes[] = {
        { "NPC L Forearm [LLar]", "left forearm" },
        { "NPC R Forearm [RLar]", "right forearm" },
        { "NPC L Calf [LClf]", "left leg" },
        { "NPC R Calf [RClf]", "right leg" },
        { "TailBone02", "tail" },
    };
    constexpr const char* kNeckNode = "NPC Neck [Neck]";

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
        if (items.empty()) {
            return {};
        }
        if (items.size() == 1) {
            return items[0];
        }
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
        e.sentence = parts.empty() ? std::string{} : parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            e.sentence += ", and " + parts[i];
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

    void Notify(const std::string& text)
    {
        if (g_settings.debugNotifications) {
            RE::DebugNotification(("[CHIM Gore] " + text).c_str());
        }
    }

    // ------------------------------------------------------------------------------------
    // Sending to CHIM
    // ------------------------------------------------------------------------------------
    void LogToChim(const std::string& sentence)
    {
        if (!g_settings.logEachEvent || sentence.empty()) {
            return;
        }
        // "info_*" events are stored in CHIM's event log and shown to nearby NPCs as context.
        if (!CallStatic(kChimScript, "logMessage", nullptr, std::string(sentence), std::string("info_gore"))) {
            logger::warn("Could not reach CHIM (AIAgentFunctions.logMessage). Is CHIM installed?");
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
            logger::info("No follower accepted the reflection request");
            return;
        }
        const std::string name = (*candidates)[index];
        logger::info("Asking {} to reflect on the fight", name);
        const bool dispatched = CallStatic(
            kChimScript, "requestMessageForEligibleActor",
            [candidates, index, message, name](const RE::BSScript::Variable& result) {
                const int code = result.IsInt() ? result.GetSInt() : -99;
                if (code == 1) {
                    logger::info("CHIM queued the reflection for {}", name);
                    return;
                }
                logger::info("CHIM did not queue reflection for {} (code {}), trying next", name, code);
                if (auto tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([candidates, index, message]() { TryReflect(candidates, index + 1, message); });
                }
            },
            std::string(message), std::string("instruction"), std::string(name));
        if (!dispatched) {
            logger::warn("Could not reach CHIM (AIAgentFunctions.requestMessageForEligibleActor)");
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
        if (!g_settings.reflectAfterCombat) {
            return;
        }
        auto candidates = std::make_shared<std::vector<std::string>>(PickCandidates(events));
        if (candidates->empty()) {
            logger::info("Fight ended with {} gore event(s) but no follower is nearby", events.size());
            return;
        }
        Notify(std::format("{} gore event(s), asking {}", events.size(), candidates->front()));
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
                // Combat (re)started: any pending quiet timer becomes stale.
                if (g_fight.quietScheduled) {
                    ++g_fight.generation;
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
            logger::info("Combat ended; reflection in {} ms unless combat resumes", delay.count());
            Scheduler::Get().After(delay, [generation, fightGeneration]() {
                if (generation != g_sessionGeneration.load()) {
                    return;
                }
                auto player = RE::PlayerCharacter::GetSingleton();
                if (player && player->IsInCombat()) {
                    return;  // the watcher will reset and reschedule
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
            auto body = ref->GetLinkedRef(g_mods.ngdActorKeyword);
            if (body != victim) {
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
        bool papyrusDecapitated{false};
        std::uint64_t generation{0};
        RE::ActorHandle victim;
    };

    void FinalizeInspection(const std::shared_ptr<PendingInspection>& pending)
    {
        if (pending->generation != g_sessionGeneration.load()) {
            return;
        }
        auto victim = pending->victim.get();
        GoreEvent event;
        {
            std::lock_guard lock(pending->mutex);
            event = pending->event;
            event.decapitated = event.decapitated || pending->papyrusDecapitated;
        }
        if (victim && g_mods.ngd) {
            event.headDistanceMeters = FindHeadDistanceMeters(victim.get());
            if (event.headDistanceMeters >= 0.0f) {
                event.decapitated = true;
            }
        }
        if (!event.decapitated && event.limbs.empty()) {
            logger::debug("{} died without dismemberment", event.victimName);
            return;
        }
        ScoreEvent(event);
        BuildSentence(event);
        logger::info("Gore event (score {}): {}", event.score, event.sentence);
        Notify(event.sentence);
        LogToChim(event.sentence);
        AddEventToFight(std::move(event));
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
                tasks->AddTask([pending]() { FinalizeInspection(pending); });
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

        // Hold one slot while dispatching so callbacks cannot finish before we are done.
        pending->outstanding = 1;

        auto add = [&](auto&& dispatch) {
            {
                std::lock_guard lock(pending->mutex);
                ++pending->outstanding;
            }
            if (!dispatch()) {
                CompleteOne(pending);
            }
        };

        RE::Actor* victimPtr = victim.get();

        if (g_mods.ngd) {
            add([&]() {
                return CallStatic("NGDecapitations", "IsDecapitated",
                    [pending](const RE::BSScript::Variable& r) {
                        if (r.IsBool() && r.GetBool()) {
                            std::lock_guard lock(pending->mutex);
                            pending->papyrusDecapitated = true;
                        }
                        CompleteOne(pending);
                    },
                    std::move(victimPtr));
            });
        }

        if (g_mods.df) {
            for (const auto& limb : kLimbNodes) {
                RE::Actor* target = victim.get();
                add([&]() {
                    const std::string label = limb.label;
                    return CallStatic("DismemberingFramework", "IsDismemberedNode",
                        [pending, label](const RE::BSScript::Variable& r) {
                            if (r.IsBool() && r.GetBool()) {
                                std::lock_guard lock(pending->mutex);
                                pending->event.limbs.push_back(label);
                            }
                            CompleteOne(pending);
                        },
                        std::move(target), std::string(limb.node));
                });
            }
            RE::Actor* target = victim.get();
            add([&]() {
                return CallStatic("DismemberingFramework", "IsDismemberedNode",
                    [pending](const RE::BSScript::Variable& r) {
                        if (r.IsBool() && r.GetBool()) {
                            std::lock_guard lock(pending->mutex);
                            pending->event.decapitated = true;
                        }
                        CompleteOne(pending);
                    },
                    std::move(target), std::string(kNeckNode));
            });
        }

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
            if (!a_event || !g_settings.enabled || (!g_mods.ngd && !g_mods.df)) {
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

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            DetectIntegrations();
            if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
                holder->AddEventSink<RE::TESDeathEvent>(DeathSink::Get());
            }
            Scheduler::Get().Start();
            logger::info("Ready");
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
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
    LoadSettings();
    ApplyLogLevel();
    logger::info("CHIM Gore loading (enabled={})", g_settings.enabled);
    if (auto messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }
    return true;
}
