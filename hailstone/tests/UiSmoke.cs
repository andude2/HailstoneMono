using System;
using System.Runtime.CompilerServices;

namespace MonoCore
{
    // A tiny managed caller for the HSMono ImGui bridge. It draws a
    // window that exercises the stateful wrappers (open flags, edit buffers,
    // checkbox, slider, text filter, list clipper, tables, draw list) so the
    // native harness can prove the internal calls are registered and survive
    // one real frame.
    public static class E3ImGUI
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_Begin(string name, int flags);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_End();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_Begin_OpenFlagSet(string name, bool open);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_Begin_OpenFlagGet(string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_Text(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextDisabled(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextUnformatted(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextWrapped(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextColored(float r, float g, float b, float a, string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_Button(string label);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_ButtonEx(string label, float width, float height);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_Checkbox(string name, bool value);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_Checkbox_Get(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_InputText(string id, string initial);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string imgui_InputText_Get(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_InputInt(string id, int initial, int steps, int fastSteps);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int imgui_InputInt_Get(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_SliderInt(string id, ref int value, int minValue, int maxValue);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_PushStyleColor(int which, float r, float g, float b, float a);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_PopStyleColor(int count);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern float imgui_GetContentRegionAvailX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern float imgui_GetWindowContentRegionMinX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern float imgui_GetWindowContentRegionMaxX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern float imgui_CalcTextSizeX(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_ProgressBar(float fraction, float height, float width, string overlay);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_BeginTableS(string id, int columns, int flags);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TableSetupColumn(string label, int flags, float initWidth);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TableHeadersRow();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TableNextRow();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_TableNextColumn();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_EndTable();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_BeginChild(string id, float width, float height, int childFlags, int windowFlags);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_EndChild();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_GetWindowDrawList_AddRectFilled(float x1, float y1, float x2, float y2, uint color, float rounding, int roundingCornersFlags);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_GetWindowDrawList_AddText(float x, float y, uint color, string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextFilter_Create(string id, string defaultFilter);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_TextFilter_Draw(string id, string label, float width);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_TextFilter_PassFilter(string id, string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_TextFilter_Clear(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_ListClipper_Begin(string id, int itemsCount, float itemsHeight);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern bool imgui_ListClipper_Step(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void imgui_ListClipper_End(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int imgui_ListClipper_GetDisplayStart(string id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int imgui_ListClipper_GetDisplayEnd(string id);

        public static int Draw()
        {
            imgui_Begin_OpenFlagSet("UI smoke", true);
            if (!imgui_Begin_OpenFlagGet("UI smoke")) return 0;
            if (!imgui_Begin("UI smoke", 0)) { imgui_End(); return 0; }

            imgui_Text("hello");
            imgui_TextDisabled("disabled");
            imgui_TextUnformatted("unformatted");
            imgui_TextWrapped("wrapped");
            imgui_TextColored(1f, 0.5f, 0.25f, 1f, "coloured");
            imgui_Text("width=" + imgui_GetContentRegionAvailX().ToString("0.0")
                + " min=" + imgui_GetWindowContentRegionMinX().ToString("0.0")
                + " max=" + imgui_GetWindowContentRegionMaxX().ToString("0.0")
                + " text=" + imgui_CalcTextSizeX("hello").ToString("0.0"));
            imgui_ProgressBar(0.5f, 8f, 100f, "50%");

            if (imgui_Button("OK")) { }
            if (imgui_ButtonEx("Wide", 80f, 24f)) { }
            imgui_Checkbox("##check", true);
            imgui_InputText("##text", "value");
            if (imgui_InputText_Get("##text") != "value") return 0;
            imgui_InputInt("##int", 3, 1, 10);
            if (imgui_InputInt_Get("##int") != 3) return 0;
            int slider = 5;
            imgui_SliderInt("##slider", ref slider, 0, 10);

            if (imgui_BeginTableS("##table", 2, 0))
            {
                imgui_TableSetupColumn("One", 0, 0f);
                imgui_TableSetupColumn("Two", 0, 0f);
                imgui_TableHeadersRow();
                imgui_TableNextRow();
                imgui_TableNextColumn();
                imgui_Text("a");
                imgui_TableNextColumn();
                imgui_Text("b");
                imgui_EndTable();
            }

            if (imgui_BeginChild("##child", 0f, 40f, 0, 0)) { imgui_Text("child"); }
            imgui_EndChild();

            imgui_TextFilter_Create("##filter", "");
            imgui_TextFilter_Draw("##filter", "Filter", 120f);
            if (!imgui_TextFilter_PassFilter("##filter", "hello")) return 0;
            imgui_TextFilter_Clear("##filter");

            imgui_ListClipper_Begin("##clip", 3, -1f);
            while (imgui_ListClipper_Step("##clip"))
            {
                int start = imgui_ListClipper_GetDisplayStart("##clip");
                int end = imgui_ListClipper_GetDisplayEnd("##clip");
                if (start < 0 || end < start) return 0;
            }
            imgui_ListClipper_End("##clip");

            imgui_PushStyleColor(0, 1f, 0.8f, 0.6f, 1f);
            imgui_Text("styled");
            imgui_PopStyleColor(1);
            imgui_GetWindowDrawList_AddRectFilled(0f, 0f, 10f, 10f, 0xFFFFFFFFu, 0f, 0);
            imgui_GetWindowDrawList_AddText(0f, 0f, 0xFFFFFFFFu, "draw");

            imgui_End();
            return 1;
        }
    }
}
