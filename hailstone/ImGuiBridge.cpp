// The managed UI in Sentinel draws through MonoCore.E3ImGUI's internal calls,
// the surface MQ2Mono provides in MacroQuest. This file supplies the same
// surface from Hailstone's Dear ImGui context. The plugin is built with the
// client's Dear ImGui version (the SDK's imgui_context contract); OnUpdateImGui
// receives the context pointer from the host before the managed draw pass.
#include "ImGuiBridge.h"

#include "platform/MonoLibrary.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace hs_mono::imgui_bridge {
namespace {

MonoLibrary* runtime = nullptr;

// Per-session state MQ2Mono keeps per Mono app domain. HSMono runs one
// managed session at a time, so the maps are cleared when a session starts.
std::unordered_map<std::string, bool> open_windows;
std::unordered_map<std::string, bool> checkbox_values;
std::unordered_map<std::string, int> input_int_values;
std::unordered_map<std::string, std::array<char, 4096>> input_text_values;
std::unordered_map<std::string, std::unique_ptr<ImGuiTextFilter>> text_filters;
std::unordered_map<std::string, std::unique_ptr<ImGuiListClipper>> list_clippers;

std::string mono_text(MonoString* value)
{
    if (!value || !runtime || !runtime->string_utf8) return {};
    char* text = runtime->string_utf8(value);
    std::string result = text ? text : "";
    if (text) runtime->free_memory(text);
    return result;
}

MonoString* make_text(const std::string& value)
{
    if (!runtime || !runtime->make_string || !runtime->current_domain) return nullptr;
    auto* domain = runtime->current_domain();
    if (!domain && runtime->root_domain) domain = runtime->root_domain();
    return domain ? runtime->make_string(domain, value.c_str()) : nullptr;
}

// Copying the managed model into the edit buffer, and the buffer back out, is
// the same contract MQ2Mono's std::string-backed wrappers keep: a frame where
// the caller passes a changed value starts the edit from it, and a frame where
// the caller does not keeps what the user typed.
void set_buffer(std::array<char, 4096>& buffer, const std::string& value)
{
    if (std::strncmp(buffer.data(), value.c_str(), buffer.size() - 1) == 0) return;
    const std::size_t length = std::min(value.size(), buffer.size() - 1);
    std::memcpy(buffer.data(), value.data(), length);
    buffer[length] = '\0';
}

void add(const char* name, auto fn)
{
    if (runtime && runtime->add_internal_call)
        runtime->add_internal_call(name, reinterpret_cast<const void*>(fn));
}

// ---- windows and their open flag -----------------------------------------

bool imgui_Begin(MonoString* name, int flags)
{
    const auto title = mono_text(name);
    if (title.empty()) return false;
    auto [it, inserted] = open_windows.try_emplace(title, true);
    (void)inserted;
    return ImGui::Begin(title.c_str(), &it->second, flags);
}

void imgui_End() { ImGui::End(); }

void imgui_Begin_OpenFlagSet(MonoString* name, bool open)
{
    const auto title = mono_text(name);
    if (!title.empty()) open_windows[title] = open;
}

bool imgui_Begin_OpenFlagGet(MonoString* name)
{
    const auto it = open_windows.find(mono_text(name));
    return it != open_windows.end() && it->second;
}

// ---- text ----------------------------------------------------------------

void imgui_Text(MonoString* text)
{
    const auto value = mono_text(text);
    ImGui::TextUnformatted(value.c_str());
}

void imgui_TextUnformatted(MonoString* text)
{
    const auto value = mono_text(text);
    ImGui::TextUnformatted(value.c_str());
}

void imgui_TextDisabled(MonoString* text)
{
    const auto value = mono_text(text);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

void imgui_TextWrapped(MonoString* text)
{
    const auto value = mono_text(text);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
}

void imgui_TextColored(float r, float g, float b, float a, MonoString* text)
{
    const auto value = mono_text(text);
    ImGui::TextColored(ImVec4(r, g, b, a), "%s", value.c_str());
}

float imgui_CalcTextSizeX(MonoString* text)
{
    return ImGui::CalcTextSize(mono_text(text).c_str()).x;
}

void imgui_PushTextWrapPos(float position) { ImGui::PushTextWrapPos(position); }
void imgui_PopTextWrapPos() { ImGui::PopTextWrapPos(); }

// ---- simple widgets ------------------------------------------------------

bool imgui_Button(MonoString* label) { return ImGui::Button(mono_text(label).c_str()); }

bool imgui_ButtonEx(MonoString* label, float width, float height)
{
    return ImGui::Button(mono_text(label).c_str(), ImVec2(width, height));
}

bool imgui_RightAlignButton(MonoString* label)
{
    const auto text = mono_text(label);
    const float buttonWidth = ImGui::CalcTextSize(text.c_str()).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > buttonWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - buttonWidth));
    return ImGui::Button(text.c_str());
}

