#include "pch.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#undef min
#undef max
#include "CurvePresetModel.h"
#include "AEConfig.h"
#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "NativeGraphBridge.h"
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shcore.lib")

void AEGP_SuiteHandler::MissingSuiteError() const { throw std::runtime_error("AE Suite unavailable"); }
static SPBasicSuite* g_sp = nullptr;
static AEGP_PluginID g_plugin_id = 0;
static AEGP_Command g_command = 0;
static HWND g_ae_main = nullptr;
static HMODULE g_module = nullptr;
static std::wstring g_plugin_dir;
static bool s_panel_active = false;
static POINT g_popupOrigin{};

struct PropertyInfo {
    AEGP_StreamRefH streamH = nullptr;
    int32_t id = 0;
    std::wstring name;
    AEGP_StreamType type = AEGP_StreamType_OneD;
    int numDims = 1, temporalDims = 1, numKFs = 0;
};
struct KfInfo {
    double time = 0;
    AEGP_StreamValue2 value{};
    double curValue[3]{};
    AEGP_KeyframeEase easeIn[3]{}, easeOut[3]{};
    AEGP_KeyframeInterpolationType inInterp = AEGP_KeyInterp_LINEAR, outInterp = AEGP_KeyInterp_LINEAR;
};
struct PanelState {
    HWND hwnd = nullptr;
    int curProp = -1, selKf = -1, presetSegment = 0, presetDim = 0, libraryIndex = 0;
    bool toast = false;
    std::wstring presetStatus, toastMsg;
};
static PanelState g_panel;
// These owned AE handles exist only during a command/idle callback.
static std::vector<PropertyInfo> g_props;
static std::vector<KfInfo> g_kfs;
static void CurvesDebugLog(const wchar_t* message) {
    OutputDebugStringW(message);
    if (g_plugin_dir.empty()) return;
    // The installed AEX lives in Program Files. Runtime diagnostics must go
    // to a user-writable directory, otherwise failed toggles leave no evidence.
    wchar_t appData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData))) return;
    std::wstring directory = std::wstring(appData) + L"\\DalimaoCurves";
    CreateDirectoryW(directory.c_str(), nullptr);
    FILE* file = nullptr;
    _wfopen_s(&file, (directory + L"\\native-graph.log").c_str(), L"a, ccs=UTF-8");
    if (file) { fwprintf(file, L"%s\n", message); fclose(file); }
}
static double TimeToSec(const A_Time& t) { return t.scale ? double(t.value) / t.scale : 0; }
static int ClampI(int value, int lo, int hi) { return std::clamp(value, lo, hi); }
static double GetDimValue(const AEGP_StreamVal2& value, AEGP_StreamType type, int dim) {
    if (type == AEGP_StreamType_OneD) return value.one_d;
    if (type == AEGP_StreamType_TwoD || type == AEGP_StreamType_TwoD_SPATIAL)
        return dim == 0 ? value.two_d.x : value.two_d.y;
    return dim == 0 ? value.three_d.x : dim == 1 ? value.three_d.y : value.three_d.z;
}
#include "CurveStreams.inl"
static void ReloadKeyframes() {
    AEGP_SuiteHandler suites(g_sp);
    DisposeKeyframes(suites); g_kfs.clear();
    if (g_panel.curProp < 0 || g_panel.curProp >= int(g_props.size())) return;
    auto& pi = g_props[g_panel.curProp];
    A_long count = 0;
    bool failed = suites.KeyframeSuite5()->AEGP_GetStreamNumKFs(pi.streamH, &count) != 0;
    pi.numKFs = count;
    for (int i = 0; !failed && i < count; ++i) {
        KfInfo key; A_Time time{};
        auto ks = suites.KeyframeSuite5();
        failed = ks->AEGP_GetKeyframeTime(pi.streamH, i, AEGP_LTimeMode_CompTime, &time) != 0;
        if (failed) break;
        failed = ks->AEGP_GetNewKeyframeValue(g_plugin_id, pi.streamH, i, &key.value) != 0;
        if (failed) break;
        key.time = TimeToSec(time);
        for (int d = 0; d < pi.numDims; ++d) key.curValue[d] = GetDimValue(key.value.val, pi.type, d);
        for (int d = 0; d < pi.temporalDims; ++d)
            failed |= ks->AEGP_GetKeyframeTemporalEase(pi.streamH, i, d, &key.easeIn[d], &key.easeOut[d]) != 0;
        failed |= ks->AEGP_GetKeyframeInterpolation(pi.streamH, i, &key.inInterp, &key.outInterp) != 0;
        g_kfs.push_back(key);
    }
    if (failed || g_kfs.empty()) {
        DisposeKeyframes(suites); g_kfs.clear(); g_panel.curProp = -1;
    }
}
#include "CurvePresetPanel.inl"

