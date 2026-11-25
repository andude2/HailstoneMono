#include "hailstone_plugin.h"
#include "ImGuiBridge.h"
#include "platform/MonoLibrary.h"

#include <array>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

HAILSTONE_PLUGIN

namespace {

using hs_mono::MonoLibrary;

constexpr char smoke_command_name[] = "/hmonosmoke";
constexpr char mono_command_name[] = "/mono";
std::unique_ptr<MonoLibrary> runtime;
MonoDomain* root = nullptr;
MonoDomain* session = nullptr;
MonoMethod* pulse = nullptr;
MonoMethod* poll = nullptr;
MonoMethod* stop = nullptr;
bool started = false;
bool warrior_session = false;
bool sentinel_session = false;
bool sentinel_resume = false;
MonoMethod* sentinel_command = nullptr;
MonoMethod* sentinel_chat = nullptr;
MonoMethod* sentinel_status = nullptr;
MonoMethod* sentinel_imgui = nullptr;
std::set<std::string> sentinel_bindings;
uint32_t warrior_player = 0;
uint32_t warrior_zone = 0;
uint32_t warrior_instance = 0;
bool reported_pulse = false;
bool smoke_command_registered = false;
bool mono_command_registered = false;
bool warrior_pending = false;

struct AttackTarget {
    uint32_t id;
    uint64_t incarnation;
};
std::optional<AttackTarget> requested_attack;
std::optional<AttackTarget> owned_attack;
std::chrono::steady_clock::time_point attack_deadline;

void log(int level, const char* message)
{
    host->log(host->host, level, message);
}

void release_attack()
{
    if (!owned_attack) return;
    owned_attack.reset();
    HailstonePlayer player{};
    player.size = sizeof player;
    if (host->get_player(host->host, &player) && player.spawn_id == warrior_player) {
        DoCommand("/attack off");
        log(1, "Sentinel live attack release requested");
    }
}

bool invoke(MonoMethod* method, void** args = nullptr, MonoObject** result = nullptr)
{
    MonoObject* exception = nullptr;
    auto* value = runtime->invoke(method, nullptr, args, &exception);
    if (result) *result = value;
    return exception == nullptr;
}

void clear_sentinel_commands()
{
    for (const auto& name : sentinel_bindings) RemoveCommand(name.c_str());
    sentinel_bindings.clear();
}

bool invoke_text(MonoMethod* method, const char* text)
{
    auto* previous = runtime->current_domain();
    if (!runtime->set_domain(session, false)) return false;
    auto* value = runtime->make_string(session, text);
    void* args[] = {value};
    const bool ok = value && invoke(method, args);
    runtime->set_domain(previous ? previous : root, true);
    return ok;
}

void sentinel_command_callback(void* user, const char* args)
{
    if (!sentinel_session) return;
    auto line = *static_cast<const std::string*>(user);
    if (args && *args) {
        line += ' ';
        line += args;
    }
    if (!invoke_text(sentinel_command, line.c_str())) log(3, "Sentinel command callback failed");
}

int bind_sentinel_command(MonoString* value)
{
    if (!value) return 0;
    char* text = runtime->string_utf8(value);
    if (!text) return 0;
    auto [it, inserted] = sentinel_bindings.emplace(text);
    runtime->free_memory(text);
    if (!inserted) return 1;
    if (AddCommand(it->c_str(), sentinel_command_callback, const_cast<std::string*>(&*it))) return 1;
    log(2, it->c_str());
    sentinel_bindings.erase(it);
    return 0;
}

std::optional<std::vector<HS_BulkRaidData>> raid_roster();
std::optional<std::string> sentinel_raid_query(std::string_view expression);
MonoString* sentinel_raid_heal_members();

MonoString* sentinel_parse(MonoString* value)
{
    std::array<char, 4096> result{};
    char* text = value ? runtime->string_utf8(value) : nullptr;
    if (text && std::string_view(text).starts_with("${Raid.")) {
        const auto answer = sentinel_raid_query(text);
        if (answer) {
            runtime->free_memory(text);
            return runtime->make_string(session, answer->c_str());
        }
    }
    const int size = text ? ParseMacroData(text, result.data(), result.size()) : -1;
    if (text) runtime->free_memory(text);
    return runtime->make_string(session, size >= 0 && static_cast<std::size_t>(size) < result.size() ?
                                result.data() : "NULL");
}

void sentinel_execute(MonoString* value)
{
    char* text = value ? runtime->string_utf8(value) : nullptr;
    if (!text) return;
    DoCommand(text);
    runtime->free_memory(text);
}

void sentinel_print(MonoString* value)
{
    char* text = value ? runtime->string_utf8(value) : nullptr;
    if (!text) return;
    // Hailstone renders MacroQuest colour codes (\ag, \ay, \a#RRGGBB, \ax ...)
    // in plugin chat lines and strips them from plain-text logs itself.
    WriteChatf("%s", text);
    runtime->free_memory(text);
}

int sentinel_lend_thread(int lend)
{
    if (!HAILSTONE_HOST_HAS(host, lend_thread) || !host->lend_thread) return 0;
    return host->lend_thread(host->host, lend);
}

int sentinel_command_exists(MonoString* value)
{
    if (!HAILSTONE_HOST_HAS(host, command_exists) || !host->command_exists) return -1;
    char* text = value ? runtime->string_utf8(value) : nullptr;
    if (!text) return HAILSTONE_COMMAND_NONE;
    const int result = host->command_exists(host->host, text);
    runtime->free_memory(text);
    return result;
}

MonoString* sentinel_scripts_config_directory()
{
    if (!HAILSTONE_HOST_HAS(host, get_scripts_config_dir) || !host->get_scripts_config_dir)
        return runtime->make_string(session, "");
    std::array<char, 4096> directory{};
    const int length = host->get_scripts_config_dir(host->host, directory.data(), directory.size());
    return runtime->make_string(session, length > 0 && static_cast<std::size_t>(length) < directory.size() ?
                                directory.data() : "");
}

MonoString* sentinel_config()
{
    std::array<char, 4096> directory{};
    const int length = host->character_config_dir ?
        host->character_config_dir(host->host, directory.data(), directory.size()) : -1;
    return runtime->make_string(session, length > 0 && static_cast<std::size_t>(length) < directory.size() ?
                                directory.data() : "");
}

MonoString* sentinel_plugin_directory()
{
    std::array<char, 4096> directory{};
    const int length = host->config_dir(host->host, directory.data(), directory.size());
    return runtime->make_string(session, length > 0 && static_cast<std::size_t>(length) < directory.size() ?
                                directory.data() : "");
}

int sentinel_network_allowed()
{
    return host->caps && (host->caps(host->host) & HAILSTONE_CAP_NET) ? 1 : 0;
}

int sentinel_use_ability(int kind, MonoString* name)
{
    if (!name || !HAILSTONE_HOST_HAS(host, use_ability) || !host->use_ability) return 0;
    char* value = runtime->string_utf8(name);
    if (!value) return 0;
    const int result = host->use_ability(host->host, kind, value);
    runtime->free_memory(value);
    return result;
}

// The C# catalogue and a few class brains ask for the Ability Book's entries.
// The Hailstone host lists the usable skills, disciplines and owned activated
// AAs in one bulk read; hand them over as "kind\tid\tname" lines. The managed
// side keeps the small map and answers MQ's ${Me.AltAbility[...]} expressions
// from it, since Hailstone has no AltAbility TLO type.
MonoString* sentinel_ability_records()
{
    std::string result;
    if (HAILSTONE_HOST_HAS(host, get_ability_data_bulk) && host->get_ability_data_bulk) {
        std::array<unsigned char, 16384> data{};
        const auto written = host->get_ability_data_bulk(host->host, data.data(),
                                                         static_cast<int32_t>(data.size()));
        if (written >= static_cast<int32_t>(sizeof(HS_Bulk_ListHeader))) {
            HS_Bulk_ListHeader header{};
            std::memcpy(&header, data.data(), sizeof header);
            std::size_t offset = sizeof header;
            const auto length = static_cast<std::size_t>(written);
            for (int32_t i = 0; i < header.count; ++i) {
                if (offset + sizeof(HS_Bulk_AbilityRecord) > length) break;
                HS_Bulk_AbilityRecord record{};
                std::memcpy(&record, data.data() + offset, sizeof record);
                offset += sizeof record;
                if (record.nameSize < 0 || offset + static_cast<std::size_t>(record.nameSize) > length) break;
                const std::string_view name(reinterpret_cast<const char*>(data.data() + offset),
                                            static_cast<std::size_t>(record.nameSize));
                offset += static_cast<std::size_t>(record.nameSize);
                if (name.find_first_of("\t\r\n") != std::string_view::npos) continue;
                result += std::to_string(record.kind);
                result += '\t';
                result += std::to_string(record.id);
                result += '\t';
                result += name;
                result += '\n';
            }
        }
    }
    return runtime->make_string(session, result.c_str());
}

uint32_t sentinel_aa_value(int group)
{
    if (!HAILSTONE_HOST_HAS(host, aa_value) || !host->aa_value) return 0;
    return host->aa_value(host->host, group);
}

// The extended-target window needs slot ids, aggro and HP from the bulk read,
// then the spawn's name/level/con/distance from the spawn accessor. The SDK
// supplies the slot role and aggro, which the spawn TLO does not expose.
// The reply is one tab-separated line per slot:
// index, id, aggroPct, pctHPs, level, conColor, name, distance*10, valid, slot role.
MonoString* sentinel_xtargets()
{
    const bool have_bulk = HAILSTONE_HOST_HAS(host, get_xtarget_data_bulk) && host->get_xtarget_data_bulk;
    const bool have_slot = HAILSTONE_HOST_HAS(host, get_xtarget) && host->get_xtarget;
    if (!have_bulk && !have_slot) return runtime->make_string(session, "NULL");

    HailstonePlayer player{};
    player.size = sizeof player;
    const bool have_player_state = host->get_player && host->get_player(host->host, &player);
    std::array<HS_Bulk_XTargetData, 64> records{};
    const auto written = have_bulk ? host->get_xtarget_data_bulk(host->host,
        reinterpret_cast<unsigned char*>(records.data()), static_cast<int32_t>(sizeof records)) : 0;
    const auto bytes = std::min(written > 0 ? static_cast<std::size_t>(written) : std::size_t{0},
                                sizeof records);
    std::size_t count = bytes / sizeof(HS_Bulk_XTargetData);
    if (have_slot && have_player_state) {
        const auto total = std::min<std::size_t>(player.xtarget_slots, records.size());
        // Some hosts provide only the per-slot accessor. Keep the physical
        // slot indices and use spawn HP for any rows missing bulk metrics.
        for (; count < total; ++count) {
            HailstoneXTarget slot{};
            slot.size = sizeof slot;
            if (host->get_xtarget(host->host, static_cast<uint32_t>(count), &slot)) {
                records[count].id = static_cast<int32_t>(slot.spawn_id);
                records[count].targetType = static_cast<int32_t>(slot.type);
            }
        }
    }

    HailstoneSpawn player_spawn{};
    player_spawn.size = sizeof player_spawn;
    const bool have_player = have_player_state && player.spawn_id != 0 && host->get_spawn &&
        host->get_spawn(host->host, player.spawn_id, &player_spawn);

    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        const int index = static_cast<int>(i) + 1;
        int level = 0, con = 0, valid = 0, hp = record.pctHPs;
        int distance_tenths = 0;
        std::string name;
        if (record.id > 0 && host->get_spawn) {
            HailstoneSpawn spawn{};
            spawn.size = sizeof spawn;
            if (host->get_spawn(host->host, static_cast<uint32_t>(record.id), &spawn)) {
                name = spawn.name;
                for (char& c : name) {
                    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
                }
                level = static_cast<int>(spawn.level);
                con = static_cast<int>(spawn.con_color);
                if (hp <= 0) hp = static_cast<int>(spawn.hp_percent);
                valid = spawn.is_npc && !spawn.is_corpse ? 1 : 0;
                if (have_player) {
                    const double dx = static_cast<double>(spawn.x) - player_spawn.x;
                    const double dy = static_cast<double>(spawn.y) - player_spawn.y;
                    distance_tenths = static_cast<int>(std::llround(std::sqrt(dx * dx + dy * dy) * 10.0));
                }
            }
        }
        result += std::to_string(index);
        result += '\t';
        result += std::to_string(record.id);
        result += '\t';
        result += std::to_string(record.aggroPct);
        result += '\t';
        result += std::to_string(hp);
        result += '\t';
        result += std::to_string(level);
        result += '\t';
        result += std::to_string(con);
        result += '\t';
        result += name;
        result += '\t';
        result += std::to_string(distance_tenths);
        result += '\t';
        result += std::to_string(valid);
        result += '\t';
        result += std::to_string(record.targetType);
        result += '\n';
    }
    return runtime->make_string(session, result.c_str());
}

