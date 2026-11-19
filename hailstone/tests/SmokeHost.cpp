#include "hailstone_plugin.h"
#include "ImGuiBridge.h"
#include "platform/MonoLibrary.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

extern "C" void hailstone_plugin_set_host(const PluginHostApi*);
extern "C" void InitializePlugin();
extern "C" void ShutdownPlugin();
extern "C" void OnPulse();
extern "C" void OnBeginZone();

namespace {

HailstoneCommandFn smoke_command = nullptr;
std::string config_dir;
std::string message;
bool in_zone = false;
bool throw_on_pulse = false;
int init_count = 0;
int pulse_count = 0;
int stop_count = 0;
int unload_count = 0;
int tokens[9]{};

template<typename T>
T* token(int index)
{
    return reinterpret_cast<T*>(&tokens[index]);
}

MonoDomain* current = nullptr;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "Failed: %s (last log: %s)\n", what, message.c_str());
    std::exit(1);
}

} // namespace

namespace hailstone_mono {

MonoLibrary::~MonoLibrary() = default;

bool MonoLibrary::open(const std::filesystem::path&)
{
    root_domain = [] { return token<MonoDomain>(0); };
    create_domain = [](char*, char*) { return token<MonoDomain>(1); };
    set_domain = [](MonoDomain* domain, mono_bool) -> mono_bool { current = domain; return 1; };
    unload_domain = [](MonoDomain* domain) {
        check(domain == token<MonoDomain>(1) && current == token<MonoDomain>(0), "unload runs from root");
        ++unload_count;
    };
    open_assembly = [](MonoDomain* domain, const char*) {
        check(domain == current, "open in session domain");
        return token<MonoAssembly>(2);
    };
    assembly_image = [](MonoAssembly*) { return token<MonoImage>(3); };
    find_class = [](MonoImage*, const char*, const char*) { return token<MonoClass>(4); };
    find_method = [](MonoClass*, const char* name, int argc) {
        if (std::string_view(name) == "OnInit" && argc == 0) return token<MonoMethod>(5);
        if (std::string_view(name) == "OnPulse" && argc == 1) return token<MonoMethod>(6);
        if (std::string_view(name) == "OnStop" && argc == 0) return token<MonoMethod>(7);
        return static_cast<MonoMethod*>(nullptr);
    };
    make_string = [](MonoDomain* domain, const char* value) {
        check(domain == current && std::string_view(value) == "42", "SDK character snapshot marshalled");
        return token<MonoString>(8);
    };
    invoke = [](MonoMethod* method, void*, void** args, MonoObject** exception) -> MonoObject* {
        check(current == token<MonoDomain>(1), "managed call runs inside session");
        if (method == token<MonoMethod>(5)) ++init_count;
        if (method == token<MonoMethod>(6)) {
            ++pulse_count;
            check(args && args[0] == token<MonoString>(8), "managed pulse receives a string");
            if (throw_on_pulse) *exception = token<MonoObject>(8);
        }
        if (method == token<MonoMethod>(7)) ++stop_count;
        return nullptr;
    };
    return true;
}

bool MonoLibrary::pin() const { return true; }

namespace imgui_bridge {
void install(MonoLibrary*) {}
void set_context(void*) {}
void reset_state() {}
} // namespace imgui_bridge

} // namespace hailstone_mono

int main(int argc, char** argv)
{
    check(argc == 2, "supply fixture config directory");
    config_dir = argv[1];
    PluginHostApi api{};
    api.log = [](void*, int, const char* text) { message = text; };
    api.add_command = [](void*, const char* name, HailstoneCommandFn fn, void*) {
        const std::string_view registered(name);
        check(registered == "/mono" || registered == "/hmonosmoke", "command registration");
        if (registered == "/hmonosmoke") smoke_command = fn;
        return 1;
    };
    api.remove_command = [](void*, const char* name) {
        if (std::string_view(name) == "/hmonosmoke") smoke_command = nullptr;
    };
    api.get_player = [](void*, HailstonePlayer* out) {
        check(out->size == sizeof(*out), "player struct size is set");
        out->in_zone = in_zone;
        out->spawn_id = in_zone ? 42u : 0u;
        return 1;
    };
    api.config_dir = [](void*, char* out, std::size_t capacity) {
        check(capacity > config_dir.size(), "fixture path fits");
        std::memcpy(out, config_dir.c_str(), config_dir.size() + 1);
        return static_cast<int>(config_dir.size());
    };
    hailstone_plugin_set_host(&api);
    InitializePlugin();
    check(smoke_command != nullptr, "command available");
    smoke_command(nullptr, "start");
    check(init_count == 0, "startup outside a zone is refused");

    in_zone = true;
    smoke_command(nullptr, "start");
    smoke_command(nullptr, "start");
    OnPulse();
    OnPulse();
    check(init_count == 1 && pulse_count == 1, "one init and one managed smoke pulse");
    OnBeginZone();
    OnPulse();
    check(stop_count == 1 && unload_count == 1 && pulse_count == 1, "zone teardown");

    smoke_command(nullptr, "start");
    throw_on_pulse = true;
    OnPulse();
    check(init_count == 2 && pulse_count == 2 && stop_count == 2 && unload_count == 2,
          "managed exception closes the session");

    smoke_command(nullptr, "start");
    ShutdownPlugin();
    check(stop_count == 3 && unload_count == 3 && !smoke_command, "shutdown releases the session and command");

    InitializePlugin();
    smoke_command(nullptr, "start");
    smoke_command(nullptr, "stop");
    ShutdownPlugin();
    check(init_count == 4 && stop_count == 4 && unload_count == 4, "reload and explicit stop");
    std::puts("Mono smoke host lifecycle passed");
}