struct Target {
    A_long comp = 0;
    AEGP_LayerIDVal layer = 0;
    // SDK stream IDs are unique within the AE session, including project switches.
    int32_t stream = 0;
    bool operator==(const Target&) const = default;
};
struct DisplayProperty { int32_t id; std::wstring name; };
static Target g_target;
static std::vector<DisplayProperty> g_displayProps;
static std::vector<KfInfo> g_displayKeys; // stream value handles are zeroed before copying
static int g_dimensions = 1;
enum ControlId { PROP = 101, SEGMENT, DIMENSION, REFRESH, OUT_SLIDER, IN_SLIDER,
    OUT_LABEL, IN_LABEL, LIBRARY, SAVE, APPLY, DELETE_PRESET, RELOAD, STATUS, RETURN_LAYERS, PRESET = 200 };
enum class ActionKind { None, Close, Refresh, Sync, Property, Segment, Dimension, Preset, Handles, Save, Apply, Delete, Reload };
struct Action {
    ActionKind kind = ActionKind::None;
    Target target;
    int index = 0, segment = 0, dim = 0;
    double out = 0, in = 0;
    std::array<KfInfo, 2> keys{};
    bool hasPair = false;
};
static Action g_pending;
static bool g_closeAfterPending = false, g_closing = false, g_transition = false;
static ULONGLONG g_lastSync = 0;
static HHOOK g_keyboardHook = nullptr;
static UINT g_shortcut = 0;
static bool g_ctrl = false, g_alt = false, g_shift = false;
static bool g_activationHeld = false;
static HWND Control(int id) { return GetDlgItem(g_panel.hwnd, id); }
static void SetControlsBusy(bool busy) {
    for (int id : { PROP, SEGMENT, DIMENSION, REFRESH, OUT_SLIDER, IN_SLIDER, LIBRARY, SAVE, APPLY, DELETE_PRESET, RELOAD })
        EnableWindow(Control(id), !busy);
    for (int i = 0; i < 6; ++i) EnableWindow(Control(PRESET + i), !busy);
}
static void Queue(ActionKind kind, int index = 0) {
    // A pending edit and its target are one immutable request. Never replace it
    // with a different target while AE is busy rendering or in a dialog.
    if (g_pending.kind != ActionKind::None) {
        if (kind == ActionKind::Close) g_closeAfterPending = true;
        return;
    }
    if (g_closing || (g_transition && kind != ActionKind::Close)) return;
    Action action; action.kind = kind; action.target = g_target; action.index = index;
    action.segment = g_panel.presetSegment; action.dim = g_panel.presetDim;
    action.out = SendMessageW(Control(OUT_SLIDER), TBM_GETPOS, 0, 0) / 10.0;
    action.in = SendMessageW(Control(IN_SLIDER), TBM_GETPOS, 0, 0) / 10.0;
    if (action.segment >= 0 && action.segment + 1 < int(g_displayKeys.size())) {
        action.keys = { g_displayKeys[action.segment], g_displayKeys[action.segment + 1] };
        action.hasPair = true;
    }
    g_pending = action;
    SetControlsBusy(true);
}
static void ReleaseHostData() {
    AEGP_SuiteHandler suites(g_sp);
    DisposeKeyframes(suites); g_kfs.clear(); DisposeProperties(suites);
}
static bool AcquireActive(Target& target, AEGP_CompH& comp) {
    AEGP_SuiteHandler suites(g_sp);
    AEGP_LayerH layer = nullptr; AEGP_ItemH item = nullptr;
    if (suites.LayerSuite9()->AEGP_GetActiveLayer(&layer) || !layer ||
        suites.LayerSuite9()->AEGP_GetLayerParentComp(layer, &comp) || !comp ||
        suites.CompSuite11()->AEGP_GetItemFromComp(comp, &item) ||
        suites.ItemSuite9()->AEGP_GetItemID(item, &target.comp) ||
        suites.LayerSuite9()->AEGP_GetLayerID(layer, &target.layer)) {
        g_panel.presetStatus = L"请在 AE 时间轴中选中一个带关键帧的图层，然后点击“读取 AE 选择”";
        return false;
    }
    CollectProperties(suites, layer, g_props);
    return true;
}
static void ReadSelection(AEGP_CompH comp, int& property, int& segment) {
    AEGP_SuiteHandler suites(g_sp); AEGP_Collection2H collection = nullptr;
    if (suites.CompSuite11()->AEGP_GetNewCollectionFromCompSelection(g_plugin_id, comp, &collection)) return;
    A_u_long count = 0; int candidate = -1; bool multiple = false;
    std::vector<int> selectedKeys;
    auto cs = suites.CollectionSuite2();
    if (!cs->AEGP_GetCollectionNumItems(collection, &count)) for (A_u_long i = 0; i < count; ++i) {
        AEGP_CollectionItemV2 entry{}; int32_t id = 0;
        if (cs->AEGP_GetCollectionItemByIndex(collection, i, &entry) || !entry.stream_refH ||
            suites.StreamSuite6()->AEGP_GetUniqueStreamID(entry.stream_refH, &id)) continue;
        for (int j = 0; j < int(g_props.size()); ++j) if (g_props[j].id == id) {
            if (candidate >= 0 && candidate != j) multiple = true;
            candidate = j;
            if (entry.type == AEGP_CollectionItemType_KEYFRAME) selectedKeys.push_back(entry.u.keyframe.index);
        }
    }
    cs->AEGP_DisposeCollection(collection);
    if (!multiple && candidate >= 0) {
        property = candidate;
        std::sort(selectedKeys.begin(), selectedKeys.end());
        selectedKeys.erase(std::unique(selectedKeys.begin(), selectedKeys.end()), selectedKeys.end());
        if (selectedKeys.size() == 2 && selectedKeys[1] == selectedKeys[0] + 1) segment = selectedKeys[0];
    }
}
static bool ShowNativeProperty(AEGP_CompH comp) {
    if (g_panel.curProp < 0) return false;
    AEGP_SuiteHandler suites(g_sp); AEGP_Collection2H selection = nullptr;
    auto cs = suites.CollectionSuite2();
    if (cs->AEGP_NewCollection(g_plugin_id, &selection)) return false;
    AEGP_CollectionItemV2 entry{};
    entry.type = AEGP_CollectionItemType_STREAMREF;
    A_Err error = suites.StreamSuite6()->AEGP_DuplicateStreamRef(g_plugin_id,
        g_props[g_panel.curProp].streamH, &entry.stream_refH);
    if (!error) {
        error = cs->AEGP_CollectionPushBack(selection, &entry);
        if (error) suites.StreamSuite6()->AEGP_DisposeStream(entry.stream_refH);
        else error = suites.CompSuite11()->AEGP_SetSelection(comp, selection);
    }
    cs->AEGP_DisposeCollection(selection);
    return !error;
}
static void AddCombo(HWND control, const std::wstring& value) { SendMessageW(control, CB_ADDSTRING, 0, LPARAM(value.c_str())); }
static void SliderLabels() {
    wchar_t label[120];
    swprintf_s(label, L"起点出手柄长度  %.1f%%", SendMessageW(Control(OUT_SLIDER), TBM_GETPOS, 0, 0) / 10.0);
    SetWindowTextW(Control(OUT_LABEL), label);
    swprintf_s(label, L"终点入手柄长度  %.1f%%", SendMessageW(Control(IN_SLIDER), TBM_GETPOS, 0, 0) / 10.0);
    SetWindowTextW(Control(IN_LABEL), label);
}
static void UpdateControls() {
    if (!g_panel.hwnd) return;
    SetControlsBusy(false);
    SendMessageW(Control(PROP), CB_RESETCONTENT, 0, 0);
    for (const auto& p : g_displayProps) AddCombo(Control(PROP), p.name);
    int selected = -1;
    for (int i = 0; i < int(g_displayProps.size()); ++i) if (g_displayProps[i].id == g_target.stream) selected = i;
    SendMessageW(Control(PROP), CB_SETCURSEL, selected, 0);
    SendMessageW(Control(SEGMENT), CB_RESETCONTENT, 0, 0);
    for (int i = 0; i + 1 < int(g_displayKeys.size()); ++i) {
        wchar_t text[100]; swprintf_s(text, L"第 %d → %d 帧   %.3fs → %.3fs", i + 1, i + 2,
            g_displayKeys[i].time, g_displayKeys[i + 1].time);
        AddCombo(Control(SEGMENT), text);
    }
    SendMessageW(Control(SEGMENT), CB_SETCURSEL, g_panel.presetSegment, 0);
    SendMessageW(Control(DIMENSION), CB_RESETCONTENT, 0, 0);
    const wchar_t* dims[] = { L"X / 数值", L"Y", L"Z" };
    for (int i = 0; i < g_dimensions; ++i) AddCombo(Control(DIMENSION), g_dimensions == 1 ? L"数值 / 空间速度" : dims[i]);
    SendMessageW(Control(DIMENSION), CB_SETCURSEL, g_panel.presetDim, 0);
    bool pair = selected >= 0 && g_panel.presetSegment >= 0 && g_panel.presetSegment + 1 < int(g_displayKeys.size());
    if (pair) {
        const auto& a = g_displayKeys[g_panel.presetSegment]; const auto& b = g_displayKeys[g_panel.presetSegment + 1];
        double out = a.outInterp == AEGP_KeyInterp_BEZIER ? a.easeOut[g_panel.presetDim].influenceF * 1000 : 333;
        double in = b.inInterp == AEGP_KeyInterp_BEZIER ? b.easeIn[g_panel.presetDim].influenceF * 1000 : 333;
        SendMessageW(Control(OUT_SLIDER), TBM_SETPOS, TRUE, ClampI(int(out + .5), 1, 1000));
        SendMessageW(Control(IN_SLIDER), TBM_SETPOS, TRUE, ClampI(int(in + .5), 1, 1000));
    }
    SliderLabels();
    for (int id : { OUT_SLIDER, IN_SLIDER, SAVE, APPLY }) EnableWindow(Control(id), pair);
    for (int i = 0; i < 6; ++i) EnableWindow(Control(PRESET + i), pair);
    SendMessageW(Control(LIBRARY), CB_RESETCONTENT, 0, 0);
    for (const auto& p : g_userPresets) AddCombo(Control(LIBRARY), std::wstring(p.name.begin(), p.name.end()));
    SendMessageW(Control(LIBRARY), CB_SETCURSEL, g_panel.libraryIndex, 0);
    SetWindowTextW(Control(STATUS), (g_panel.toast ? g_panel.toastMsg : g_panel.presetStatus).c_str());
    if (g_transition || g_closing) SetControlsBusy(true);
}
static void SnapshotDisplay() {
    g_displayProps.clear();
    for (const auto& pi : g_props) g_displayProps.push_back({ pi.id, pi.name });
    g_displayKeys = g_kfs;
    for (auto& key : g_displayKeys) key.value = {};
    if (g_panel.curProp >= 0) {
        auto& pi = g_props[g_panel.curProp]; g_target.stream = pi.id;
        g_dimensions = pi.temporalDims;
    } else g_target.stream = 0;
    g_panel.presetSegment = ClampI(g_panel.presetSegment, 0, std::max(0, int(g_kfs.size()) - 2));
    g_panel.presetDim = ClampI(g_panel.presetDim, 0, std::max(0, g_dimensions - 1));
}
static bool PairUnchanged(const Action& action) {
    if (!action.hasPair || action.segment < 0 || action.segment + 1 >= int(g_kfs.size())) return false;
    for (int i = 0; i < 2; ++i) {
        const auto& a = action.keys[i]; const auto& b = g_kfs[action.segment + i];
        if (fabs(a.time - b.time) > 1e-7 || a.inInterp != b.inInterp || a.outInterp != b.outInterp) return false;
        for (int d = 0; d < 3; ++d) {
            if (a.curValue[d] != b.curValue[d] || a.easeIn[d].speedF != b.easeIn[d].speedF ||
                a.easeOut[d].speedF != b.easeOut[d].speedF || a.easeIn[d].influenceF != b.easeIn[d].influenceF ||
                a.easeOut[d].influenceF != b.easeOut[d].influenceF) return false;
        }
    }
    return true;
}
static void ProcessAction(const Action& action) {
    g_panel.toast = false;
    if (action.kind == ActionKind::Reload) { LoadPresetLibrary(); UpdateControls(); return; }
    if (action.kind == ActionKind::Delete) {
        auto next = g_userPresets;
        if (action.index >= 0 && action.index < int(next.size())) {
            next.erase(next.begin() + action.index);
            if (SavePresetLibrary(next)) { g_panel.libraryIndex = 0; g_panel.presetStatus = L"已删除模板"; }
        }
        UpdateControls(); return;
    }
    Target current; AEGP_CompH comp = nullptr;
    if (!AcquireActive(current, comp)) {
        g_target = {}; g_displayProps.clear(); g_displayKeys.clear(); UpdateControls(); return;
    }
    const bool refresh = action.kind == ActionKind::Refresh;
    if (!refresh && (current.comp != action.target.comp || current.layer != action.target.layer)) {
        g_panel.presetStatus = L"AE 中的图层或合成已改变，未修改。请点击“读取 AE 选择”";
        UpdateControls(); return;
    }
    int property = -1;
    int32_t id = action.target.stream;
    if (action.kind == ActionKind::Property && action.index >= 0 && action.index < int(g_displayProps.size())) id = g_displayProps[action.index].id;
    for (int i = 0; i < int(g_props.size()); ++i) if (g_props[i].id == id) property = i;
    g_panel.presetSegment = action.segment; g_panel.presetDim = action.dim;
    if (refresh) {
        property = -1; g_panel.presetSegment = 0; g_panel.presetDim = 0;
        ReadSelection(comp, property, g_panel.presetSegment);
        if (property < 0 && g_props.size() == 1) property = 0;
        g_panel.presetStatus = property < 0 ? L"请选择上方要编辑的属性；多个属性不会自动批量修改" : L"已读取 AE 选择。可使用预设按钮或拖动手柄长度滑块";
    }
    g_target = current; g_panel.curProp = property;
    ReloadKeyframes();
    if (g_panel.curProp >= 0) {
        if (action.kind == ActionKind::Property) {
            g_panel.presetSegment = 0; g_panel.presetDim = 0;
            g_panel.presetStatus = ShowNativeProperty(comp) ? L"已选中 AE 中对应属性，可直接查看原生曲线" : L"已读取属性；AE 未接受界面选择，请在时间轴中点选该属性";
        }
        else if (action.kind == ActionKind::Segment) g_panel.presetSegment = action.index;
        else if (action.kind == ActionKind::Dimension) g_panel.presetDim = action.index;
        else if (!refresh && action.kind != ActionKind::Sync) {
            if (!PairUnchanged(action)) g_panel.presetStatus = L"关键帧已在 AE 中改变，本次未修改；已刷新，请再次操作";
            else if (action.kind == ActionKind::Preset && action.index >= 0 && action.index < 6) ApplySegmentPreset(curve_presets::Builtins()[action.index]);
            else if (action.kind == ActionKind::Handles) ApplyHandleInfluences(action.out, action.in);
            else if (action.kind == ActionKind::Save) CaptureSegmentPreset();
            else if (action.kind == ActionKind::Apply && action.index >= 0 && action.index < int(g_userPresets.size())) ApplySegmentPreset(g_userPresets[action.index]);
        }
    } else if (!refresh) g_panel.presetStatus = L"目标属性或关键帧已不存在，未修改；请重新读取 AE 选择";
    SnapshotDisplay(); UpdateControls();
}

