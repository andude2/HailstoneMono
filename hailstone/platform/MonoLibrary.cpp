#include "MonoLibrary.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bit>
#include <type_traits>

namespace hs_mono {

MonoLibrary::~MonoLibrary()
{
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}

bool MonoLibrary::open(const std::filesystem::path& path)
{
    const auto module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) return false;
    module_ = module;

    const auto bind = [module](auto& target, const char* name) {
        target = std::bit_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(module, name));
        return target != nullptr;
    };
    return bind(current_domain, "mono_domain_get") &&
        bind(root_domain, "mono_get_root_domain") &&
        bind(set_dirs, "mono_set_dirs") &&
        bind(init, "mono_jit_init_version") &&
        bind(create_domain, "mono_domain_create_appdomain") &&
        bind(set_domain, "mono_domain_set") &&
        bind(unload_domain, "mono_domain_unload") &&
        bind(open_assembly, "mono_domain_assembly_open") &&
        bind(assembly_image, "mono_assembly_get_image") &&
        bind(find_class, "mono_class_from_name") &&
        bind(find_method, "mono_class_get_method_from_name") &&
        bind(invoke, "mono_runtime_invoke") &&
        bind(make_string, "mono_string_new") &&
        bind(unbox, "mono_object_unbox") &&
        bind(string_utf8, "mono_string_to_utf8") &&
        bind(free_memory, "mono_free") &&
        bind(add_internal_call, "mono_add_internal_call") &&
        bind(byte_class, "mono_get_byte_class") &&
        bind(array_new, "mono_array_new") &&
        bind(array_addr, "mono_array_addr_with_size");
}

bool MonoLibrary::pin() const
{
    // Keep the runtime's root domain alive when Hailstone unloads the plugin.
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        static_cast<const wchar_t*>(module_), &pinned) != 0;
}

} // namespace hs_mono