MonoString* sentinel_item_click(int32_t slot, int32_t sub)
{
    HailstoneItem item{};
    item.size = sizeof item;
    if (host->get_item && slot >= 0 && slot < HAILSTONE_SLOT_CURSOR &&
        sub >= -1 && sub < 80) {
        const HailstoneItemSlot address{0, static_cast<int16_t>(slot),
            static_cast<int16_t>(sub), -1};
        if (host->get_item(host->host, &address, &item) && item.click_spell) {
            const std::string result = std::to_string(item.click_spell) + '\t' + item.name;
            return runtime->make_string(session, result.c_str());
        }
    }
    return runtime->make_string(session, "");
}

// One bulk read for Sentinel's NativeData capture (HailstoneBulkKind in
// Native/HailstoneNativeSource.cs). The bytes are the SDK's own layouts
// (sdk/hailstone_plugin.h HS_Bulk*); the managed side translates them into
// the MQ2Mono wire formats its tested decoders consume. Capture runs on the
// game thread before the worker is granted, so a single reused scratch
// buffer is safe. It is sized for a full zone of spawns: the host's spawn
// writer truncates silently at the buffer limit, and a truncated roster
// would read as despawns.
MonoArray* sentinel_bulk(int kind)
{
    using BulkFn = int32_t (*)(void*, unsigned char*, int32_t);
    BulkFn read = nullptr;
    switch (kind) {
    case 0: // SelfEffects
        if (HAILSTONE_HOST_HAS(host, get_buff_data_bulk)) read = host->get_buff_data_bulk;
        break;
    case 1: // TargetEffects
        if (HAILSTONE_HOST_HAS(host, get_target_buff_data_bulk)) read = host->get_target_buff_data_bulk;
        break;
    case 2: // PetEffects
        if (HAILSTONE_HOST_HAS(host, get_pet_buff_data_bulk)) read = host->get_pet_buff_data_bulk;
        break;
    case 3: // XTargets
        if (HAILSTONE_HOST_HAS(host, get_xtarget_data_bulk)) read = host->get_xtarget_data_bulk;
        break;
    case 4: // SpawnDeltas and FullSpawns read the same host snapshot; the
    case 5: // managed adapter derives delta records from it.
        if (HAILSTONE_HOST_HAS(host, get_spawns_data_bulk)) read = host->get_spawns_data_bulk;
        break;
    case 6: // AvailAAs
        if (HAILSTONE_HOST_HAS(host, get_avail_aa_data_bulk)) read = host->get_avail_aa_data_bulk;
        break;
    case 7: // AvailDiscs
        if (HAILSTONE_HOST_HAS(host, get_avail_disc_data_bulk)) read = host->get_avail_disc_data_bulk;
        break;
    default:
        break;
    }

    std::size_t length = 0;
    if (read) {
        static thread_local std::vector<unsigned char> buffer(4u << 20);
        const auto written = read(host->host, buffer.data(), static_cast<int32_t>(buffer.size()));
        if (written > 0) {
            length = static_cast<std::size_t>(written);
            if (length > buffer.size()) length = buffer.size();
            auto* result = runtime->array_new(session, runtime->byte_class(), length);
            if (result) {
                std::memcpy(runtime->array_addr(result, static_cast<int>(sizeof(unsigned char)), 0),
                            buffer.data(), length);
                return result;
            }
            return nullptr;
        }
    }
    return runtime->array_new(session, runtime->byte_class(), 0);
}