#include "CurvePopupUI.inl"

static void DestroyControls() {
    g_pending = {};
    if (g_panel.hwnd) DestroyWindow(g_panel.hwnd);
    g_panel.hwnd = nullptr; s_panel_active = false;
    popup_ui::Cleanup();
    g_closing = false; g_transition = false; g_closeAfterPending = false;
    g_displayKeys.clear(); g_displayProps.clear(); g_target = {};
}
static void ClosePanel() {
    if (g_closing) return;
    g_closing = true; g_transition = true;
    native_graph::Close();
    CurvesDebugLog(L"Shortcut close: requesting native layer bars");
    g_panel.presetStatus = L"正在返回 AE 图层滑条…";
    UpdateControls();
}
static LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && wParam == PM_REMOVE) {
        auto message = reinterpret_cast<MSG*>(lParam);
        if ((message->message == WM_KEYUP || message->message == WM_SYSKEYUP) && message->wParam == g_shortcut)
            g_activationHeld = false;
        if ((message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) &&
            g_activationHeld && message->wParam == g_shortcut && (message->lParam & (1LL << 30)))
            message->message = WM_NULL;
        if ((message->hwnd == g_panel.hwnd || IsChild(g_panel.hwnd, message->hwnd)) &&
            (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)) {
            bool match = g_shortcut && message->wParam == g_shortcut &&
                bool(GetKeyState(VK_CONTROL) & 0x8000) == g_ctrl &&
                bool(GetKeyState(VK_MENU) & 0x8000) == g_alt && bool(GetKeyState(VK_SHIFT) & 0x8000) == g_shift;
            if (message->wParam == VK_ESCAPE || match) {
                if (!(message->lParam & (1LL << 30))) { Queue(ActionKind::Close); g_activationHeld = match; }
                message->message = WM_NULL;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}
static LRESULT CALLBACK ControlsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: popup_ui::Paint(hwnd); return 0;
    case WM_DPICHANGED:
        popup_ui::ChangeDpi(hwnd, HIWORD(wParam), *reinterpret_cast<RECT*>(lParam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DRAWITEM:
        if (popup_ui::DrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_MEASUREITEM:
        if (popup_ui::MeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) return TRUE;
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(popup_ui::ControlColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam), message));
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->code == NM_CUSTOMDRAW) return popup_ui::CustomDraw(reinterpret_cast<NMHDR*>(lParam));
        break;
    case WM_NCHITTEST: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }; ScreenToClient(hwnd, &point);
        RECT client{}; GetClientRect(hwnd, &client);
        if (point.y >= 0 && point.y < popup_ui::HeaderHeight() && point.x < client.right - popup_ui::HeaderHeight()) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE: Queue(ActionKind::Close); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wParam), event = HIWORD(wParam);
        if (event == CBN_SELCHANGE) {
            int selected = int(SendMessageW(HWND(lParam), CB_GETCURSEL, 0, 0));
            if (id == PROP) Queue(ActionKind::Property, selected);
            if (id == SEGMENT) Queue(ActionKind::Segment, selected);
            if (id == DIMENSION) Queue(ActionKind::Dimension, selected);
            if (id == LIBRARY) g_panel.libraryIndex = selected;
        } else if (event == BN_CLICKED) {
            if (id >= PRESET && id < PRESET + 6) Queue(ActionKind::Preset, id - PRESET);
            else if (id == REFRESH) Queue(ActionKind::Refresh);
            else if (id == SAVE) Queue(ActionKind::Save);
            else if (id == APPLY) Queue(ActionKind::Apply, g_panel.libraryIndex);
            else if (id == DELETE_PRESET) Queue(ActionKind::Delete, g_panel.libraryIndex);
            else if (id == RELOAD) Queue(ActionKind::Reload);
            else if (id == RETURN_LAYERS) Queue(ActionKind::Close);
        }
        return 0;
    }
    case WM_HSCROLL:
        SliderLabels();
        if (LOWORD(wParam) == TB_ENDTRACK) Queue(ActionKind::Handles);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
