// Draws one real ImGui frame through the managed UI smoke assembly. This is
// the bridge's own test: it proves the MonoCore.E3ImGUI internal calls are
// registered and that the stateful wrappers (open flags, edit buffers, text
// filters, list clippers) survive a NewFrame/Render cycle without a game.
#include "ImGuiBridge.h"
#include "platform/MonoLibrary.h"

#include <imgui.h>

#include <cstdio>
#include <filesystem>

namespace {

using hs_mono::MonoLibrary;

bool failed(const char* what)
{
    std::fprintf(stderr, "ui_bridge_test: %s\n", what);
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: ui_bridge_test <mono-runtime-dir> <UiSmoke.dll>\n");
        return 2;
    }

    ImGui::CreateContext();
    ImGui::SetCurrentContext(ImGui::GetCurrentContext());
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 480.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    // ImGui 1.93: no renderer backend here, so claim texture support and let
    // NewFrame build the atlas (the client's tests do the same); nothing
    // uploads the result. Without this NewFrame logs that the atlas is not
    // built and the backend does not support RendererHasTextures.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    hs_mono::imgui_bridge::set_context(ImGui::GetCurrentContext());
    ImGui::NewFrame();

    const std::filesystem::path runtime_dir(argv[1]);
    MonoLibrary library;
    if (!library.open(runtime_dir / L"mono-2.0-sgen.dll")) {
        ImGui::EndFrame();
        ImGui::DestroyContext();
        failed("could not load the Mono runtime");
        return 1;
    }
    const auto lib = (runtime_dir / L"lib").u8string();
    const auto etc = (runtime_dir / L"etc").u8string();
    library.set_dirs(reinterpret_cast<const char*>(lib.c_str()),
                     reinterpret_cast<const char*>(etc.c_str()));
    MonoDomain* root = library.init("UiBridgeTest", "v4.0.30319");
    if (!root) {
        ImGui::EndFrame();
        ImGui::DestroyContext();
        failed("Mono initialization failed");
        return 1;
    }

    hs_mono::imgui_bridge::install(&library);
    auto* assembly = library.open_assembly(root, argv[2]);
    auto* image = assembly ? library.assembly_image(assembly) : nullptr;
    auto* type = image ? library.find_class(image, "MonoCore", "E3ImGUI") : nullptr;
    auto* draw = type ? library.find_method(type, "Draw", 0) : nullptr;
    if (!draw) {
        ImGui::EndFrame();
        ImGui::DestroyContext();
        failed("UiSmoke.Draw was not found");
        return 1;
    }

    MonoObject* exception = nullptr;
    MonoObject* result = library.invoke(draw, nullptr, nullptr, &exception);
    int ok = result && library.unbox ? *static_cast<int*>(library.unbox(result)) : 0;

    // A second frame catches state left behind by the first (filters, clippers,
    // edit buffers, open flags).
    ImGui::EndFrame();
    ImGui::NewFrame();
    MonoObject* second_exception = nullptr;
    MonoObject* second = library.invoke(draw, nullptr, nullptr, &second_exception);
    if (second && library.unbox) ok += *static_cast<int*>(library.unbox(second));

    ImGui::EndFrame();
    ImGui::Render();
    ImGui::DestroyContext();

    if (exception || second_exception) {
        std::fprintf(stderr, "ui_bridge_test: managed draw raised an exception\n");
        return 1;
    }
    if (ok != 2) {
        std::fprintf(stderr, "ui_bridge_test: Draw returned %d, expected 2\n", ok);
        return 1;
    }
    std::puts("PASS: ImGui bridge drew two frames through the managed smoke assembly");
    return 0;
}