void register_sentinel_calls()
{
    runtime->add_internal_call("Sentinel.HailstoneHost::Parse", reinterpret_cast<const void*>(&sentinel_parse));
    runtime->add_internal_call("Sentinel.HailstoneHost::Command", reinterpret_cast<const void*>(&sentinel_execute));
    runtime->add_internal_call("Sentinel.HailstoneHost::Print", reinterpret_cast<const void*>(&sentinel_print));
    runtime->add_internal_call("Sentinel.HailstoneHost::LendThread", reinterpret_cast<const void*>(&sentinel_lend_thread));
    runtime->add_internal_call("Sentinel.HailstoneHost::CommandExists", reinterpret_cast<const void*>(&sentinel_command_exists));
    runtime->add_internal_call("Sentinel.HailstoneHost::ScriptsConfigDirectory", reinterpret_cast<const void*>(&sentinel_scripts_config_directory));
    runtime->add_internal_call("Sentinel.HailstoneHost::Bind", reinterpret_cast<const void*>(&bind_sentinel_command));
    runtime->add_internal_call("Sentinel.HailstoneHost::Unbind", reinterpret_cast<const void*>(&clear_sentinel_commands));
    runtime->add_internal_call("Sentinel.HailstoneHost::ConfigDirectory", reinterpret_cast<const void*>(&sentinel_config));
    runtime->add_internal_call("Sentinel.HailstoneHost::PluginDirectory", reinterpret_cast<const void*>(&sentinel_plugin_directory));
    runtime->add_internal_call("Sentinel.HailstoneHost::NetworkAllowed", reinterpret_cast<const void*>(&sentinel_network_allowed));
    runtime->add_internal_call("Sentinel.HailstoneHost::UseAbility", reinterpret_cast<const void*>(&sentinel_use_ability));
    runtime->add_internal_call("Sentinel.HailstoneHost::AbilityRecords", reinterpret_cast<const void*>(&sentinel_ability_records));
    runtime->add_internal_call("Sentinel.HailstoneHost::AAValue", reinterpret_cast<const void*>(&sentinel_aa_value));
    runtime->add_internal_call("Sentinel.HailstoneHost::XTargets", reinterpret_cast<const void*>(&sentinel_xtargets));
    runtime->add_internal_call("Sentinel.HailstoneHost::ItemClick", reinterpret_cast<const void*>(&sentinel_item_click));
    runtime->add_internal_call("Sentinel.HailstoneHost::RaidHealMembers", reinterpret_cast<const void*>(&sentinel_raid_heal_members));
    runtime->add_internal_call("Sentinel.HailstoneHost::Bulk", reinterpret_cast<const void*>(&sentinel_bulk));
    hs_mono::imgui_bridge::install(runtime.get());
}