bool imgui_SmallButton(MonoString* label) { return ImGui::SmallButton(mono_text(label).c_str()); }

bool imgui_InvisibleButton(MonoString* label, float width, float height, int flags)
{
    return ImGui::InvisibleButton(mono_text(label).c_str(), ImVec2(width, height), flags);
}

bool imgui_Selectable(MonoString* label, bool selected)
{
    return ImGui::Selectable(mono_text(label).c_str(), selected);
}

bool imgui_MenuItem(MonoString* label) { return ImGui::MenuItem(mono_text(label).c_str()); }

bool imgui_CollapsingHeader(MonoString* label, int flags)
{
    return ImGui::CollapsingHeader(mono_text(label).c_str(), static_cast<ImGuiTreeNodeFlags>(flags));
}

bool imgui_TreeNode(MonoString* label) { return ImGui::TreeNode(mono_text(label).c_str()); }
void imgui_TreePop() { ImGui::TreePop(); }

bool imgui_Checkbox(MonoString* name, bool value)
{
    const auto id = mono_text(name);
    if (id.empty()) return false;
    auto& stored = checkbox_values[id];
    stored = value;
    return ImGui::Checkbox(id.c_str(), &stored);
}

bool imgui_Checkbox_Get(MonoString* name)
{
    const auto it = checkbox_values.find(mono_text(name));
    return it != checkbox_values.end() && it->second;
}

void imgui_Checkbox_Clear(MonoString* name) { checkbox_values.erase(mono_text(name)); }

void imgui_Separator() { ImGui::Separator(); }
void imgui_SeparatorText(MonoString* text) { ImGui::SeparatorText(mono_text(text).c_str()); }
void imgui_SameLine() { ImGui::SameLine(); }
void imgui_Spacing() { ImGui::Spacing(); }
void imgui_NewLine() { ImGui::NewLine(); }
void imgui_Dummy(float width, float height) { ImGui::Dummy(ImVec2(width, height)); }
void imgui_BeginGroup() { ImGui::BeginGroup(); }
void imgui_EndGroup() { ImGui::EndGroup(); }
void imgui_BeginDisabled(bool disabled) { ImGui::BeginDisabled(disabled); }
void imgui_EndDisabled() { ImGui::EndDisabled(); }
void imgui_PushID(int id) { ImGui::PushID(id); }
void imgui_PopID() { ImGui::PopID(); }

// ---- layout --------------------------------------------------------------

void imgui_SetNextItemWidth(float width) { ImGui::SetNextItemWidth(width); }
void imgui_PushItemWidth(float width) { ImGui::PushItemWidth(width); }
void imgui_PopItemWidth(float) { ImGui::PopItemWidth(); }
void imgui_SetTooltip(MonoString* text) { ImGui::SetTooltip("%s", mono_text(text).c_str()); }
void imgui_BeginTooltip() { ImGui::BeginTooltip(); }
void imgui_EndTooltip() { ImGui::EndTooltip(); }

void imgui_SetNextWindowSizeWithCond(float width, float height, int cond)
{
    ImGui::SetNextWindowSize(ImVec2(width, height), cond);
}

void imgui_SetNextWindowBgAlpha(float alpha) { ImGui::SetNextWindowBgAlpha(alpha); }

void imgui_SetNextWindowPos(float x, float y, int cond, float pivotX, float pivotY)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), cond, ImVec2(pivotX, pivotY));
}

float imgui_GetContentRegionAvailX() { return ImGui::GetContentRegionAvail().x; }
float imgui_GetContentRegionAvailY() { return ImGui::GetContentRegionAvail().y; }
// GetWindowContentRegionMin/Max are compiled out by IMGUI_DISABLE_OBSOLETE_FUNCTIONS,
// which the client uses. These are the same two expressions imgui.cpp uses.
float imgui_GetWindowContentRegionMinX()
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    return window ? window->ContentRegionRect.Min.x - window->Pos.x : 0.0f;
}
float imgui_GetWindowContentRegionMaxX()
{
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    return window ? window->ContentRegionRect.Max.x - window->Pos.x : 0.0f;
}
float imgui_GetCursorPosX() { return ImGui::GetCursorPosX(); }
void imgui_SetCursorPosX(float x) { ImGui::SetCursorPosX(x); }
float imgui_GetCursorPosY() { return ImGui::GetCursorPosY(); }
void imgui_SetCursorPosY(float y) { ImGui::SetCursorPosY(y); }
float imgui_GetCursorScreenPosX() { return ImGui::GetCursorScreenPos().x; }
float imgui_GetCursorScreenPosY() { return ImGui::GetCursorScreenPos().y; }
float imgui_GetTextLineHeight() { return ImGui::GetTextLineHeight(); }

