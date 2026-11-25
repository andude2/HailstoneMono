#pragma once

namespace hs_mono {

class MonoLibrary;

// The managed UI (Sentinel's E3ImGUI declarations) draws through the client's
// Dear ImGui context. Hailstone hands a plugin that context in OnUpdateImGui,
// and the plugin must be built with the same Dear ImGui version (the SDK's
// imgui_context comment). This bridge registers the internal calls Sentinel
// uses and keeps the small amount of per-window state MQ2Mono kept natively.
namespace imgui_bridge {

// Called once after the Mono library is loaded and the runtime is ready.
void install(MonoLibrary* runtime);

// Point this module's copy of Dear ImGui at the client's context. OnUpdateImGui
// runs inside the client's frame, so the context is already current in the
// client; a plugin has its own copy of the ImGui statics and must set it too.
void set_context(void* context);

// Drop per-session state (open windows, edit buffers, filters, clippers).
void reset_state();

} // namespace imgui_bridge
} // namespace hs_mono