void close_session(const char* reason = nullptr)
{
    if (!session) return;
    const bool was_warrior = warrior_session;
    requested_attack.reset();
    warrior_pending = false;
    release_attack();
    if (started && runtime->set_domain(session, false) && !invoke(stop))
        log(3, "HSMono: managed OnStop failed");
    if (sentinel_session || !sentinel_bindings.empty()) clear_sentinel_commands();
    sentinel_session = false;
    sentinel_command = nullptr;
    sentinel_chat = nullptr;
    sentinel_status = nullptr;
    sentinel_imgui = nullptr;
    started = false;
    warrior_session = false;
    pulse = nullptr;
    poll = nullptr;
    stop = nullptr;
    reported_pulse = false;
    // Mono requires the calling thread to leave the app domain before unload.
    runtime->set_domain(root, true);
    runtime->unload_domain(session);
    session = nullptr;
    if (was_warrior && reason) log(1, reason);
}

bool open_runtime(const std::filesystem::path& directory)
{
    if (runtime) return root != nullptr;

    auto library = std::make_unique<MonoLibrary>();
    if (!library->open(directory / L"mono-2.0-sgen.dll")) {
        log(3, "HSMono: could not load the configured x64 Mono library or its exports");
        return false;
    }

    root = library->root_domain();
    if (!root) {
        if (!library->pin()) {
            log(3, "HSMono: could not keep the Mono library resident");
            return false;
        }
        const auto lib = (directory / L"lib").u8string();
        const auto etc = (directory / L"etc").u8string();
        library->set_dirs(reinterpret_cast<const char*>(lib.c_str()),
                          reinterpret_cast<const char*>(etc.c_str()));
        root = library->init("HSMono", "v4.0.30319");
    }
    runtime = std::move(library);
    if (!root) log(3, "HSMono: Mono runtime initialization failed");
    return root != nullptr;
}

bool active_player(HailstonePlayer& player)
{
    player.size = sizeof player;
    return host->get_player(host->host, &player) && player.in_zone && player.spawn_id != 0;
}

void start_session(bool warrior = false, bool sentinel = false)
{
    if (session) {
        log(1, "HSMono: smoke assembly already running");
        return;
    }
    HailstonePlayer player{};
    if (!active_player(player)) {
        log(2, "HSMono: enter a zone with the read capability before starting the smoke test");
        return;
    }

    std::array<char, 4096> config{};
    const auto length = host->config_dir(host->host, config.data(), config.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= config.size()) {
        log(3, "HSMono: plugin configuration directory unavailable");
        return;
    }
    const auto directory = std::filesystem::path(std::u8string(config.data(), config.data() + length));
    const auto runtime_dir = directory / L"runtime";
    const auto assembly = directory / ((warrior || sentinel) ? L"managed/sentinel.dll" : L"managed/Smoke.dll");
    if (!std::filesystem::is_regular_file(assembly) ||
        !std::filesystem::is_regular_file(runtime_dir / L"lib/mono/4.5/mscorlib.dll") ||
        !std::filesystem::is_directory(runtime_dir / L"etc")) {
        log(3, "HSMono: install the selected managed assembly and the Mono framework in the plugin configuration directory");
        return;
    }
    if (!open_runtime(runtime_dir)) return;

    char domain_name[] = "HSMono.Smoke";
    session = runtime->create_domain(domain_name, nullptr);
    if (session && runtime->set_domain(session, false)) {
        const auto path = assembly.u8string();
        const auto managed = runtime->open_assembly(session, reinterpret_cast<const char*>(path.c_str()));
        const auto image = managed ? runtime->assembly_image(managed) : nullptr;
        const auto smoke = image ? runtime->find_class(image, (warrior || sentinel) ? "Sentinel" : "HSMono",
            sentinel ? "HailstoneHost" : warrior ? "HailstoneWarriorTrial" : "Smoke") : nullptr;
        if (smoke) {
            const auto init = runtime->find_method(smoke, "OnInit", 0);
            pulse = runtime->find_method(smoke, "OnPulse", sentinel ? 0 : 1);
            stop = runtime->find_method(smoke, "OnStop", 0);
            poll = warrior ? runtime->find_method(smoke, "OnPoll", 0) : nullptr;
            if (sentinel) {
                sentinel_command = runtime->find_method(smoke, "OnCommand", 1);
                sentinel_chat = runtime->find_method(smoke, "OnIncomingChat", 1);
                sentinel_status = runtime->find_method(smoke, "Status", 0);
                sentinel_imgui = runtime->find_method(smoke, "OnUpdateImGui", 0);
                register_sentinel_calls();
                hs_mono::imgui_bridge::reset_state();
                if (!sentinel_imgui) log(2, "HSMono: managed assembly has no OnUpdateImGui; running headless");
            }
            started = init && pulse && stop && (!warrior || poll) &&
                (!sentinel || (sentinel_command && sentinel_chat && sentinel_status)) && invoke(init);
        }
        runtime->set_domain(root, true);
    }
    if (!started) {
        log(3, "HSMono: managed assembly load or OnInit failed");
        if (sentinel) WriteChatf("%s", "Sentinel C# startup failed. Unload competing Sentinel plugins and check hailstone.log.");
        close_session();
        return;
    }
    warrior_session = warrior;
    sentinel_session = sentinel;
    if (sentinel) {
        warrior_player = player.spawn_id;
        warrior_zone = player.zone_id;
        warrior_instance = player.zone_instance;
        log(1, "Sentinel C# runtime started");
        WriteChatf("%s", "Sentinel C# started. Use /sentinel status.");
        return;
    }
    log(1, warrior ? "Sentinel warrior dry run initialized" : "HSMono: managed OnInit succeeded");
}