// ---- input ---------------------------------------------------------------

bool imgui_InputText(MonoString* id, MonoString* initial)
{
    const auto key = mono_text(id);
    if (key.empty()) return false;
    auto& buffer = input_text_values[key];
    set_buffer(buffer, mono_text(initial));
    return ImGui::InputText(key.c_str(), buffer.data(), static_cast<int>(buffer.size()));
}

MonoString* imgui_InputText_Get(MonoString* id)
{
    const auto it = input_text_values.find(mono_text(id));
    return make_text(it == input_text_values.end() ? std::string() : std::string(it->second.data()));
}

void imgui_InputText_Clear(MonoString* id) { input_text_values.erase(mono_text(id)); }

bool imgui_InputTextMultiline(MonoString* id, MonoString* initial, float width, float height)
{
    const auto key = mono_text(id);
    if (key.empty()) return false;
    auto& buffer = input_text_values[key];
    set_buffer(buffer, mono_text(initial));
    return ImGui::InputTextMultiline(key.c_str(), buffer.data(), static_cast<int>(buffer.size()),
                                     ImVec2(width, height));
}

bool imgui_InputInt(MonoString* id, int initial, int steps, int fastSteps)
{
    const auto key = mono_text(id);
    if (key.empty()) return false;
    int& stored = input_int_values[key];
    stored = initial;
    return ImGui::InputInt(key.c_str(), &stored, steps, fastSteps);
}

int imgui_InputInt_Get(MonoString* id)
{
    const auto it = input_int_values.find(mono_text(id));
    return it == input_int_values.end() ? -1 : it->second;
}

void imgui_InputInt_Clear(MonoString* id) { input_int_values.erase(mono_text(id)); }

bool imgui_SliderInt(MonoString* id, int* value, int minValue, int maxValue)
{
    if (!value) return false;
    const auto key = mono_text(id);
    if (key.empty()) return false;
    int edited = *value;
    const bool changed = ImGui::SliderInt(key.c_str(), &edited, minValue, maxValue);
    if (changed) *value = edited;
    return changed;
}

// ---- tabs, combo, tables -------------------------------------------------

bool imgui_BeginTabBar(MonoString* name) { return ImGui::BeginTabBar(mono_text(name).c_str()); }
void imgui_EndTabBar() { ImGui::EndTabBar(); }
bool imgui_BeginTabItem(MonoString* label) { return ImGui::BeginTabItem(mono_text(label).c_str()); }
void imgui_EndTabItem() { ImGui::EndTabItem(); }

bool imgui_BeginCombo(MonoString* label, MonoString* preview, int flags)
{
    return ImGui::BeginCombo(mono_text(label).c_str(), mono_text(preview).c_str(), flags);
}

void imgui_EndCombo() { ImGui::EndCombo(); }

bool imgui_BeginTableS(MonoString* id, int columns, int flags)
{
    return ImGui::BeginTable(mono_text(id).c_str(), columns, flags);
}

void imgui_TableSetupColumn(MonoString* label, int flags, float initWidth)
{
    ImGui::TableSetupColumn(mono_text(label).c_str(), static_cast<ImGuiTableColumnFlags>(flags), initWidth);
}

void imgui_TableSetupColumn_Default(MonoString* label)
{
    ImGui::TableSetupColumn(mono_text(label).c_str());
}

bool imgui_BeginChild(MonoString* id, float width, float height, int childFlags, int windowFlags)
{
    return ImGui::BeginChild(mono_text(id).c_str(), ImVec2(width, height),
                             static_cast<ImGuiChildFlags>(childFlags),
                             static_cast<ImGuiWindowFlags>(windowFlags));
}

void imgui_EndChild() { ImGui::EndChild(); }
void imgui_EndTable() { ImGui::EndTable(); }
void imgui_TableHeadersRow() { ImGui::TableHeadersRow(); }
void imgui_TableNextRow() { ImGui::TableNextRow(); }
bool imgui_TableNextColumn() { return ImGui::TableNextColumn(); }

void imgui_ProgressBar(float fraction, float height, float width, MonoString* overlay)
{
    ImGui::ProgressBar(fraction, ImVec2(width, height), mono_text(overlay).c_str());
}

// ---- popups and styles ---------------------------------------------------

bool imgui_BeginPopupContextWindow(MonoString* id, int flags)
{
    return ImGui::BeginPopupContextWindow(mono_text(id).c_str(), flags);
}

bool imgui_BeginPopupContextItem(MonoString* id, int flags)
{
    return ImGui::BeginPopupContextItem(mono_text(id).c_str(), flags);
}

