#include "hailstone_plugin.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <string_view>
#include <thread>

extern "C" void hailstone_plugin_set_host(const PluginHostApi*);
extern "C" void InitializePlugin();
extern "C" void ShutdownPlugin();
extern "C" void OnPulse();
extern "C" void OnBeginZone();
extern "C" void OnZoned();
extern "C" void OnUpdateImGui();

namespace {
HailstoneCommandFn command = nullptr;
const char* config = nullptr;
std::string result;
struct Binding { HailstoneCommandFn fn; void* user; };
std::map<std::string, Binding> bindings;
std::vector<std::string> chat;
bool production = false;
bool broker_probe = false;
const auto client_thread = std::this_thread::get_id();
void require_client_thread()
{
    if (std::this_thread::get_id() != client_thread) std::abort();
}
enum class Party { solo, group, raid };
Party party = Party::solo;
uint32_t zone = 10;
uint32_t target = 42;
uint64_t incarnation = 1;
bool attacking = false;
int attack_ons = 0;
int attack_offs = 0;
int xtarget_bulk_calls = 0;

int copy(std::string_view text, char* out, std::size_t capacity)
{
    if (text.size() >= capacity) return -1;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return static_cast<int>(text.size());
}

bool turn(const char* action, const char* expected)
{
    result.clear();
    command(nullptr, action);
    for (int i = 0; i < 500; ++i) {
        OnPulse();
        if (result.find(expected) != std::string::npos) return true;
        if (result.find("failed") != std::string::npos) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) return 2;
    broker_probe = argc == 3;
    config = argv[1];
    PluginHostApi host{};
    host.size = sizeof host;
    host.write_chat = [](void*, const char* text, uint32_t) { require_client_thread(); chat.emplace_back(text); };
    host.caps = [](void*) -> uint32_t { return HAILSTONE_CAP_READ | HAILSTONE_CAP_COMMANDS | HAILSTONE_CAP_ACTIONS | HAILSTONE_CAP_UI | HAILSTONE_CAP_NET; };
    host.do_command = [](void*, const char* text) {
        require_client_thread();
        const std::string_view command_text(text);
        if (command_text == "/attack on") { attacking = true; ++attack_ons; }
        else if (command_text == "/attack off") { attacking = false; ++attack_offs; }
        else if (!production) std::abort();
    };
    host.get_spawn = [](void*, uint32_t id, HailstoneSpawn* spawn) {
        if (id != target) return 0;
        spawn->is_npc = 1;
        spawn->incarnation = incarnation;
        std::snprintf(spawn->name, sizeof spawn->name, "%s", "Training NPC");
        spawn->level = 10;
        spawn->con_color = 13; // CONCOLOR_RED
        spawn->x = 10.0f;
        spawn->y = 20.0f;
        return 1;
    };
    host.get_xtarget_data_bulk = [](void*, unsigned char* out, int32_t capacity) -> int32_t {
        require_client_thread();
        ++xtarget_bulk_calls;
        if (capacity < static_cast<int32_t>(sizeof(HS_Bulk_XTargetData))) return 0;
        HS_Bulk_XTargetData record{};
        record.targetType = HS_BULK_XTARGET_AUTO_HATER;
        record.id = static_cast<int32_t>(target);
        record.aggroPct = 77;
        record.pctHPs = 88;
        std::memcpy(out, &record, sizeof record);
        return sizeof record;
    };
    host.config_dir = [](void*, char* out, std::size_t size) { return copy(config, out, size); };
    host.add_command = [](void*, const char* name, HailstoneCommandFn fn, void* user) {
        require_client_thread();
        if (std::string_view(name) == "/hmonosmoke") command = fn;
        return bindings.emplace(name, Binding{fn, user}).second ? 1 : 0;
    };
    host.remove_command = [](void*, const char* name) {
        require_client_thread();
        if (std::string_view(name) == "/hmonosmoke") command = nullptr;
        bindings.erase(name);
    };
    host.log = [](void*, int, const char* text) { result = text; std::puts(text); };
    host.get_player = [](void*, HailstonePlayer* player) {
        player->in_zone = 1;
        player->spawn_id = 1;
        player->class_id = 1;
        player->target_id = target;
        player->zone_id = zone;
        player->in_group = party == Party::group;
        player->in_raid = party == Party::raid;
        return 1;
    };
    host.get_raid_data_bulk = [](void*, unsigned char* out, int32_t capacity) -> int32_t {
        HS_BulkRaidHeader header{};
        header.size = sizeof header;
        header.active = 1;
        header.version = 1;
        header.count = header.total = 1;
        header.recordSize = sizeof(HS_BulkRaidData);
        HS_BulkRaidData member{};
        member.flags = HS_BULK_RAID_MAIN_ASSIST;
        std::memcpy(member.name, "Warrior", sizeof "Warrior");
        constexpr int32_t size = sizeof header + sizeof member;
        if (capacity < size) return 0;
        std::memcpy(out, &header, sizeof header);
        std::memcpy(out + sizeof header, &member, sizeof member);
        return size;
    };
    host.parse = [](void*, const char* text, char* out, std::size_t size) {
        require_client_thread();
        const std::string_view query(text);
        std::string_view value = "FALSE";
        if (query == "${Me.Class.ShortName}") value = "WAR";
        else if (query == "${Me.CleanName}" || query == "${Group.MainTank.Name}") value = "Warrior";
        else if (query.ends_with(".CleanName}") || query.ends_with(".Name}")) value = "Training NPC";
        else if (query == "${Zone.ShortName}") value = "training";
        else if (query == "${Group.MainAssist.CleanName}" || query == "${Group.MainAssist.Name}" ||
                 query == "${Group.MainTank.CleanName}") value = "NULL";
        else if (query == "${Group.Members}") value = "1";
        else if (query == "${Me.Combat}") value = attacking ? "TRUE" : "FALSE";
        else if (query == "${Me.PctEndurance}") value = "100";
        else if (query == "${Me.Pet.ID}" || query == "${Me.Casting.ID}") value = "0";
        else if (query.ends_with(".ID}")) return copy(std::to_string(target), out, size);
        else if (query.ends_with(".Type}")) value = "NPC";
        else if (query.ends_with(".Distance3D}")) value = "8";
        return copy(value, out, size);
    };

    // A real context and frame for the production UI pass below. The plugin's
    // OnUpdateImGui export hands this context to the managed windows.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int pixelWidth = 0, pixelHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &pixelWidth, &pixelHeight);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    host.imgui_context = [](void*) -> void* { return ImGui::GetCurrentContext(); };

    hailstone_plugin_set_host(&host);
    InitializePlugin();
    const auto draw_ui = [] {
        int vertices = 0;
        for (int i = 0; i < 3; ++i) {
            ImGui::NewFrame();
            OnUpdateImGui();
            ImGui::Render();
            if (const ImDrawData* data = ImGui::GetDrawData()) vertices = data->TotalVtxCount;
        }
        return vertices;
    };
    if (broker_probe) {
        production = true;
        command(nullptr, "sentinel");
        const bool ui_drawn = draw_ui() > 0;
        const auto sentinel = bindings.find("/sentinel");
        const auto engage = bindings.find("/engage");
        if (sentinel == bindings.end() || engage == bindings.end()) { ShutdownPlugin(); return 1; }
        bool online = false;
        for (int i = 0; i < 1000 && !online; ++i) {
            if (i % 20 == 0) sentinel->second.fn(sentinel->second.user, "status");
            OnPulse();
            for (const auto& line : chat) {
                if (line.find(" connected pending=") != std::string::npos) online = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (online) {
            engage->second.fn(engage->second.user, "42");
            for (int i = 0; i < 600 && (attack_ons == 0 || attack_offs == 0); ++i) {
                OnPulse();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        const bool received_release = online && attack_ons > 0 && attack_offs > 0;
        ShutdownPlugin();
        if (received_release) std::puts("Broker echo fixture passed");
        ImGui::DestroyContext();
        return received_release && ui_drawn ? 0 : 1;
    }
    const bool first = command && turn("warrior", "/attack on") &&
        result.find("Uncaptured reads") == std::string::npos;
    party = Party::group;
    const bool group = first && turn("warrior", "/attack on") &&
        result.find("Uncaptured reads") == std::string::npos;
    party = Party::raid;
    const bool raid = group && turn("warrior", "/attack on") &&
        result.find("Uncaptured reads") == std::string::npos;
    const bool release = raid && turn("disengage", "/attack off");

    // Close the domain before polling a submitted decision.
    command(nullptr, "warrior");
    OnBeginZone();
    OnPulse();
    const bool zoning = result.find("closed: zoning") != std::string::npos;

    command(nullptr, "warrior");
    ++zone;
    OnPulse();
    const bool identity = result.find("character or zone changed") != std::string::npos;

    ShutdownPlugin();
    InitializePlugin();
    const bool restart = release && zoning && identity && command && turn("warrior", "/attack on") &&
        attack_ons == 0 && attack_offs == 0;

    command(nullptr, "attack");
    const bool raid_blocked = result.find("you are in a raid") != std::string::npos && attack_ons == 0;
    party = Party::solo;
    const bool live = restart && raid_blocked && turn("attack", "live attack enabled") && attacking && attack_ons == 1;
    const bool stopped = live && turn("disengage", "/attack off") && !attacking && attack_offs == 1;
    const bool live_again = stopped && turn("attack", "live attack enabled") && attack_ons == 2;
    ++target;
    OnPulse();
    const bool changed_target = live_again && !attacking && attack_offs == 2;

    result.clear();
    command(nullptr, "attack");
    ++incarnation;
    for (int i = 0; i < 500 && result.find("discarded") == std::string::npos; ++i) {
        OnPulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool reused_target = result.find("discarded") != std::string::npos && attack_ons == 2;
    const bool before_zone = changed_target && reused_target && turn("attack", "live attack enabled");
    OnBeginZone();
    const bool zone_release = before_zone && !attacking && attack_ons == 3 && attack_offs == 3;
    if (!zone_release) { ShutdownPlugin(); return 1; }

    production = true;
    const auto mono = bindings.find("/mono");
    if (mono == bindings.end()) { ShutdownPlugin(); return 1; }
    const Binding mono_command = mono->second;
    const auto run_mono = [&](const char* args) { mono_command.fn(mono_command.user, args); };
    run_mono("list");
    const bool empty_list = !chat.empty() && chat.back().find("No managed apps running") != std::string::npos;
    run_mono("version");
    const bool version = !chat.empty() && chat.back().find("HSMono version " HAILSTONE_MONO_VERSION) != std::string::npos;
    run_mono("load Unknown");
    const bool unknown_app = !chat.empty() && chat.back().find("Unknown managed app") != std::string::npos;
    run_mono("  LoAd   sEnTiNeL  ");
    run_mono("list");
    const bool running_list = !chat.empty() && chat.back() == "HSMono: Sentinel";
    const bool ui_drawn = draw_ui() > 0;
    const auto sentinel = bindings.find("/sentinel");
    if (sentinel == bindings.end()) { ShutdownPlugin(); return 1; }
    // The C# windows are the only implementation in Hailstone, so their
    // commands must be in the client's command table, not skipped as they
    // were when MQSentinel's native plugin owned them.
    const bool window_commands = bindings.find("/sentpeers") != bindings.end()
        && bindings.find("/sentoptions") != bindings.end()
        && bindings.find("/sentmini") != bindings.end()
        && bindings.find("/sentsongs") != bindings.end()
        && bindings.find("/senttt") != bindings.end()
        && bindings.find("/sentstuck") != bindings.end();
    if (!window_commands) {
        for (const auto& binding : bindings) std::printf("bound: %s\n", binding.first.c_str());
        ShutdownPlugin(); return 1;
    }
    // /sentpeers must reach the legacy ImGui HeartbeatWindow in Hailstone:
    // there is no MQSentinel RmlUi roster here. Toggle it and check ImGui
    // created the window with the same name HeartbeatWindow uses.
    bindings.find("/sentpeers")->second.fn(bindings.find("/sentpeers")->second.user, "");
    // Commands enter the managed queue and are handled on the worker turn that
    // OnPulse grants; the game's frame loop does this before the next draw.
    for (int i = 0; i < 20; ++i) {
        OnPulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    draw_ui();
    const bool peer_window = ImGui::FindWindowByName("EQ Hub###SentinelHeartbeatBars") != nullptr;
    if (!peer_window) std::puts("peer window was not created");
    sentinel->second.fn(sentinel->second.user, "status");
    bool status = false;
    for (int i = 0; i < 200 && !status; ++i) {
        OnPulse();
        for (const auto& line : chat) {
            if (line.find("[Sentinel] commands:") != std::string::npos)
                status = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto engage = bindings.find("/engage");
    if (engage == bindings.end()) { ShutdownPlugin(); return 1; }
    engage->second.fn(engage->second.user, "local 43");
    for (int i = 0; i < 200 && !attacking; ++i) {
        OnPulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool engaged = attacking;
    const auto disengage = bindings.find("/sentdisengage");
    if (disengage == bindings.end()) { ShutdownPlugin(); return 1; }
    disengage->second.fn(disengage->second.user, "");
    for (int i = 0; i < 200 && attacking; ++i) {
        OnPulse();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool disengaged = !attacking;
    const auto clear = bindings.find("/sentclear");
    if (clear == bindings.end()) { ShutdownPlugin(); return 1; }
    clear->second.fn(clear->second.user, "local");
    bool camp_clear = false;
    for (int i = 0; i < 200 && !camp_clear; ++i) {
        OnPulse();
        for (const auto& line : chat) {
            if (line.find("1 haters remaining") != std::string::npos) camp_clear = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    OnBeginZone();
    const bool zone_unbound = bindings.find("/sentinel") == bindings.end();
    ++zone;
    OnZoned();
    const bool zone_rebound = bindings.find("/sentinel") != bindings.end();
    for (const auto& line : chat) std::puts(line.c_str());
    run_mono("unload Sentinel");
    const bool cleared = bindings.find("/sentinel") == bindings.end();
    run_mono("run Sentinel");
    const bool alias_loaded = bindings.find("/sentinel") != bindings.end();
    OnBeginZone();
    run_mono("stop ALL");
    OnZoned();
    const bool cancelled_resume = bindings.find("/sentinel") == bindings.end();
    ShutdownPlugin();
    const bool commands_removed = bindings.find("/mono") == bindings.end() && command == nullptr;
    ImGui::DestroyContext();
    std::printf("xtarget bulk calls=%d\n", xtarget_bulk_calls);
    if (!ui_drawn) std::puts("UI pass drew no vertices");
    return status && engaged && disengaged && camp_clear && zone_unbound && zone_rebound && cleared && ui_drawn
        && window_commands && peer_window && xtarget_bulk_calls > 0
        && empty_list && version && unknown_app && running_list && alias_loaded && cancelled_resume
        && commands_removed ? 0 : 1;
}