void append_value(std::string& snapshot, std::string_view query, std::string_view text)
{
    if (text.find_first_of("\r\n\t") != std::string_view::npos) return;
    snapshot += '\n';
    snapshot += query;
    snapshot += '\t';
    snapshot += text;
}

void append_query(std::string& snapshot, const std::string& query)
{
    std::array<char, 512> value{};
    const auto n = ParseMacroData(query.c_str(), value.data(), value.size());
    if (n >= 0 && static_cast<std::size_t>(n) < value.size())
        append_value(snapshot, query, {value.data(), static_cast<std::size_t>(n)});
}

std::optional<std::vector<HS_BulkRaidData>> raid_roster()
{
    if (!HAILSTONE_HOST_HAS(host, get_raid_data_bulk) || !host->get_raid_data_bulk) return std::nullopt;
    std::array<unsigned char, 16384> data{};
    const auto written = host->get_raid_data_bulk(host->host, data.data(), static_cast<int32_t>(data.size()));
    if (written < static_cast<int32_t>(sizeof(HS_BulkRaidHeader)) ||
        static_cast<std::size_t>(written) > data.size()) return std::nullopt;

    HS_BulkRaidHeader header{};
    std::memcpy(&header, data.data(), sizeof header);
    const auto length = static_cast<std::size_t>(written);
    if (header.version != 1 || header.size < sizeof header || header.size > length ||
        header.recordSize < sizeof(HS_BulkRaidData) || header.count != header.total ||
        header.count > (length - header.size) / header.recordSize) return std::nullopt;
    std::vector<HS_BulkRaidData> members;
    members.reserve(header.count);
    for (uint32_t i = 0; i < header.count; ++i) {
        HS_BulkRaidData member{};
        std::memcpy(&member, data.data() + header.size + static_cast<std::size_t>(i) * header.recordSize,
                    sizeof member);
        members.push_back(member);
    }
    return members;
}

std::string raid_name(const HS_BulkRaidData& member)
{
    const std::string_view name(member.name, sizeof member.name);
    const auto end = name.find('\0');
    return end != std::string_view::npos ? std::string(name.substr(0, end)) : "";
}

MonoString* sentinel_raid_heal_members()
{
    HailstonePlayer player{};
    if (!active_player(player) || !player.in_raid) return runtime->make_string(session, "NULL");

    std::string result;
    if (const auto members = raid_roster()) {
        for (const auto& member : *members) {
            const auto name = raid_name(member);
            if (member.spawnID == 0 || name.empty() || name.find_first_of("\t\r\n") != std::string::npos)
                continue;
            result += std::to_string(member.spawnID) + '\t' + std::to_string(member.pctHPs) + '\t' + name + '\n';
        }
    }
    return runtime->make_string(session, result.c_str());
}

std::string raid_assist(const std::vector<HS_BulkRaidData>& members)
{
    std::string name;
    for (const auto& member : members) {
        if (!(member.flags & HS_BULK_RAID_MAIN_ASSIST)) continue;
        // The SDK has roles, with no ordered MQ raid assist slots.
        if (!name.empty()) return "";
        name = raid_name(member);
    }
    return name;
}

std::optional<std::string> sentinel_raid_query(std::string_view expression)
{
    HailstonePlayer player{};
    if (!active_player(player)) return std::nullopt;
    if (!player.in_raid)
        return expression == "${Raid.Members}" ? "0" : "NULL";

    const auto members = raid_roster();
    if (!members || members->empty())
        return expression == "${Raid.Members}" ? "1" : "NULL"; // keep raid gates closed when roster data is unavailable
    if (expression == "${Raid.Members}") return std::to_string(members->size());
    if (expression == "${Raid.MainAssist[1].Name}" || expression == "${Raid.MainAssist[1].CleanName}") {
        const auto name = raid_assist(*members);
        return name.empty() ? "NULL" : name;
    }
    constexpr std::string_view prefix = "${Raid.Member[";
    if (!expression.starts_with(prefix)) return std::nullopt;
    const auto end = expression.find(']', prefix.size());
    if (end == std::string_view::npos) return std::nullopt;
    unsigned index = 0;
    const auto ptr = expression.data() + end;
    const auto read = std::from_chars(expression.data() + prefix.size(), ptr, index);
    if (read.ec != std::errc{} || read.ptr != ptr || index == 0 || index > members->size()) return "NULL";

    const auto& member = (*members)[index - 1];
    const auto field = expression.substr(end + 1);
    if (field == ".Name}" || field == ".CleanName}") return raid_name(member);
    if (field == ".ID}") return std::to_string(member.spawnID);
    if (field == ".PctHPs}") return member.pctHPs >= 0 ? std::to_string(member.pctHPs) : "NULL";
    return std::nullopt;
}