static void CreateControls() {
    HMONITOR monitor = MonitorFromPoint(g_popupOrigin, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) SystemParametersInfoW(SPI_GETWORKAREA, 0, &info.rcWork, 0);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = GetDpiForWindow(g_ae_main);
    if (!dpiX) dpiX = 96;
    popup_ui::Initialize(dpiX);
    INITCOMMONCONTROLSEX common{ sizeof(common), ICC_BAR_CLASSES }; InitCommonControlsEx(&common);
    WNDCLASSW wc{}; wc.hInstance = g_module; wc.lpfnWndProc = ControlsWndProc;
    wc.lpszClassName = L"DalimaoCurveCursorPopup"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_DROPSHADOW; RegisterClassW(&wc);
    RECT bounds = popup_ui::CursorPlacement(g_popupOrigin, info.rcWork, dpiX);
    g_panel.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Dalimao Curves · 鼠标浮窗",
        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        g_ae_main, nullptr, g_module, nullptr);
    if (!g_panel.hwnd) { popup_ui::Cleanup(); throw std::runtime_error("Unable to create cursor popup"); }
    popup_ui::CreateChildren(g_panel.hwnd);
    ShowWindow(g_panel.hwnd, SW_SHOWNOACTIVATE);
    wchar_t placement[256]{};
    swprintf_s(placement, L"Cursor popup: origin=(%ld,%ld) bounds=(%ld,%ld,%ld,%ld) dpi=%u",
        g_popupOrigin.x, g_popupOrigin.y, bounds.left, bounds.top, bounds.right, bounds.bottom, dpiX);
    CurvesDebugLog(placement);
}
static BOOL CALLBACK FindAeWindow(HWND hwnd, LPARAM result) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid); wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    if (pid == GetCurrentProcessId() && (wcsstr(cls, L"AE_CApplication_") || wcsstr(cls, L"AfterFX")) && IsWindowVisible(hwnd)) {
        *reinterpret_cast<HWND*>(result) = hwnd; return FALSE;
    }
    return TRUE;
}
static void CaptureShortcut() {
    g_shortcut = 0;
    // The command hook observes the binding actually used, including unsaved
    // keyboard profiles. Menu activation does not invent a shortcut.
    for (int key = VK_BACK; key < 255; ++key) {
        if (key == VK_RETURN || key == VK_ESCAPE || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
            key == VK_CAPITAL || key == VK_NUMLOCK || key == VK_SCROLL || key == VK_LWIN || key == VK_RWIN ||
            (key >= VK_LSHIFT && key <= VK_RMENU)) continue;
        if (GetKeyState(key) & 0x8000) { g_shortcut = key; break; }
    }
    g_ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    g_alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    g_shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    g_activationHeld = g_shortcut != 0;
}
static A_Err CommandHook(AEGP_GlobalRefcon, AEGP_CommandRefcon, AEGP_Command command,
    AEGP_HookPriority, A_Boolean, A_Boolean* handled) {
    if (command != g_command) return A_Err_NONE;
    *handled = TRUE;
    try {
        if (s_panel_active) { g_activationHeld = g_shortcut != 0; Queue(ActionKind::Close); return A_Err_NONE; }
        GetCursorPos(&g_popupOrigin);
        CaptureShortcut(); EnumWindows(FindAeWindow, LPARAM(&g_ae_main));
        if (g_shortcut == VK_F3 && g_shift && !g_ctrl && !g_alt) {
            MessageBoxW(g_ae_main, L"Shift+F3 用于 AE 原生曲线切换。请给 DalimaoCurves 分配其他快捷键，例如 4。", L"Dalimao Curves", MB_OK);
            return A_Err_NONE;
        }
        if (!g_ae_main || !native_graph::Open(g_ae_main)) { CurvesDebugLog(native_graph::Status().c_str()); return A_Err_NONE; }
        CurvesDebugLog(L"Shortcut open: requesting native Graph Editor; no custom graph");
        g_panel = {}; s_panel_active = true; g_transition = true;
        LoadPresetLibrary(); CreateControls();
        Action initial; initial.kind = ActionKind::Refresh; ProcessAction(initial);
    } catch (...) { CurvesDebugLog(L"Native curve controls command failed"); native_graph::Close(); DestroyControls(); }
    try { ReleaseHostData(); } catch (...) { CurvesDebugLog(L"Host resource cleanup failed"); }
    return A_Err_NONE;
}
static A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* sleep) {
    if (!s_panel_active) return A_Err_NONE;
    *sleep = std::min<A_long>(*sleep, 6);
    if (g_transition && !native_graph::Busy()) {
        CurvesDebugLog(native_graph::Status().c_str());
        g_transition = false;
        if (native_graph::Failed()) {
            g_closing = false;
            g_panel.presetStatus = L"原生面板切换未完成，请保持 AE 在前台后重试：" + native_graph::Status();
            CurvesDebugLog(g_panel.presetStatus.c_str()); UpdateControls();
        } else if (g_closing) { DestroyControls(); return A_Err_NONE; }
        else UpdateControls();
    }
    Action action = g_pending; g_pending = {};
    if (action.kind == ActionKind::None && !g_transition && !g_closing &&
        GetTickCount64() - g_lastSync > 500 && GetForegroundWindow() != g_panel.hwnd) {
        g_lastSync = GetTickCount64();
        action.kind = ActionKind::Sync; action.target = g_target;
        action.segment = g_panel.presetSegment; action.dim = g_panel.presetDim;
    }
    if (action.kind == ActionKind::None) return A_Err_NONE;
    try {
        if (action.kind == ActionKind::Close) ClosePanel();
        else ProcessAction(action);
    } catch (...) { g_panel.presetStatus = L"AE 操作失败，已停止本次操作；请重新读取选择"; UpdateControls(); }
    try { ReleaseHostData(); } catch (...) { CurvesDebugLog(L"Host resource cleanup failed"); }
    if (g_closeAfterPending) { g_closeAfterPending = false; Queue(ActionKind::Close); }
    return A_Err_NONE;
}
static A_Err UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType) {
    try { AEGP_SuiteHandler(g_sp).CommandSuite1()->AEGP_EnableCommand(g_command); } catch (...) {}
    return A_Err_NONE;
}
static A_Err DeathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon) {
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    if (g_panel.hwnd) DestroyWindow(g_panel.hwnd);
    popup_ui::Cleanup();
    native_graph::Shutdown(); return A_Err_NONE;
}
extern "C" DllExport A_Err EntryPointFunc(SPBasicSuite* basic, A_long, A_long,
    AEGP_PluginID pluginId, AEGP_GlobalRefcon*) {
    try {
        g_sp = basic; g_plugin_id = pluginId;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&EntryPointFunc), &g_module);
        wchar_t path[MAX_PATH * 2]{}; GetModuleFileNameW(g_module, path, MAX_PATH * 2);
        g_plugin_dir = std::filesystem::path(path).parent_path().wstring();
        AEGP_SuiteHandler suites(basic); A_Err err = A_Err_NONE;
        ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&g_command));
        ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(g_command, "DalimaoCurves", AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));
        ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(pluginId, AEGP_HP_BeforeAE, g_command, CommandHook, nullptr));
        ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(pluginId, UpdateMenuHook, nullptr));
        ERR(suites.RegisterSuite5()->AEGP_RegisterIdleHook(pluginId, IdleHook, nullptr));
        ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(pluginId, DeathHook, nullptr));
        if (!err) g_keyboardHook = SetWindowsHookExW(WH_GETMESSAGE, KeyboardHook, nullptr, GetCurrentThreadId());
        CurvesDebugLog(L"Dalimao Curves 2 loaded: native graph, presets, templates and handle sliders"); return err;
    } catch (...) { return A_Err_GENERIC; }
}