void imgui_EndPopup() { ImGui::EndPopup(); }

void imgui_PushStyleColor(int which, float r, float g, float b, float a)
{
    ImGui::PushStyleColor(which, ImVec4(r, g, b, a));
}

void imgui_PopStyleColor(int count) { ImGui::PopStyleColor(count); }
void imgui_PushStyleVarFloat(int which, float value) { ImGui::PushStyleVar(which, value); }
void imgui_PushStyleVarVec2(int which, float x, float y) { ImGui::PushStyleVar(which, ImVec2(x, y)); }
void imgui_PopStyleVar(int count) { ImGui::PopStyleVar(count); }

// ---- item and mouse observations ----------------------------------------

float imgui_GetItemRectMinX() { return ImGui::GetItemRectMin().x; }
float imgui_GetItemRectMinY() { return ImGui::GetItemRectMin().y; }
float imgui_GetItemRectMaxX() { return ImGui::GetItemRectMax().x; }
float imgui_GetItemRectMaxY() { return ImGui::GetItemRectMax().y; }
bool imgui_IsItemHovered() { return ImGui::IsItemHovered(); }
bool imgui_IsMouseClicked(int button) { return ImGui::IsMouseClicked(button); }
float imgui_GetMousePosX() { return ImGui::GetMousePos().x; }
float imgui_GetMousePosY() { return ImGui::GetMousePos().y; }
bool imgui_IsMouseDragging(int button, float lockThreshold)
{
    return ImGui::IsMouseDragging(button, lockThreshold);
}
float imgui_GetMouseDeltaX() { return ImGui::GetIO().MouseDelta.x; }
float imgui_GetMouseDeltaY() { return ImGui::GetIO().MouseDelta.y; }

// ---- window draw list ----------------------------------------------------

void imgui_GetWindowDrawList_AddRectFilled(float x1, float y1, float x2, float y2,
                                           std::uint32_t color, float rounding, int corners)
{
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, y2), color, rounding, corners);
}

void imgui_GetWindowDrawList_AddCircleFilled(float centerX, float centerY, float radius,
                                             std::uint32_t color, int segments)
{
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(centerX, centerY), radius, color, segments);
}

void imgui_GetWindowDrawList_AddRect(float x1, float y1, float x2, float y2,
                                     std::uint32_t color, float rounding, int corners, float thickness)
{
    // ImGui 1.92.8 swapped AddRect's thickness/flags; `corners` is the flags.
    ImGui::GetWindowDrawList()->AddRect(ImVec2(x1, y1), ImVec2(x2, y2), color, rounding, thickness, corners);
}

// The coverage-mask gradient text state (below): between Begin and End the
// AddRectFilledMultiColor that carries the gradient is not drawn; its left
// and right colours tint the mask text's vertices instead.
struct CoverageMask {
    bool active = false;
    bool drawing = false;
    bool have_gradient = false;
    float x1 = 0, x2 = 0;
    int vtx_start = 0;
    std::uint32_t left = 0xffffffff, right = 0xffffffff;
};
CoverageMask coverage_mask;

void imgui_GetWindowDrawList_AddRectFilledMultiColor(float x1, float y1, float x2, float y2,
                                                     std::uint32_t topLeft, std::uint32_t topRight,
                                                     std::uint32_t bottomRight, std::uint32_t bottomLeft)
{
    if (coverage_mask.drawing) {
        coverage_mask.left = topLeft;
        coverage_mask.right = topRight;
        coverage_mask.have_gradient = true;
        return;
    }
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), topLeft, topRight,
                                                       bottomRight, bottomLeft);
}

void imgui_GetWindowDrawList_AddLine(float x1, float y1, float x2, float y2,
                                     std::uint32_t color, float thickness)
{
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), color, thickness);
}

void imgui_GetWindowDrawList_AddText(float x, float y, std::uint32_t color, MonoString* text)
{
    ImGui::GetWindowDrawList()->AddText(ImVec2(x, y), color, mono_text(text).c_str());
}

// Coverage-mask gradient text (MQ's mq/imgui/AlphaMask.h) is framebuffer
// alpha state, which Hailstone's Vulkan UI renderer has no equivalent of, so
// it is emulated on the draw list's vertices: Create pushes the region's clip
// and notes where the mask text's vertices start; the gradient rect between
// Begin and End is swallowed (AddRectFilledMultiColor above) and End tints
// each mask vertex by its x across the region, the font texture still
// supplying per-glyph coverage. Not registering these at all would leave
// Mono printing "cannot resolve internal call / libraries out of sync" on
// every load, though UiTheme would then fall back to solid text.
std::uint32_t coverage_lerp(std::uint32_t a, std::uint32_t b, float t)
{
    const auto channel = [t](std::uint32_t x, std::uint32_t y, int shift) {
        const float from = static_cast<float>((x >> shift) & 0xff);
        const float to = static_cast<float>((y >> shift) & 0xff);
        return static_cast<std::uint32_t>(from + (to - from) * t + 0.5f) << shift;
    };
    return channel(a, b, 0) | channel(a, b, 8) | channel(a, b, 16) | channel(a, b, 24);
}