bool append_raid(std::string& snapshot, const HailstonePlayer& player)
{
    if (!player.in_raid) {
        append_value(snapshot, "${Raid.Members}", "0");
        return true;
    }
    const auto members = raid_roster();
    if (!members || members->empty()) return false;
    append_value(snapshot, "${Raid.Members}", std::to_string(members->size()));
    const auto assist = raid_assist(*members);
    append_value(snapshot, "${Raid.MainAssist[1].CleanName}", assist);
    append_value(snapshot, "${Raid.MainAssist[1].Name}", assist);
    return true;
}

bool warrior_identity_matches(const HailstonePlayer& player)
{
    return player.spawn_id == warrior_player && player.zone_id == warrior_zone &&
        player.zone_instance == warrior_instance;
}

bool query_matches(const char* query, std::string_view expected)
{
    std::array<char, 32> value{};
    const int len = ParseMacroData(query, value.data(), value.size());
    return len >= 0 && static_cast<std::size_t>(len) < value.size() &&
        std::string_view(value.data(), static_cast<std::size_t>(len)) == expected;
}

std::optional<AttackTarget> live_target(const HailstonePlayer& player, const char** reason = nullptr)
{
    const auto blocked = [reason](const char* message) -> std::optional<AttackTarget> {
        if (reason) *reason = message;
        return std::nullopt;
    };
    if (player.in_raid) return blocked("the live attack test is solo-only and you are in a raid");
    if (player.in_group) return blocked("the live attack test is solo-only and you are in a group");
    if (player.class_id != 1) return blocked("the active character is not a warrior");
    if (!host->caps || !(host->caps(host->host) & HAILSTONE_CAP_ACTIONS))
        return blocked("the server did not grant the actions capability");
    if (!player.target_id) return blocked("select a living NPC first");
    if (!query_matches("${Me.Combat}", "FALSE"))
        return blocked("autoattack is already on or its state is unavailable");
    if (!query_matches("${Me.Dead}", "FALSE") || !query_matches("${Me.Stunned}", "FALSE"))
        return blocked("the character is dead, stunned, or its state is unavailable");
    if (!query_matches("${Target.Dead}", "FALSE") || !query_matches("${Target.Type}", "NPC"))
        return blocked("the selected target is not a living NPC");

    std::array<char, 32> value{};
    const auto len = ParseMacroData("${Target.Distance3D}", value.data(), value.size());
    if (len <= 0 || static_cast<std::size_t>(len) >= value.size()) return blocked("target distance is unavailable");
    double distance = 0;
    const auto end = value.data() + len;
    const auto parsed = std::from_chars(value.data(), end, distance);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(distance) ||
        distance < 0 || distance > 200) return blocked("the target is outside the 200-unit range");

    HailstoneSpawn spawn{};
    spawn.size = sizeof spawn;
    if (!host->get_spawn(host->host, player.target_id, &spawn) || !spawn.is_npc ||
        spawn.is_corpse || !spawn.incarnation) return blocked("the selected NPC has no current spawn identity");
    return AttackTarget{player.target_id, spawn.incarnation};
}

void deliver_attack(const AttackTarget& request)
{
    HailstonePlayer player{};
    const auto current = active_player(player) && warrior_identity_matches(player) ?
        live_target(player) : std::nullopt;
    if (!current || current->id != request.id || current->incarnation != request.incarnation ||
        std::chrono::steady_clock::now() > attack_deadline) {
        log(2, "Sentinel live attack discarded: target or player state changed");
        return;
    }
    DoCommand("/attack on");
    if (query_matches("${Me.Combat}", "TRUE")) {
        owned_attack = request;
        log(1, "Sentinel live attack enabled");
        WriteChatf("%s", "Sentinel live attack enabled");
    } else {
        log(2, "Sentinel live attack was not enabled by Hailstone");
        WriteChatf("%s", "Sentinel live attack was not enabled by Hailstone");
    }
}

bool invoke_warrior(MonoMethod* method, const char* snapshot = nullptr)
{
    bool propose_attack = false;
    bool ok = runtime->set_domain(session, false) != 0;
    if (ok) {
        auto* value = snapshot ? runtime->make_string(session, snapshot) : nullptr;
        void* args[] = {value};
        MonoObject* result = nullptr;
        ok = (!snapshot || value) && invoke(method, snapshot ? args : nullptr, &result);
        if (ok && result) {
            char* output = runtime->string_utf8(reinterpret_cast<MonoString*>(result));
            if (output) {
                constexpr std::string_view marker = "ATTACK_ON\n";
                propose_attack = std::string_view(output).starts_with(marker);
                log(1, output + (propose_attack ? marker.size() : 0));
                if (method == poll) warrior_pending = false;
                runtime->free_memory(output);
            } else {
                ok = false;
            }
        }
        runtime->set_domain(root, true);
    }
    if (method == poll && !warrior_pending && requested_attack) {
        const auto request = *requested_attack;
        requested_attack.reset();
        if (ok && propose_attack) deliver_attack(request);
        else if (ok) log(1, "Sentinel live attack was not proposed by the warrior");
    }
    if (!ok) log(3, "Sentinel warrior trial failed in managed callback");
    return ok;
}

