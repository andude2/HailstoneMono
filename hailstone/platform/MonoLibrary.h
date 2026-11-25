#pragma once

#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-publib.h>

#include <filesystem>

namespace hs_mono {

class MonoLibrary {
public:
    MonoLibrary() = default;
    ~MonoLibrary();
    MonoLibrary(const MonoLibrary&) = delete;
    MonoLibrary& operator=(const MonoLibrary&) = delete;

    bool open(const std::filesystem::path& path);
    bool pin() const;

    decltype(&mono_domain_get) current_domain = nullptr;
    decltype(&mono_get_root_domain) root_domain = nullptr;
    decltype(&mono_set_dirs) set_dirs = nullptr;
    decltype(&mono_jit_init_version) init = nullptr;
    decltype(&mono_domain_create_appdomain) create_domain = nullptr;
    decltype(&mono_domain_set) set_domain = nullptr;
    decltype(&mono_domain_unload) unload_domain = nullptr;
    decltype(&mono_domain_assembly_open) open_assembly = nullptr;
    decltype(&mono_assembly_get_image) assembly_image = nullptr;
    decltype(&mono_class_from_name) find_class = nullptr;
    decltype(&mono_class_get_method_from_name) find_method = nullptr;
    decltype(&mono_runtime_invoke) invoke = nullptr;
    decltype(&mono_string_new) make_string = nullptr;
    decltype(&mono_object_unbox) unbox = nullptr;

    decltype(&mono_string_to_utf8) string_utf8 = nullptr;
    decltype(&mono_free) free_memory = nullptr;

    decltype(&mono_add_internal_call) add_internal_call = nullptr;

    decltype(&mono_get_byte_class) byte_class = nullptr;
    decltype(&mono_array_new) array_new = nullptr;
    decltype(&mono_array_addr_with_size) array_addr = nullptr;

private:
    void* module_ = nullptr;
};

} // namespace hs_mono