void imgui_GetWindowDrawList_CreateCoverageMaskLayer(float x1, float y1, float x2, float y2)
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->PushClipRect(ImVec2(x1, y1), ImVec2(x2, y2), true);
    coverage_mask = CoverageMask{true, false, false, x1, x2, list->VtxBuffer.Size, 0xffffffff, 0xffffffff};
}

void imgui_GetWindowDrawList_BeginCoverageMaskedDraw()
{
    if (coverage_mask.active) coverage_mask.drawing = true;
}

void imgui_GetWindowDrawList_EndCoverageMaskedDraw()
{
    ImDrawList* list = ImGui::GetWindowDrawList();
    if (coverage_mask.active) {
        if (coverage_mask.have_gradient) {
            const float span = std::max(1.0f, coverage_mask.x2 - coverage_mask.x1);
            for (int i = coverage_mask.vtx_start; i < list->VtxBuffer.Size; ++i) {
                ImDrawVert& vert = list->VtxBuffer[i];
                const float t = std::clamp((vert.pos.x - coverage_mask.x1) / span, 0.0f, 1.0f);
                const std::uint32_t gradient = coverage_lerp(coverage_mask.left, coverage_mask.right, t);
                vert.col = (vert.col & 0xff000000u) | (gradient & 0x00ffffffu);
            }
        }
        list->PopClipRect();
    }
    coverage_mask = CoverageMask{};
}

// Spell icons live in the client's A_SpellIcons sheets. The plugin SDK has no
// texture accessor yet, so an icon cell cannot be drawn here; a no-op keeps
// the buff and song windows drawing their text, borders and timers.
void imgui_DrawSpellIconByIconIndex(int, float) {}

// File-backed textures need the client's resource manager, which the plugin
// SDK does not expose. Returning zero/null makes MiniHud use its text-button
// fallback instead of throwing on every frame.
void* imgui_LoadTextureFromFile(MonoString*) { return nullptr; }

void imgui_Image(void*, float, float) {}
bool imgui_ImageButton(MonoString*, void*, float, float) { return false; }

// ---- text filter and list clipper ---------------------------------------

void imgui_TextFilter_Create(MonoString* id, MonoString* defaultFilter)
{
    text_filters[mono_text(id)] = std::make_unique<ImGuiTextFilter>(mono_text(defaultFilter).c_str());
}

bool imgui_TextFilter_Draw(MonoString* id, MonoString* label, float width)
{
    const auto it = text_filters.find(mono_text(id));
    if (it == text_filters.end()) return false;
    // ImGui 1.93 dropped Draw's width parameter; keep the managed signature
    // and apply a positive width the way the old Draw did.
    if (width > 0.0f) ImGui::SetNextItemWidth(width);
    return it->second->Draw(mono_text(label).c_str());
}

bool imgui_TextFilter_PassFilter(MonoString* id, MonoString* text)
{
    const auto it = text_filters.find(mono_text(id));
    return it == text_filters.end() || it->second->PassFilter(mono_text(text).c_str());
}

void imgui_TextFilter_Clear(MonoString* id)
{
    const auto it = text_filters.find(mono_text(id));
    if (it != text_filters.end()) it->second->Clear();
}

bool imgui_TextFilter_IsActive(MonoString* id)
{
    const auto it = text_filters.find(mono_text(id));
    return it != text_filters.end() && it->second->IsActive();
}

void imgui_ListClipper_Begin(MonoString* id, int itemsCount, float itemsHeight)
{
    auto& clipper = list_clippers[mono_text(id)];
    clipper = std::make_unique<ImGuiListClipper>();
    clipper->Begin(itemsCount, itemsHeight);
}

bool imgui_ListClipper_Step(MonoString* id)
{
    const auto it = list_clippers.find(mono_text(id));
    return it != list_clippers.end() && it->second && it->second->Step();
}

void imgui_ListClipper_End(MonoString* id)
{
    const auto it = list_clippers.find(mono_text(id));
    if (it == list_clippers.end()) return;
    if (it->second) it->second->End();
    list_clippers.erase(it);
}

int imgui_ListClipper_GetDisplayStart(MonoString* id)
{
    const auto it = list_clippers.find(mono_text(id));
    return it != list_clippers.end() && it->second ? it->second->DisplayStart : 0;
}