void run_warrior(bool disengage, bool live)
{
    HailstonePlayer player{};
    if (!active_player(player) || player.class_id != 1) {
        log(2, "Sentinel warrior dry run requires an active warrior character");
        return;
    }
    if ((warrior_session || sentinel_session) && !warrior_identity_matches(player))
        close_session("Sentinel warrior dry run closed: character or zone changed");
    if (!warrior_session) {
        if (disengage) {
            log(2, "Start a warrior dry run first");
            return;
        }
        close_session();
        start_session(true);
        if (!warrior_session) return;
        warrior_player = player.spawn_id;
        warrior_zone = player.zone_id;
        warrior_instance = player.zone_instance;
    }

    if (disengage) {
        requested_attack.reset();
        release_attack();
    }
    if (warrior_pending) {
        log(1, "Wait for the current warrior decision");
        return;
    }
    const char* blocked = nullptr;
    const auto attack = live ? live_target(player, &blocked) : std::nullopt;
    if (live && !attack) {
        const auto message = std::string("Sentinel live attack blocked: ") + blocked;
        log(2, message.c_str());
        WriteChatf("%s", message.c_str());
        return;
    }
    std::string snapshot = disengage ? "disengage" : live ? "attack" : "engage";
    if (!disengage && !append_raid(snapshot, player)) {
        log(2, "Sentinel warrior dry run: raid roster is unavailable");
        return;
    }
    // An empty server keeps this trial on Sentinel defaults instead of MQ config.
    append_value(snapshot, "${EverQuest.Server}", "");
    if (!player.in_group) append_value(snapshot, "${Group.Members}", "0");
    else for (const char* query : {"${Group.Members}", "${Group.MainAssist.CleanName}",
                                   "${Group.MainAssist.Name}", "${Group.MainTank.CleanName}",
                                   "${Group.MainTank.Name}"})
        append_query(snapshot, query);
    for (const char* query : {"${Me.Class.ShortName}", "${Me.CleanName}", "${Me.Dead}", "${Me.Hovering}",
                            "${Me.Stunned}", "${Me.Combat}", "${Me.PctEndurance}",
                            "${Me.Casting.ID}", "${Me.Pet.ID}", "${Target.ID}",
                            "${Target.Distance3D}", "${Target.Dead}", "${Target.Type}",
                            "${Stick.Active}", "${Nav.Active}", "${Zone.ShortName}"})
        append_query(snapshot, query);
    for (uint32_t slot = 1; slot <= player.xtarget_slots && slot <= 64; ++slot) {
        const auto xtarget = "${Me.XTarget[" + std::to_string(slot) + "].";
        append_query(snapshot, xtarget + "ID}");
        append_query(snapshot, xtarget + "TargetType}");
    }
    if (player.target_id) {
        const auto spawn = "${Spawn[id " + std::to_string(player.target_id) + "].";
        for (const char* member : {"ID", "Dead", "Type", "Distance3D"})
            append_query(snapshot, spawn + member + '}');
    }

    requested_attack = attack;
    attack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    warrior_pending = true;
    if (!invoke_warrior(pulse, snapshot.c_str())) close_session();
}

void smoke_command(void*, const char* args)
{
    const std::string_view action(args ? args : "");
    if (action == "stop") {
        sentinel_resume = false;
        close_session();
        return;
    }
    if (action == "sentinel") {
        if (sentinel_session) return;
        close_session();
        try { start_session(false, true); }
        catch (const std::exception&) { close_session(); log(3, "Sentinel C# runtime startup failed"); }
        return;
    }
    if (sentinel_session) {
        WriteChatf("%s", "Sentinel C# is running. Use /sentinel or /mono unload Sentinel.");
        return;
    }
    if (action != "start" && action != "warrior" && action != "disengage" && action != "attack") {
        log(1, "HSMono: use /hmonosmoke sentinel, start, stop, warrior, disengage, or attack");
        return;
    }
    try {
        if (action == "start") start_session();
        else run_warrior(action == "disengage", action == "attack");
    } catch (const std::exception&) {
        close_session();
        log(3, "HSMono: could not prepare the managed smoke session");
    }
}

void mono_command(void* context, const char* args)
{
    std::string input(args ? args : "");
    std::transform(input.begin(), input.end(), input.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::istringstream words(input);
    std::string action, app, extra;
    words >> action >> app >> extra;
    const bool loading = action == "load" || action == "run" || action == "start";
    const bool unloading = action == "unload" || action == "stop";

    if (action == "list" && app.empty()) {
        const char* status = sentinel_session ? "Sentinel" :
            sentinel_resume ? "Sentinel (waiting for zone)" : "No managed apps running.";
        WriteChatf("HSMono: %s", status);
    } else if (action == "version" && app.empty()) {
        WriteChatf("%s", "HSMono version " HAILSTONE_MONO_VERSION);
    } else if ((action == "sentinel" && app.empty()) ||
               (loading && app == "sentinel" && extra.empty())) {
        smoke_command(context, "sentinel");
    } else if (unloading && extra.empty() && (app == "sentinel" || app == "all")) {
        if (sentinel_session || sentinel_resume) {
            sentinel_resume = false;
            if (sentinel_session) close_session();
            WriteChatf("%s", "HSMono: Sentinel unloaded.");
        } else {
            WriteChatf("%s", "HSMono: No managed apps running.");
        }
    } else if ((loading || unloading) && !app.empty() && extra.empty()) {
        WriteChatf("%s", "HSMono: Unknown managed app. Supported app: Sentinel.");
    } else {
        WriteChatf("%s", "Usage: /mono load Sentinel | unload Sentinel | unload all | list | version");
    }
}

} // namespace

HAILSTONE_API void InitializePlugin()
{
    mono_command_registered = AddCommand(mono_command_name, mono_command, nullptr) != 0;
    if (!mono_command_registered) log(2, "HSMono: could not register /mono");
    smoke_command_registered = AddCommand(smoke_command_name, smoke_command, nullptr) != 0;
    if (!smoke_command_registered) log(2, "HSMono: could not register /hmonosmoke");
}

HAILSTONE_API void OnPulse()
{
    if (!session) return;
    HailstonePlayer player{};
    if (!active_player(player) || ((warrior_session || sentinel_session) && !warrior_identity_matches(player))) {
        close_session("Sentinel warrior dry run closed: character or zone changed");
        return;
    }
    if (sentinel_session) {
        bool ok = runtime->set_domain(session, false) != 0;
        bool should_stop = false;
        if (ok) {
            MonoObject* status = nullptr;
            ok = invoke(pulse) && invoke(sentinel_status, nullptr, &status);
            if (ok && status) {
                char* value = runtime->string_utf8(reinterpret_cast<MonoString*>(status));
                should_stop = value && std::string_view(value) == "stop";
                if (value) runtime->free_memory(value);
            }
            runtime->set_domain(root, true);
        }
        if (!ok) log(3, "Sentinel C# pulse failed");
        if (!ok || should_stop) close_session();
        return;
    }
    if (warrior_session) {
        if (owned_attack) {
            HailstoneSpawn spawn{};
            spawn.size = sizeof spawn;
            if (player.target_id != owned_attack->id || !host->get_spawn(host->host, player.target_id, &spawn) ||
                spawn.incarnation != owned_attack->incarnation || spawn.is_corpse ||
                !query_matches("${Target.Dead}", "FALSE") || !query_matches("${Me.Dead}", "FALSE"))
                release_attack();
            else if (query_matches("${Me.Combat}", "FALSE")) owned_attack.reset();
        }
        if (warrior_pending && !invoke_warrior(poll)) close_session();
        return;
    }
    if (reported_pulse) return;

    std::array<char, 16> id{};
    std::snprintf(id.data(), id.size(), "%u", static_cast<unsigned int>(player.spawn_id));
    bool ok = runtime->set_domain(session, false) != 0;
    if (ok) {
        auto* value = runtime->make_string(session, id.data());
        void* args[] = {value};
        ok = value && invoke(pulse, args);
        runtime->set_domain(root, true);
    }
    if (!ok) {
        log(3, "HSMono: managed OnPulse failed");
        close_session();
        return;
    }
    reported_pulse = true;
    log(1, "HSMono: managed OnPulse received the active character snapshot");
}

// A pet's tells ("<pet> tells you, 'Attacking <target>'") are the pet reporting to its owner.
// Sentinel relays the tells it sees to the crew, where every pet's reports are spam, so the
// player's own pet's tells are not handed to it. The line still shows in this character's own
// chat; every other tell, and another player's pet's, passes unchanged.
bool own_pet_tell(const char* text)
{
    if (!text || !host) return false;
    if (!HAILSTONE_HOST_HAS(host, get_player) || !host->get_player) return false;
    if (!HAILSTONE_HOST_HAS(host, get_spawn) || !host->get_spawn) return false;
    const std::string_view line = text;
    constexpr std::string_view marker = " tells you, '";
    const auto at = line.find(marker);
    if (at == std::string_view::npos || at == 0) return false;
    const std::string_view sender = line.substr(0, at);
    HailstonePlayer player{};
    player.size = sizeof player;
    if (!host->get_player(host->host, &player) || player.pet_id == 0) return false;
    HailstoneSpawn pet{};
    pet.size = sizeof pet;
    if (!host->get_spawn(host->host, player.pet_id, &pet)) return false;
    const std::string_view name = pet.name;
    if (name.size() != sender.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto a = std::tolower(static_cast<unsigned char>(name[i]));
        const auto b = std::tolower(static_cast<unsigned char>(sender[i]));
        // Zone spawn names use '_' where the chat sender's name has a space.
        if ((a == '_' ? ' ' : a) != (b == '_' ? ' ' : b)) return false;
    }
    return true;
}

HAILSTONE_API int OnIncomingChat(const char* text, uint32_t)
{
    if (sentinel_session && text && !own_pet_tell(text) && !invoke_text(sentinel_chat, text))
        log(3, "Sentinel chat callback failed");
    return 0;
}

// Hailstone calls this inside its ImGui frame for plugins with the `ui`
// capability. The client's context is current on its side, but this module
// links its own Dear ImGui copy, so it must point that copy at the same
// context before the managed code submits any widget.
HAILSTONE_API void OnUpdateImGui()
{
    if (!sentinel_session || !session || !sentinel_imgui) return;
    if (!HAILSTONE_HOST_HAS(host, imgui_context) || !host->imgui_context) return;
    void* context = host->imgui_context(host->host);
    if (!context) return;
    hs_mono::imgui_bridge::set_context(context);

    auto* previous = runtime->current_domain();
    if (!runtime->set_domain(session, false)) return;
    const bool ok = invoke(sentinel_imgui);
    runtime->set_domain(previous ? previous : root, true);
    if (!ok) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            log(3, "Sentinel C# UI draw failed; the session continues headless");
        }
    }
}

HAILSTONE_API void OnBeginZone()
{
    sentinel_resume = sentinel_session || sentinel_resume;
    close_session("Sentinel warrior dry run closed: zoning");
}

HAILSTONE_API void OnZoned()
{
    if (!sentinel_resume || session) return;
    sentinel_resume = false;
    try { start_session(false, true); }
    catch (const std::exception&) { close_session(); log(3, "Sentinel C# restart failed"); }
}

HAILSTONE_API void ShutdownPlugin()
{
    sentinel_resume = false;
    close_session();
    if (smoke_command_registered) RemoveCommand(smoke_command_name);
    if (mono_command_registered) RemoveCommand(mono_command_name);
    smoke_command_registered = false;
    mono_command_registered = false;
    runtime.reset();
    root = nullptr;
}