int imgui_ListClipper_GetDisplayEnd(MonoString* id)
{
    const auto it = list_clippers.find(mono_text(id));
    return it != list_clippers.end() && it->second ? it->second->DisplayEnd : 0;
}

} // namespace

void install(MonoLibrary* library)
{
    runtime = library;

    add("MonoCore.E3ImGUI::imgui_Begin", &imgui_Begin);
    add("MonoCore.E3ImGUI::imgui_End", &imgui_End);
    add("MonoCore.E3ImGUI::imgui_Begin_OpenFlagSet", &imgui_Begin_OpenFlagSet);
    add("MonoCore.E3ImGUI::imgui_Begin_OpenFlagGet", &imgui_Begin_OpenFlagGet);
    add("MonoCore.E3ImGUI::imgui_PushID", &imgui_PushID);
    add("MonoCore.E3ImGUI::imgui_PopID", &imgui_PopID);
    add("MonoCore.E3ImGUI::imgui_PushTextWrapPos", &imgui_PushTextWrapPos);
    add("MonoCore.E3ImGUI::imgui_PopTextWrapPos", &imgui_PopTextWrapPos);
    add("MonoCore.E3ImGUI::imgui_TextFilter_Create", &imgui_TextFilter_Create);
    add("MonoCore.E3ImGUI::imgui_TextFilter_Draw", &imgui_TextFilter_Draw);
    add("MonoCore.E3ImGUI::imgui_TextFilter_PassFilter", &imgui_TextFilter_PassFilter);
    add("MonoCore.E3ImGUI::imgui_TextFilter_Clear", &imgui_TextFilter_Clear);
    add("MonoCore.E3ImGUI::imgui_TextFilter_IsActive", &imgui_TextFilter_IsActive);
    add("MonoCore.E3ImGUI::imgui_ListClipper_Begin", &imgui_ListClipper_Begin);
    add("MonoCore.E3ImGUI::imgui_ListClipper_Step", &imgui_ListClipper_Step);
    add("MonoCore.E3ImGUI::imgui_ListClipper_End", &imgui_ListClipper_End);
    add("MonoCore.E3ImGUI::imgui_ListClipper_GetDisplayStart", &imgui_ListClipper_GetDisplayStart);
    add("MonoCore.E3ImGUI::imgui_ListClipper_GetDisplayEnd", &imgui_ListClipper_GetDisplayEnd);
    add("MonoCore.E3ImGUI::imgui_Button", &imgui_Button);
    add("MonoCore.E3ImGUI::imgui_ButtonEx", &imgui_ButtonEx);
    add("MonoCore.E3ImGUI::imgui_RightAlignButton", &imgui_RightAlignButton);
    add("MonoCore.E3ImGUI::imgui_SetNextItemWidth", &imgui_SetNextItemWidth);
    add("MonoCore.E3ImGUI::imgui_PushItemWidth", &imgui_PushItemWidth);
    add("MonoCore.E3ImGUI::imgui_PopItemWidth", &imgui_PopItemWidth);
    add("MonoCore.E3ImGUI::imgui_TextWrapped", &imgui_TextWrapped);
    add("MonoCore.E3ImGUI::imgui_NewLine", &imgui_NewLine);
    add("MonoCore.E3ImGUI::imgui_BeginGroup", &imgui_BeginGroup);
    add("MonoCore.E3ImGUI::imgui_EndGroup", &imgui_EndGroup);
    add("MonoCore.E3ImGUI::imgui_BeginDisabled", &imgui_BeginDisabled);
    add("MonoCore.E3ImGUI::imgui_EndDisabled", &imgui_EndDisabled);
    add("MonoCore.E3ImGUI::imgui_CollapsingHeader", &imgui_CollapsingHeader);
    add("MonoCore.E3ImGUI::imgui_TreeNode", &imgui_TreeNode);
    add("MonoCore.E3ImGUI::imgui_TreePop", &imgui_TreePop);
    add("MonoCore.E3ImGUI::imgui_Selectable", &imgui_Selectable);
    add("MonoCore.E3ImGUI::imgui_Checkbox", &imgui_Checkbox);
    add("MonoCore.E3ImGUI::imgui_Checkbox_Get", &imgui_Checkbox_Get);
    add("MonoCore.E3ImGUI::imgui_Checkbox_Clear", &imgui_Checkbox_Clear);
    add("MonoCore.E3ImGUI::imgui_InputText", &imgui_InputText);
    add("MonoCore.E3ImGUI::imgui_InputText_Get", &imgui_InputText_Get);
    add("MonoCore.E3ImGUI::imgui_InputText_Clear", &imgui_InputText_Clear);
    add("MonoCore.E3ImGUI::imgui_InputTextMultiline", &imgui_InputTextMultiline);
    add("MonoCore.E3ImGUI::imgui_InputInt", &imgui_InputInt);
    add("MonoCore.E3ImGUI::imgui_InputInt_Get", &imgui_InputInt_Get);
    add("MonoCore.E3ImGUI::imgui_InputInt_Clear", &imgui_InputInt_Clear);
    add("MonoCore.E3ImGUI::imgui_SliderInt", &imgui_SliderInt);
    add("MonoCore.E3ImGUI::imgui_BeginTabBar", &imgui_BeginTabBar);
    add("MonoCore.E3ImGUI::imgui_EndTabBar", &imgui_EndTabBar);
    add("MonoCore.E3ImGUI::imgui_BeginTabItem", &imgui_BeginTabItem);
    add("MonoCore.E3ImGUI::imgui_EndTabItem", &imgui_EndTabItem);
    add("MonoCore.E3ImGUI::imgui_BeginCombo", &imgui_BeginCombo);
    add("MonoCore.E3ImGUI::imgui_EndCombo", &imgui_EndCombo);
    add("MonoCore.E3ImGUI::imgui_BeginTableS", &imgui_BeginTableS);
    add("MonoCore.E3ImGUI::imgui_TableSetupColumn", &imgui_TableSetupColumn);
    add("MonoCore.E3ImGUI::imgui_BeginChild", &imgui_BeginChild);
    add("MonoCore.E3ImGUI::imgui_EndChild", &imgui_EndChild);
    add("MonoCore.E3ImGUI::imgui_EndTable", &imgui_EndTable);
    add("MonoCore.E3ImGUI::imgui_TableSetupColumn_Default", &imgui_TableSetupColumn_Default);
    add("MonoCore.E3ImGUI::imgui_TableHeadersRow", &imgui_TableHeadersRow);
    add("MonoCore.E3ImGUI::imgui_TableNextRow", &imgui_TableNextRow);
    add("MonoCore.E3ImGUI::imgui_TableNextColumn", &imgui_TableNextColumn);
    add("MonoCore.E3ImGUI::imgui_ProgressBar", &imgui_ProgressBar);
    add("MonoCore.E3ImGUI::imgui_GetContentRegionAvailX", &imgui_GetContentRegionAvailX);
    add("MonoCore.E3ImGUI::imgui_GetContentRegionAvailY", &imgui_GetContentRegionAvailY);
    add("MonoCore.E3ImGUI::imgui_SetNextWindowSizeWithCond", &imgui_SetNextWindowSizeWithCond);
    add("MonoCore.E3ImGUI::imgui_SetNextWindowBgAlpha", &imgui_SetNextWindowBgAlpha);
    add("MonoCore.E3ImGUI::imgui_SetNextWindowPos", &imgui_SetNextWindowPos);
    add("MonoCore.E3ImGUI::imgui_GetWindowContentRegionMinX", &imgui_GetWindowContentRegionMinX);
    add("MonoCore.E3ImGUI::imgui_GetWindowContentRegionMaxX", &imgui_GetWindowContentRegionMaxX);
    add("MonoCore.E3ImGUI::imgui_GetCursorPosX", &imgui_GetCursorPosX);
    add("MonoCore.E3ImGUI::imgui_SetCursorPosX", &imgui_SetCursorPosX);
    add("MonoCore.E3ImGUI::imgui_GetCursorPosY", &imgui_GetCursorPosY);
    add("MonoCore.E3ImGUI::imgui_SetCursorPosY", &imgui_SetCursorPosY);
    add("MonoCore.E3ImGUI::imgui_GetCursorScreenPosX", &imgui_GetCursorScreenPosX);
    add("MonoCore.E3ImGUI::imgui_GetCursorScreenPosY", &imgui_GetCursorScreenPosY);
    add("MonoCore.E3ImGUI::imgui_GetTextLineHeight", &imgui_GetTextLineHeight);
    add("MonoCore.E3ImGUI::imgui_CalcTextSizeX", &imgui_CalcTextSizeX);
    add("MonoCore.E3ImGUI::imgui_InvisibleButton", &imgui_InvisibleButton);
    add("MonoCore.E3ImGUI::imgui_SmallButton", &imgui_SmallButton);
    add("MonoCore.E3ImGUI::imgui_IsItemHovered", &imgui_IsItemHovered);
    add("MonoCore.E3ImGUI::imgui_IsMouseClicked", &imgui_IsMouseClicked);
    add("MonoCore.E3ImGUI::imgui_GetMousePosX", &imgui_GetMousePosX);
    add("MonoCore.E3ImGUI::imgui_GetMousePosY", &imgui_GetMousePosY);
    add("MonoCore.E3ImGUI::imgui_IsMouseDragging", &imgui_IsMouseDragging);
    add("MonoCore.E3ImGUI::imgui_GetMouseDeltaX", &imgui_GetMouseDeltaX);
    add("MonoCore.E3ImGUI::imgui_GetMouseDeltaY", &imgui_GetMouseDeltaY);
    add("MonoCore.E3ImGUI::imgui_Text", &imgui_Text);
    add("MonoCore.E3ImGUI::imgui_TextUnformatted", &imgui_TextUnformatted);
    add("MonoCore.E3ImGUI::imgui_TextDisabled", &imgui_TextDisabled);
    add("MonoCore.E3ImGUI::imgui_TextColored", &imgui_TextColored);
    add("MonoCore.E3ImGUI::imgui_Separator", &imgui_Separator);
    add("MonoCore.E3ImGUI::imgui_SeparatorText", &imgui_SeparatorText);
    add("MonoCore.E3ImGUI::imgui_SameLine", &imgui_SameLine);
    add("MonoCore.E3ImGUI::imgui_Spacing", &imgui_Spacing);
    add("MonoCore.E3ImGUI::imgui_Dummy", &imgui_Dummy);
    add("MonoCore.E3ImGUI::imgui_SetTooltip", &imgui_SetTooltip);
    add("MonoCore.E3ImGUI::imgui_BeginTooltip", &imgui_BeginTooltip);
    add("MonoCore.E3ImGUI::imgui_EndTooltip", &imgui_EndTooltip);
    add("MonoCore.E3ImGUI::imgui_BeginPopupContextWindow", &imgui_BeginPopupContextWindow);
    add("MonoCore.E3ImGUI::imgui_BeginPopupContextItem", &imgui_BeginPopupContextItem);
    add("MonoCore.E3ImGUI::imgui_EndPopup", &imgui_EndPopup);
    add("MonoCore.E3ImGUI::imgui_MenuItem", &imgui_MenuItem);
    add("MonoCore.E3ImGUI::imgui_PushStyleColor", &imgui_PushStyleColor);
    add("MonoCore.E3ImGUI::imgui_PopStyleColor", &imgui_PopStyleColor);
    add("MonoCore.E3ImGUI::imgui_GetItemRectMinX", &imgui_GetItemRectMinX);
    add("MonoCore.E3ImGUI::imgui_GetItemRectMinY", &imgui_GetItemRectMinY);
    add("MonoCore.E3ImGUI::imgui_GetItemRectMaxX", &imgui_GetItemRectMaxX);
    add("MonoCore.E3ImGUI::imgui_GetItemRectMaxY", &imgui_GetItemRectMaxY);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddRectFilled", &imgui_GetWindowDrawList_AddRectFilled);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddCircleFilled", &imgui_GetWindowDrawList_AddCircleFilled);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddRect", &imgui_GetWindowDrawList_AddRect);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddRectFilledMultiColor",
        &imgui_GetWindowDrawList_AddRectFilledMultiColor);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddLine", &imgui_GetWindowDrawList_AddLine);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_AddText", &imgui_GetWindowDrawList_AddText);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_CreateCoverageMaskLayer",
        &imgui_GetWindowDrawList_CreateCoverageMaskLayer);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_BeginCoverageMaskedDraw",
        &imgui_GetWindowDrawList_BeginCoverageMaskedDraw);
    add("MonoCore.E3ImGUI::imgui_GetWindowDrawList_EndCoverageMaskedDraw",
        &imgui_GetWindowDrawList_EndCoverageMaskedDraw);
    add("MonoCore.E3ImGUI::imgui_DrawSpellIconByIconIndex", &imgui_DrawSpellIconByIconIndex);
    add("MonoCore.E3ImGUI::imgui_Image", &imgui_Image);
    add("MonoCore.E3ImGUI::imgui_ImageButton", &imgui_ImageButton);
    add("MonoCore.E3ImGUI::imgui_LoadTextureFromFile", &imgui_LoadTextureFromFile);
    add("MonoCore.E3ImGUI::imgui_PushStyleVarFloat", &imgui_PushStyleVarFloat);
    add("MonoCore.E3ImGUI::imgui_PushStyleVarVec2", &imgui_PushStyleVarVec2);
    add("MonoCore.E3ImGUI::imgui_PopStyleVar", &imgui_PopStyleVar);
}

void set_context(void* context)
{
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
}

void reset_state()
{
    open_windows.clear();
    checkbox_values.clear();
    input_int_values.clear();
    input_text_values.clear();
    text_filters.clear();
    list_clippers.clear();
}

} // namespace hs_mono::imgui_bridge
