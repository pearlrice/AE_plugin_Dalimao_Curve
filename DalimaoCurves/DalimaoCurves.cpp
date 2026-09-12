// DalimaoCurves.cpp - AEGP plugin: keyframe Bezier curve panel for After Effects
//
// 呼出方式 (与 DalimaoShortcuts 相同):
//   在 AE「编辑 > 键盘快捷键」里给 Window 菜单命令「DalimaoCurves」绑定快捷键,
//   按下快捷键 -> CommandHook -> 弹出曲线面板 (Win32 分层窗口 + GDI+ 绘制)。
//
// 曲线面板:
//   - 顶部属性下拉框: 列出选中图层中所有带关键帧的属性, 点选切换;
//   - 多维属性 (位置/缩放等) 提供 X/Y/Z 维度切换;
//   - 图形区: 按 AE 实际求值采样绘制曲线, 关键帧为圆点;
//   - 选中关键帧后显示左右贝塞尔手柄 (入/出缓动):
//       水平拖动 = 改影响 influence, 垂直拖动 = 改速度 speed;
//   - 拖动关键帧圆点可改该维度的值;
//   - 修改实时写回 AE (可通过 Ctrl+Z 撤销)。
//
// 关闭方式 (与 DalimaoShortcuts 的搜索菜单相同):
//   鼠标移出面板外一定距离自动关闭; Esc / 右键也可关闭。

#include "pch.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

#include "AEConfig.h"
#include "entry.h"
#include "AE_GeneralPlug.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// Required by AEGP_SuiteHandler (SDK)
void AEGP_SuiteHandler::MissingSuiteError() const {
    throw std::runtime_error("AE Suite not available");
}

// ---------------- Layout ----------------
static const int CP_W = 720;
static const int CP_H = 400;
static const int GRAPH_L = 56;        // left margin reserved for Y-axis labels
static const int GRAPH_T = 76;
static const int GRAPH_R = CP_W - 16;
static const int GRAPH_B = CP_H - 64;
static const int BOTTOM_T = CP_H - 58;
static const int LEAVE_MARGIN = 40;       // 移出面板超过该距离即关闭
static const int DD_ROW_H = 24;
static const int DD_MAX_VISIBLE = 10;
static const int CURVE_SAMPLES = 128;

enum GraphMode {
    GRAPH_VALUE = 0,
    GRAPH_SPEED = 1
};

enum DragMode {
    DRAG_NONE = 0,
    DRAG_KEY,       // dragging a keyframe point (value)
    DRAG_IN,        // dragging the incoming ease handle (value graph)
    DRAG_OUT,       // dragging the outgoing ease handle (value graph)
    DRAG_SPEED_IN,  // dragging the incoming speed point (speed graph)
    DRAG_SPEED_OUT, // dragging the outgoing speed point (speed graph)
    DRAG_PAN,       // dragging empty graph space to pan the view
    DRAG_SCROLL     // dragging the bottom scrollbar
};

// ---------------- Globals ----------------
static SPBasicSuite* g_sp = nullptr;
static AEGP_PluginID g_plugin_id = 0;
static AEGP_Command  g_command = 0;
static HWND          g_ae_main = NULL;
static std::wstring  g_plugin_dir;
static bool          s_panel_active = false;

struct PropertyInfo {
    AEGP_StreamRefH streamH = NULL;
    std::wstring    name;
    AEGP_StreamType type = AEGP_StreamType_OneD;
    int numDims = 1;
    int temporalDims = 1;
    int numKFs = 0;
    bool hasRange = false;
    double minV = 0.0;
    double maxV = 0.0;
};

struct KfInfo {
    double time = 0.0;
    AEGP_StreamValue2 value{};   // snapshot of the full stream value (all dims)
    double curValue[3] = { 0.0, 0.0, 0.0 };
    AEGP_KeyframeEase easeIn[3]{};
    AEGP_KeyframeEase easeOut[3]{};
    AEGP_KeyframeInterpolationType inInterp = AEGP_KeyInterp_LINEAR;
    AEGP_KeyframeInterpolationType outInterp = AEGP_KeyInterp_LINEAR;
};

struct PanelState {
    HWND hwnd = NULL;
    ULONG_PTR gdipToken = 0;
    bool finished = false;
    bool toast = false;
    std::wstring toastMsg;

    bool dragging = false;
    int dragMode = DRAG_NONE;
    int dragStartX = 0, dragStartY = 0;
    int dragAxis = 0;               // 0 = undecided, 1 = value drag, 2 = time drag
    double dragStartTime = 0.0;
    double dragStartVal = 0.0, dragStartSpeed = 0.0, dragStartInfluence = 0.0;
    double segWpx = 1.0, pxPerSpeed = 1.0, pxPerValue = 1.0;

    int mx = -1000, my = -1000;
    bool dropdownOpen = false;
    int ddHover = -1;
    int ddScroll = 0;
    int selKf = -1;
    int curProp = -1;
    int dimMask = 3;             // which axes are shown (bit 0 = X, 1 = Y, 2 = Z); default X+Y
    int dragDim = 0;             // dimension the current drag is editing
    int graphMode = GRAPH_VALUE;

    bool moving = false;               // middle-button panel drag
    int moveStartX = 0, moveStartY = 0;
    int winStartX = 0, winStartY = 0;
};

static PanelState g_panel;
static std::vector<PropertyInfo> g_props;
static std::vector<KfInfo> g_kfs;
static AEGP_LayerH g_layerH = NULL;
static double g_tMin = 0.0, g_tMax = 1.0;
static double g_vMinD[3] = { 0.0, 0.0, 0.0 };
static double g_vMaxD[3] = { 1.0, 1.0, 1.0 };
static double g_sMaxD[3] = { 1.0, 1.0, 1.0 };
static double g_pxPerSec = 100.0;      // graph horizontal zoom (pixels per second)
static double g_viewT0 = 0.0;          // time at the left edge of the graph
static HWND    g_ae_under = NULL;      // AE window under the cursor when the panel opened
static HWND    g_tl_hwnd = NULL;       // timeline window used for calibration
static int     g_tl_left = 0;          // screen x of the timeline's left edge
static int     g_tl_rulerY = 0;        // screen y of the timeline's time ruler row
static double  g_ruler_margin = 45.0;  // px from the window left to the ruler's time-0 edge
static double  g_tl_T0 = 0.0;          // timeline display start (left edge time)
static double  g_tl_span = 3.0;        // timeline's visible time span
static double  g_tl_px = 500.0;        // timeline's absolute pixels-per-second
static double  g_auto_px = 0.0;        // last auto-calibrated px/s
static double  g_manual_ratio = 0.0;   // user's manual correction ratio (0 = unset)

// ---------------- Debug log ----------------
static void CurvesDebugLog(const wchar_t* msg) {
    if (g_plugin_dir.empty()) return;
    CreateDirectoryW((g_plugin_dir + L"\\debug").c_str(), NULL);
    FILE* f = NULL;
    _wfopen_s(&f, (g_plugin_dir + L"\\debug\\DalimaoCurves.log").c_str(),
              L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    fclose(f);
}

static void LoadPanelSettings() {
    if (g_plugin_dir.empty()) return;
    FILE* f = NULL;
    _wfopen_s(&f, (g_plugin_dir + L"\\panel_settings.txt").c_str(), L"r, ccs=UTF-8");
    if (!f) return;
    double r = 0.0;
    if (fwscanf_s(f, L"%lf", &r) == 1 && r > 0.05 && r < 20.0) {
        g_manual_ratio = r;
        wchar_t lb[128];
        _snwprintf_s(lb, 128, _TRUNCATE, L"manual ratio loaded: %.4f", r);
        CurvesDebugLog(lb);
    }
    fclose(f);
}

static void SavePanelSettings() {
    if (g_plugin_dir.empty()) return;
    FILE* f = NULL;
    _wfopen_s(&f, (g_plugin_dir + L"\\panel_settings.txt").c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%.4f\n", g_manual_ratio);
    fclose(f);
    wchar_t lb[128];
    _snwprintf_s(lb, 128, _TRUNCATE, L"manual ratio saved: %.4f", g_manual_ratio);
    CurvesDebugLog(lb);
}

static double EffectivePx(double autoPx) {
    double p = autoPx;
    if (g_manual_ratio > 0.0) p = autoPx * g_manual_ratio;
    if (p < 1.0) p = 1.0;
    if (p > 4000.0) p = 4000.0;
    return p;
}

// ---------------- Small helpers ----------------
static bool PointInRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static double TimeToSec(const A_Time& t) {
    return t.scale ? (double)t.value / (double)t.scale : 0.0;
}

static A_Time SecToTime(double s) {
    A_Time t;
    t.scale = 1000000;
    t.value = (A_long)(s * 1000000.0 + 0.5);
    return t;
}

static double ClampD(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static int ClampI(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double GetDimValue(const AEGP_StreamVal2& v, AEGP_StreamType t, int d) {
    switch (t) {
        case AEGP_StreamType_OneD:          return v.one_d;
        case AEGP_StreamType_TwoD:
        case AEGP_StreamType_TwoD_SPATIAL:  return d == 0 ? v.two_d.x : v.two_d.y;
        case AEGP_StreamType_ThreeD:
        case AEGP_StreamType_ThreeD_SPATIAL:
            return d == 0 ? v.three_d.x : (d == 1 ? v.three_d.y : v.three_d.z);
        default: return 0.0;
    }
}

static void SetDimValue(AEGP_StreamVal2& v, AEGP_StreamType t, int d, double val) {
    switch (t) {
        case AEGP_StreamType_OneD:          v.one_d = (A_FpLong)val; break;
        case AEGP_StreamType_TwoD:
        case AEGP_StreamType_TwoD_SPATIAL:
            if (d == 0) v.two_d.x = (A_FpLong)val;
            else        v.two_d.y = (A_FpLong)val;
            break;
        case AEGP_StreamType_ThreeD:
        case AEGP_StreamType_ThreeD_SPATIAL:
            if (d == 0)      v.three_d.x = (A_FpLong)val;
            else if (d == 1) v.three_d.y = (A_FpLong)val;
            else             v.three_d.z = (A_FpLong)val;
            break;
        default: break;
    }
}

// ---------------- Stream walk ----------------
static std::wstring StreamName(AEGP_SuiteHandler& suites, AEGP_StreamRefH h) {
    std::wstring out;
    AEGP_MemHandle mh = NULL;
    if (!suites.StreamSuite6()->AEGP_GetStreamName(g_plugin_id, h, FALSE, &mh) && mh) {
        void* buf = NULL;
        if (!suites.MemorySuite1()->AEGP_LockMemHandle(mh, &buf) && buf) {
            out.assign((const wchar_t*)buf);
            suites.MemorySuite1()->AEGP_UnlockMemHandle(mh);
        }
        suites.MemorySuite1()->AEGP_FreeMemHandle(mh);
    }
    return out;
}

static std::wstring ItemName(AEGP_SuiteHandler& suites, AEGP_ItemH itemH) {
    std::wstring out;
    AEGP_MemHandle mh = NULL;
    if (!suites.ItemSuite9()->AEGP_GetItemName(g_plugin_id, itemH, &mh) && mh) {
        void* buf = NULL;
        if (!suites.MemorySuite1()->AEGP_LockMemHandle(mh, &buf) && buf) {
            out.assign((const wchar_t*)buf);
            suites.MemorySuite1()->AEGP_UnlockMemHandle(mh);
        }
        suites.MemorySuite1()->AEGP_FreeMemHandle(mh);
    }
    return out;
}

static void WalkStreams(AEGP_SuiteHandler& suites, AEGP_StreamRefH s,
                        const std::wstring& prefix, int depth,
                        std::vector<PropertyInfo>& out,
                        std::vector<AEGP_StreamRefH>& owned) {
    AEGP_StreamGroupingType gt = AEGP_StreamGroupingType_NONE;
    if (suites.DynamicStreamSuite4()->AEGP_GetStreamGroupingType(s, &gt)) return;

    if (gt == AEGP_StreamGroupingType_LEAF) {
        AEGP_StreamType st = AEGP_StreamType_NO_DATA;
        A_long nkfs = 0;
        if (suites.StreamSuite6()->AEGP_GetStreamType(s, &st)) return;
        bool numeric = (st == AEGP_StreamType_OneD ||
                        st == AEGP_StreamType_TwoD ||
                        st == AEGP_StreamType_TwoD_SPATIAL ||
                        st == AEGP_StreamType_ThreeD ||
                        st == AEGP_StreamType_ThreeD_SPATIAL);
        if (!numeric) return;
        if (suites.KeyframeSuite5()->AEGP_GetStreamNumKFs(s, &nkfs) || nkfs <= 0) return;

        PropertyInfo pi;
        pi.streamH = s;
        pi.type = st;
        pi.numKFs = (int)nkfs;
        A_short dims = 1, tdims = 1;
        suites.KeyframeSuite5()->AEGP_GetStreamValueDimensionality(s, &dims);
        suites.KeyframeSuite5()->AEGP_GetStreamTemporalDimensionality(s, &tdims);
        pi.numDims = dims;
        pi.temporalDims = tdims;

        std::wstring leaf = StreamName(suites, s);
        pi.name = prefix.empty() ? leaf : (leaf.empty() ? prefix : prefix + L" > " + leaf);
        out.push_back(pi);
        owned.push_back(s);   // this ref stays alive until the panel session ends
        return;
    }

    A_long n = 0;
    if (suites.DynamicStreamSuite4()->AEGP_GetNumStreamsInGroup(s, &n)) return;
    std::wstring sub = prefix;
    if (gt == AEGP_StreamGroupingType_NAMED_GROUP && depth > 0) {
        std::wstring gname = StreamName(suites, s);
        if (!gname.empty()) sub = prefix.empty() ? gname : prefix + L" > " + gname;
    }
    for (A_long i = 0; i < n; i++) {
        AEGP_StreamRefH child = NULL;
        if (suites.DynamicStreamSuite4()->AEGP_GetNewStreamRefByIndex(g_plugin_id, s, i, &child)) continue;
        WalkStreams(suites, child, sub, depth + 1, out, owned);
        bool adopted = false;
        for (AEGP_StreamRefH r : owned) {
            if (r == child) { adopted = true; break; }
        }
        if (!adopted) suites.StreamSuite6()->AEGP_DisposeStream(child);
    }
}

static bool CollectProperties(AEGP_SuiteHandler& suites, AEGP_LayerH layer,
                              std::vector<PropertyInfo>& out) {
    AEGP_StreamRefH root = NULL;
    if (suites.DynamicStreamSuite4()->AEGP_GetNewStreamRefForLayer(g_plugin_id, layer, &root)) return false;
    std::vector<AEGP_StreamRefH> owned;
    WalkStreams(suites, root, L"", 0, out, owned);
    bool rootAdopted = false;
    for (AEGP_StreamRefH r : owned) {
        if (r == root) { rootAdopted = true; break; }
    }
    if (!rootAdopted) suites.StreamSuite6()->AEGP_DisposeStream(root);
    return !out.empty();
}

static void DisposeProperties(AEGP_SuiteHandler& suites) {
    for (auto& p : g_props) {
        if (p.streamH) suites.StreamSuite6()->AEGP_DisposeStream(p.streamH);
    }
    g_props.clear();
}

static void DisposeKeyframes(AEGP_SuiteHandler& suites) {
    for (auto& kf : g_kfs) {
        suites.StreamSuite6()->AEGP_DisposeStreamValue(&kf.value);
    }
}

// ---------------- Keyframe data ----------------
static bool LoadKeyframes(AEGP_SuiteHandler& suites, const PropertyInfo& pi,
                          std::vector<KfInfo>& out,
                          double& tMin, double& tMax,
                          double vMinD[3], double vMaxD[3]) {
    out.clear();
    AEGP_KeyframeSuite5* ks = suites.KeyframeSuite5();
    int nd = pi.numDims > 3 ? 3 : pi.numDims;
    int te = pi.temporalDims > 0 ? pi.temporalDims : nd;
    for (int d = 0; d < 3; d++) vMinD[d] = vMaxD[d] = 0.0;
    tMin = tMax = 0.0;

    for (A_long i = 0; i < pi.numKFs; i++) {
        KfInfo kf;
        A_Time t;
        if (ks->AEGP_GetKeyframeTime(pi.streamH, i, AEGP_LTimeMode_CompTime, &t)) continue;
        kf.time = TimeToSec(t);
        AEGP_StreamValue2 sv{};
        if (ks->AEGP_GetNewKeyframeValue(g_plugin_id, pi.streamH, i, &sv)) continue;
        kf.value = sv;
        for (int d = 0; d < nd; d++) kf.curValue[d] = GetDimValue(sv.val, pi.type, d);
        for (int d = 0; d < te; d++)
            ks->AEGP_GetKeyframeTemporalEase(pi.streamH, i, d, &kf.easeIn[d], &kf.easeOut[d]);
        ks->AEGP_GetKeyframeInterpolation(pi.streamH, i, &kf.inInterp, &kf.outInterp);

        if (out.empty()) {
            tMin = tMax = kf.time;
            for (int d = 0; d < nd; d++) vMinD[d] = vMaxD[d] = kf.curValue[d];
        } else {
            if (kf.time < tMin) tMin = kf.time;
            if (kf.time > tMax) tMax = kf.time;
            for (int d = 0; d < nd; d++) {
                if (kf.curValue[d] < vMinD[d]) vMinD[d] = kf.curValue[d];
                if (kf.curValue[d] > vMaxD[d]) vMaxD[d] = kf.curValue[d];
            }
        }
        out.push_back(kf);
    }
    if (out.empty()) return false;

    double tPad = (tMax - tMin) * 0.06;
    if (tPad <= 0.0) tPad = 1.0;
    tMin -= tPad; tMax += tPad;
    for (int d = 0; d < nd; d++) {
        double vPad = (vMaxD[d] - vMinD[d]) * 0.15;
        if (vPad <= 0.0) vPad = fabs(vMaxD[d]) * 0.05 + 1.0;
        vMinD[d] -= vPad; vMaxD[d] += vPad;
        if (vMaxD[d] - vMinD[d] < 1e-9) { vMinD[d] -= 1.0; vMaxD[d] += 1.0; }
    }
    return true;
}

static double SampleValue(AEGP_SuiteHandler& suites, AEGP_StreamRefH h,
                          AEGP_StreamType type, int dim, double tSec) {
    A_Time t = SecToTime(tSec);
    AEGP_StreamValue2 sv{};
    if (suites.StreamSuite6()->AEGP_GetNewStreamValue(
            g_plugin_id, h, AEGP_LTimeMode_CompTime, &t, TRUE, &sv)) {
        return 0.0;
    }
    double v = GetDimValue(sv.val, type, dim);
    suites.StreamSuite6()->AEGP_DisposeStreamValue(&sv);
    return v;
}

// Center the view on the keyframe time range at the current zoom scale.
static void CenterViewOnKeys() {
    if (g_kfs.empty()) return;
    double tMin = g_kfs[0].time, tMax = g_kfs[0].time;
    for (auto& kf : g_kfs) {
        if (kf.time < tMin) tMin = kf.time;
        if (kf.time > tMax) tMax = kf.time;
    }
    double visible = (double)(GRAPH_R - GRAPH_L) / g_pxPerSec;
    double span = tMax - tMin;
    double pad = visible * 0.08;
    if (span + pad * 2.0 <= visible) {
        g_viewT0 = tMin - pad;
    } else {
        g_viewT0 = (tMin + tMax) * 0.5 - visible * 0.5;
    }
}

static void ReloadKeyframes() {
    AEGP_SuiteHandler suites(g_sp);
    g_kfs.clear();
    g_panel.selKf = -1;
    if (g_panel.curProp < 0 || g_panel.curProp >= (int)g_props.size()) return;
    const PropertyInfo& pi = g_props[g_panel.curProp];
    double vmin[3] = { 0.0, 0.0, 0.0 }, vmax[3] = { 1.0, 1.0, 1.0 };
    if (!LoadKeyframes(suites, pi, g_kfs, g_tMin, g_tMax, vmin, vmax)) {
        g_panel.curProp = -1;
        return;
    }
    for (int d = 0; d < 3; d++) { g_vMinD[d] = vmin[d]; g_vMaxD[d] = vmax[d]; }
    if (!g_tl_hwnd) {
        // not calibrated to the timeline yet: fit the keys
        double span = g_tMax - g_tMin;
        if (span < 1e-6) span = 1.0;
        g_pxPerSec = (GRAPH_R - GRAPH_L) / span;
        if (g_pxPerSec > 2000.0) g_pxPerSec = 2000.0;
        if (g_pxPerSec < 0.5) g_pxPerSec = 0.5;
        CenterViewOnKeys();
    } else {
        // calibrated: use the timeline's absolute scale so key distances match
        g_viewT0 = g_tl_T0;
        g_pxPerSec = EffectivePx(g_tl_px);
    }
    AEGP_StreamFlags flags = 0;
    A_FpLong mn = 0, mx = 0;
    PropertyInfo& p = g_props[g_panel.curProp];
    p.hasRange = false;
    if (!suites.StreamSuite6()->AEGP_GetStreamProperties(p.streamH, &flags, &mn, &mx)) {
        p.hasRange = (flags & AEGP_StreamFlag_HAS_MIN) && (flags & AEGP_StreamFlag_HAS_MAX);
        p.minV = (double)mn;
        p.maxV = (double)mx;
    }
}

// ---------------- AE writes ----------------
static void ApplyEase(int idx, int dim) {
    if (idx < 0 || idx >= (int)g_kfs.size()) return;
    AEGP_SuiteHandler suites(g_sp);
    PropertyInfo& pi = g_props[g_panel.curProp];
    KfInfo& kf = g_kfs[idx];
    AEGP_KeyframeEase in = kf.easeIn[dim], out = kf.easeOut[dim];
    AEGP_KeyframeInterpolationType inI = kf.inInterp, outI = kf.outInterp;
    suites.KeyframeSuite5()->AEGP_SetKeyframeInterpolation(pi.streamH, idx, inI, outI);
    suites.KeyframeSuite5()->AEGP_SetKeyframeTemporalEase(pi.streamH, idx, dim, &in, &out);
}

static void SetKeyValue(int idx, int dim, double v) {
    if (idx < 0 || idx >= (int)g_kfs.size()) return;
    AEGP_SuiteHandler suites(g_sp);
    PropertyInfo& pi = g_props[g_panel.curProp];
    KfInfo& kf = g_kfs[idx];
    if (pi.hasRange) v = ClampD(v, pi.minV, pi.maxV);
    AEGP_StreamValue2 sv = kf.value;
    SetDimValue(sv.val, pi.type, dim, v);
    if (!suites.KeyframeSuite5()->AEGP_SetKeyframeValue(pi.streamH, idx, &sv)) {
        kf.value = sv;
        kf.curValue[dim] = GetDimValue(sv.val, pi.type, dim);
    }
}

// AE has no "set keyframe time" API; move a key by delete + re-insert at the
// new time, restoring value / eases / interpolation.
static void ApplyKeyTimeMove(int idx) {
    if (idx < 0 || idx >= (int)g_kfs.size()) return;
    AEGP_SuiteHandler suites(g_sp);
    PropertyInfo& pi = g_props[g_panel.curProp];
    KfInfo& kf = g_kfs[idx];
    A_Time t = SecToTime(kf.time);
    if (suites.KeyframeSuite5()->AEGP_DeleteKeyframe(pi.streamH, idx)) return;
    AEGP_KeyframeIndex newIdx = 0;
    if (suites.KeyframeSuite5()->AEGP_InsertKeyframe(
            pi.streamH, AEGP_LTimeMode_CompTime, &t, &newIdx)) {
        return;
    }
    AEGP_StreamValue2 sv = kf.value;
    suites.KeyframeSuite5()->AEGP_SetKeyframeValue(pi.streamH, newIdx, &sv);
    int te = pi.temporalDims > 0 ? pi.temporalDims : (pi.numDims > 3 ? 3 : pi.numDims);
    if (te > 3) te = 3;
    for (int d = 0; d < te; d++) {
        AEGP_KeyframeEase in = kf.easeIn[d], out = kf.easeOut[d];
        suites.KeyframeSuite5()->AEGP_SetKeyframeTemporalEase(
            pi.streamH, newIdx, d, &in, &out);
    }
    suites.KeyframeSuite5()->AEGP_SetKeyframeInterpolation(
        pi.streamH, newIdx, kf.inInterp, kf.outInterp);
}

// ---------------- Geometry ----------------
static double TimeToX(double t) {
    return GRAPH_L + (t - g_viewT0) * g_pxPerSec;
}

static int SelectedDims(int* out) {
    int nd = 3;
    if (g_panel.curProp >= 0 && g_panel.curProp < (int)g_props.size())
        nd = g_props[g_panel.curProp].numDims;
    if (nd > 3) nd = 3;
    int n = 0;
    for (int d = 0; d < nd; d++)
        if (g_panel.dimMask & (1 << d)) out[n++] = d;
    if (n == 0) { out[0] = 0; n = 1; }
    return n;
}

static RECT RowRectForDim(int dim) {
    int dims[3];
    int n = SelectedDims(dims);
    for (int r = 0; r < n; r++) {
        if (dims[r] == dim) {
            int h = GRAPH_B - GRAPH_T;
            int top = GRAPH_T + MulDiv(r, h, n);
            int bot = GRAPH_T + MulDiv(r + 1, h, n);
            return { GRAPH_L, top, GRAPH_R, bot };
        }
    }
    return { GRAPH_L, GRAPH_T, GRAPH_R, GRAPH_B };
}

static double RowValueToY(const RECT& row, int dim, double v) {
    double vmin = g_vMinD[dim], vmax = g_vMaxD[dim];
    if (vmax - vmin < 1e-9) vmax = vmin + 1.0;
    return row.bottom - (v - vmin) / (vmax - vmin) * (double)(row.bottom - row.top);
}

static double RowSpeedToY(const RECT& row, int dim, double s) {
    double smax = g_sMaxD[dim];
    if (smax < 1e-9) smax = 1.0;
    return row.bottom - (s / smax) * (double)(row.bottom - row.top);
}

static double PxPerSpeedRow(const RECT& row, int dim) {
    // zoom-independent handle scale: one "value-range per second" of speed
    // maps to ~28% of the row height, so handles always stay near the keyframe
    double vRange = g_vMaxD[dim] - g_vMinD[dim];
    if (vRange <= 0.0) vRange = 1.0;
    double rowH = (double)(row.bottom - row.top);
    double r = rowH / vRange * 0.28;
    if (r < 0.001) r = 0.001;
    return r;
}

// ---------------- Scrollbar ----------------
static RECT ScrollRect() {
    return { 24, CP_H - 20, CP_W - 24, CP_H - 6 };
}

static RECT ScaleMinusRect() {
    return { CP_W - 58, BOTTOM_T + 2, CP_W - 36, BOTTOM_T + 20 };
}

static RECT ScalePlusRect() {
    return { CP_W - 32, BOTTOM_T + 2, CP_W - 12, BOTTOM_T + 20 };
}

static double ScrollRangeMin() {
    if (g_kfs.empty()) return g_viewT0;
    double mn = g_kfs[0].time;
    for (auto& k : g_kfs) if (k.time < mn) mn = k.time;
    return mn;
}

static double ScrollRangeMax() {
    if (g_kfs.empty()) return g_viewT0;
    double mx = g_kfs[0].time;
    for (auto& k : g_kfs) if (k.time > mx) mx = k.time;
    return mx;
}

static double VisibleSpanSec() {
    return (double)(GRAPH_R - GRAPH_L) / g_pxPerSec;
}

static void ScrollThumb(double& thumbLeft, double& thumbW) {
    RECT tr = ScrollRect();
    double trackW = (double)(tr.right - tr.left);
    double total = ScrollRangeMax() - ScrollRangeMin() + VisibleSpanSec();
    if (total <= 0.0) total = 1.0;
    thumbW = trackW * VisibleSpanSec() / total;
    if (thumbW < 24.0) thumbW = 24.0;
    if (thumbW > trackW) thumbW = trackW;
    double lo = ScrollRangeMin();
    double hi = ScrollRangeMax() - VisibleSpanSec();
    double f = 0.0;
    if (hi - lo > 1e-9) {
        f = (g_viewT0 - lo) / (hi - lo);
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
    }
    thumbLeft = (double)tr.left + f * (trackW - thumbW);
}

static double Dist(double x1, double y1, double x2, double y2) {
    double dx = x1 - x2, dy = y1 - y2;
    return sqrt(dx * dx + dy * dy);
}

// ---------------- UI rects ----------------
static RECT PropButtonRect() {
    return { 132, 10, CP_W - 96, 36 };
}

static RECT ModeButtonRect(int m) {
    return { (m == 0) ? CP_W - 90 : CP_W - 46, 10,
             (m == 0) ? CP_W - 48 : CP_W - 12, 36 };
}

static RECT DimButtonRect(int d) {
    return { 14 + d * 44, 44, 14 + d * 44 + 38, 68 };
}

static RECT InterpButtonRect(int i) {
    return { 150 + i * 53, 44, 150 + i * 53 + 50, 68 };
}

static RECT DropdownRect() {
    int n = (int)g_props.size();
    int rows = ClampI(n - g_panel.ddScroll, 0, DD_MAX_VISIBLE);
    return { 132, 40, CP_W - 96, 40 + rows * DD_ROW_H };
}

// ---------------- Drawing ----------------
static void DrawText(Graphics& g, const std::wstring& s, Font& f,
                     const SolidBrush& b, float x, float y) {
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(s.c_str(), (INT)s.size(), &f, PointF(x, y), &sf, &b);
}

static void DrawTextCentered(Graphics& g, const std::wstring& s, Font& f,
                             const SolidBrush& b, const RECT& r) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    RectF rf((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
    g.DrawString(s.c_str(), (INT)s.size(), &f, rf, &sf, &b);
}

static void DrawTextRight(Graphics& g, const std::wstring& s, Font& f,
                          const SolidBrush& b, float xRight, float y) {
    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetAlignment(StringAlignmentFar);
    g.DrawString(s.c_str(), (INT)s.size(), &f, RectF(xRight - 46, y, 46, 14), &sf, &b);
}

static void DrawInterpIcon(Graphics& g, const RECT& r, int type, const SolidBrush& brush) {
    Pen pen(Color(255, 226, 226, 226), 1.8f);
    // pick the visible color from the brush's ARGB
    Color c;
    brush.GetColor(&c);
    pen.SetColor(c);
    float L = (float)r.left + 7.0f, R = (float)r.right - 7.0f;
    float T = (float)r.top + 7.0f, B = (float)r.bottom - 7.0f;
    float M = (T + B) * 0.5f;
    switch (type) {
        case 0:  // linear
            g.DrawLine(&pen, L, B, R, T);
            break;
        case 1:  // bezier (gentle S)
            g.DrawBezier(&pen, L, B, L + (R - L) * 0.25f, T, L + (R - L) * 0.75f, B, R, T);
            break;
        case 2:  // hold (step)
            g.DrawLine(&pen, L, M, R - 12.0f, M);
            g.DrawLine(&pen, R - 12.0f, M, R - 12.0f, T);
            g.DrawLine(&pen, R - 12.0f, T, R, T);
            break;
        case 3:  // easy ease (S)
            g.DrawBezier(&pen, L, B, L + (R - L) * 0.10f, B, L + (R - L) * 0.90f, T, R, T);
            break;
        case 4:  // easy ease in (flat start, steep end)
            g.DrawBezier(&pen, L, B, L + (R - L) * 0.15f, B - 1.0f, L + (R - L) * 0.85f, T + 1.0f, R, T);
            break;
        case 5:  // easy ease out (steep start, flat end)
            g.DrawBezier(&pen, L, B, L + (R - L) * 0.15f, B + 1.0f, L + (R - L) * 0.85f, T - 1.0f, R, T);
            break;
    }
}

// Feathered drop shadow behind the drawn content (straight-alpha compositing).
static void ApplyPanelShadow(unsigned char* bits, int w, int h,
                             int ox, int oy, int radius, int alpha) {
    std::vector<float> mask((size_t)w * h), tmp((size_t)w * h);
    for (int i = 0; i < w * h; i++) mask[i] = bits[i * 4 + 3] / 255.0f;
    int r = radius > 1 ? radius : 1;
    int len = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            float acc = 0.0f;
            for (int x = -r; x <= r; x++) {
                int xx = x < 0 ? 0 : (x >= w ? w - 1 : x);
                acc += mask[y * w + xx];
            }
            tmp[y * w] = acc / (float)len;
            for (int x = 1; x < w; x++) {
                int xa = x + r; if (xa >= w) xa = w - 1;
                int xs = x - r - 1; if (xs < 0) xs = 0;
                acc += mask[y * w + xa] - mask[y * w + xs];
                tmp[y * w + x] = acc / (float)len;
            }
        }
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int y = -r; y <= r; y++) {
                int yy = y < 0 ? 0 : (y >= h ? h - 1 : y);
                acc += tmp[yy * w + x];
            }
            mask[x] = acc / (float)len;
            for (int y = 1; y < h; y++) {
                int ya = y + r; if (ya >= h) ya = h - 1;
                int ys = y - r - 1; if (ys < 0) ys = 0;
                acc += tmp[ya * w + x] - tmp[ys * w + x];
                mask[y * w + x] = acc / (float)len;
            }
        }
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            float m = mask[sy * w + sx];
            if (m < 0.01f) continue;
            unsigned char* p = &bits[(y * w + x) * 4];
            float dstA = p[3] / 255.0f;
            float srcA = m * alpha / 255.0f;
            if (srcA <= 0.0f) continue;
            float outA = dstA + srcA * (1.0f - dstA);
            if (outA <= 0.001f) continue;
            // black shadow placed underneath: dst color stays on top
            float outR = (p[2] / 255.0f) * dstA / outA;
            float outG = (p[1] / 255.0f) * dstA / outA;
            float outB = (p[0] / 255.0f) * dstA / outA;
            p[0] = (unsigned char)(outB * 255.0f + 0.5f);
            p[1] = (unsigned char)(outG * 255.0f + 0.5f);
            p[2] = (unsigned char)(outR * 255.0f + 0.5f);
            p[3] = (unsigned char)(outA * 255.0f + 0.5f);
        }
    }
}

static void GetInHandle(const RECT& row, int dim, int idx, double& hx, double& hy) {
    KfInfo& kf = g_kfs[idx];
    double kx = TimeToX(kf.time), ky = RowValueToY(row, dim, kf.curValue[dim]);
    double seg = (idx > 0) ? (TimeToX(kf.time) - TimeToX(g_kfs[idx - 1].time)) : 0.0;
    hx = kx - kf.easeIn[dim].influenceF * seg;
    // Incoming handle is the vertical mirror of the outgoing one:
    // positive speed (fast arrival) puts the handle below the keyframe.
    hy = ky + kf.easeIn[dim].speedF * PxPerSpeedRow(row, dim);
    if (hx < GRAPH_L + 6) hx = GRAPH_L + 6;
    if (hx > GRAPH_R - 6) hx = GRAPH_R - 6;
    if (hy < row.top + 5) hy = row.top + 5;
    if (hy > row.bottom - 5) hy = row.bottom - 5;
}

static void GetOutHandle(const RECT& row, int dim, int idx, double& hx, double& hy) {
    KfInfo& kf = g_kfs[idx];
    double kx = TimeToX(kf.time), ky = RowValueToY(row, dim, kf.curValue[dim]);
    double seg = (idx < (int)g_kfs.size() - 1)
                     ? (TimeToX(g_kfs[idx + 1].time) - TimeToX(kf.time))
                     : 0.0;
    hx = kx + kf.easeOut[dim].influenceF * seg;
    hy = ky - kf.easeOut[dim].speedF * PxPerSpeedRow(row, dim);
    if (hx < GRAPH_L + 6) hx = GRAPH_L + 6;
    if (hx > GRAPH_R - 6) hx = GRAPH_R - 6;
    if (hy < row.top + 5) hy = row.top + 5;
    if (hy > row.bottom - 5) hy = row.bottom - 5;
}

static void RenderCurvePanel() {
    PanelState& p = g_panel;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = CP_W;
    bmi.bmiHeader.biHeight = -CP_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldBm = (HBITMAP)SelectObject(hdcMem, hbm);
    memset(bits, 0, CP_W * CP_H * 4);

    Graphics g(hdcMem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    Font titleFont(L"Microsoft YaHei UI", 13.0f, FontStyleBold, UnitPixel);
    Font uiFont(L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel);
    Font smallFont(L"Microsoft YaHei UI", 10.0f, FontStyleRegular, UnitPixel);
    SolidBrush bg(Color(255, 32, 32, 32));
    SolidBrush panelBg(Color(185, 24, 24, 27));
    SolidBrush panelBrush(Color(255, 42, 42, 42));
    SolidBrush textBrush(Color(255, 226, 226, 226));
    SolidBrush dimBrush(Color(255, 148, 148, 148));
    SolidBrush accentBrush(Color(255, 80, 160, 255));
    SolidBrush keyBrush(Color(255, 255, 200, 60));
    SolidBrush selBrush(Color(255, 255, 120, 60));
    SolidBrush handleBrush(Color(255, 255, 110, 110));
    SolidBrush dropBrush(Color(255, 52, 52, 52));
    SolidBrush hoverBrush(Color(255, 70, 70, 80));
    Pen borderPen(Color(255, 92, 92, 92));
    Pen curvePen(Color(255, 90, 180, 255), 2.0f);
    Pen handlePen(Color(255, 255, 110, 110), 1.5f);
    Pen ringPen(Color(255, 255, 255, 130), 1.5f);
    Pen accentPen(Color(255, 80, 160, 255), 1.0f);

    // semi-transparent panel background so the panel's extent is visible
    g.FillRectangle(&panelBg, 0, 0, CP_W, CP_H);
    g.DrawRectangle(&borderPen, 0, 0, CP_W - 1, CP_H - 1);

    if (p.toast) {
        g.FillRectangle(&bg, 24, 64, CP_W - 48, CP_H - 110);
        g.DrawRectangle(&borderPen, 24, 64, CP_W - 49, CP_H - 111);
        std::wstring msg = p.toastMsg.empty() ? L"" : p.toastMsg;
        DrawText(g, L"DalimaoCurves", titleFont, textBrush, 24.0f, 18.0f);
        RectF msgRf(40.0f, 140.0f, (float)(CP_W - 80), 120.0f);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(msg.c_str(), (INT)msg.size(), &uiFont, msgRf, &sf, &textBrush);
        DrawText(g, L"点击 / Esc / 右键 / 鼠标移出面板 关闭", smallFont, dimBrush,
                 40.0f, (float)(CP_H - 56));
    } else if (g_panel.curProp >= 0 && g_panel.curProp < (int)g_props.size()) {
        PropertyInfo& pi = g_props[g_panel.curProp];

        // ---- header ----
        DrawText(g, L"DalimaoCurves", titleFont, textBrush, 12.0f, 10.0f);
        DrawText(g, L"属性", uiFont, dimBrush, 112.0f, 13.0f);

        RECT pbr = PropButtonRect();
        g.FillRectangle(&panelBrush, (float)pbr.left, (float)pbr.top,
                        (float)(pbr.right - pbr.left), (float)(pbr.bottom - pbr.top));
        g.DrawRectangle(&borderPen, (float)pbr.left, (float)pbr.top,
                        (float)(pbr.right - pbr.left - 1), (float)(pbr.bottom - pbr.top - 1));
        std::wstring pname = pi.name;
        if (pname.size() > 22) pname = pname.substr(0, 21) + L"…";
        DrawText(g, pname + (p.dropdownOpen ? L"  ▲" : L"  ▼"), uiFont,
                 p.dropdownOpen ? accentBrush : textBrush, (float)(pbr.left + 6), 14.0f);

        // mode toggle: value graph / speed graph
        static const wchar_t* MODES[] = { L"值", L"速度" };
        for (int m = 0; m < 2; m++) {
            RECT mr = ModeButtonRect(m);
            bool active = (p.graphMode == m);
            g.FillRectangle(&panelBrush, (float)mr.left, (float)mr.top,
                            (float)(mr.right - mr.left), (float)(mr.bottom - mr.top));
            if (active) g.DrawRectangle(&accentPen, (float)mr.left, (float)mr.top,
                                        (float)(mr.right - mr.left - 1),
                                        (float)(mr.bottom - mr.top - 1));
            else g.DrawRectangle(&borderPen, (float)mr.left, (float)mr.top,
                                 (float)(mr.right - mr.left - 1),
                                 (float)(mr.bottom - mr.top - 1));
            DrawTextCentered(g, MODES[m], uiFont, active ? accentBrush : dimBrush, mr);
        }

        // dimension buttons
        if (pi.numDims > 1) {
            static const wchar_t* DL[] = { L"X", L"Y", L"Z" };
            int show = pi.numDims > 3 ? 3 : pi.numDims;
            for (int d = 0; d < show; d++) {
                RECT dr = DimButtonRect(d);
                bool active = (p.dimMask & (1 << d));
                g.FillRectangle(&panelBrush, (float)dr.left, (float)dr.top,
                                (float)(dr.right - dr.left), (float)(dr.bottom - dr.top));
                if (active) g.DrawRectangle(&accentPen, (float)dr.left, (float)dr.top,
                                            (float)(dr.right - dr.left - 1),
                                            (float)(dr.bottom - dr.top - 1));
                else g.DrawRectangle(&borderPen, (float)dr.left, (float)dr.top,
                                     (float)(dr.right - dr.left - 1),
                                     (float)(dr.bottom - dr.top - 1));
                DrawTextCentered(g, DL[d], uiFont, active ? accentBrush : dimBrush, dr);
            }
        }

        // keyframe interpolation type buttons
        {
            bool hasSel = (p.selKf >= 0 && p.selKf < (int)g_kfs.size());
            for (int i = 0; i < 6; i++) {
                RECT ir = InterpButtonRect(i);
                bool active = false;
                if (hasSel) {
                    KfInfo& kf = g_kfs[p.selKf];
                    switch (i) {
                        case 0: active = (kf.inInterp == AEGP_KeyInterp_LINEAR && kf.outInterp == AEGP_KeyInterp_LINEAR); break;
                        case 1: active = (kf.inInterp == AEGP_KeyInterp_BEZIER && kf.outInterp == AEGP_KeyInterp_BEZIER); break;
                        case 2: active = (kf.outInterp == AEGP_KeyInterp_HOLD); break;
                        case 3: active = (kf.easeIn[0].speedF == 0 && kf.easeOut[0].speedF == 0 &&
                                          kf.easeIn[0].influenceF > 0.3 && kf.easeOut[0].influenceF > 0.3); break;
                        default: break;
                    }
                }
                g.FillRectangle(&panelBrush, (float)ir.left, (float)ir.top,
                                (float)(ir.right - ir.left), (float)(ir.bottom - ir.top));
                if (active) g.DrawRectangle(&accentPen, (float)ir.left, (float)ir.top,
                                            (float)(ir.right - ir.left - 1),
                                            (float)(ir.bottom - ir.top - 1));
                else g.DrawRectangle(&borderPen, (float)ir.left, (float)ir.top,
                                     (float)(ir.right - ir.left - 1),
                                     (float)(ir.bottom - ir.top - 1));
                DrawInterpIcon(g, ir, i, hasSel ? (active ? accentBrush : textBrush) : dimBrush);
            }
        }

        // ---- graph (transparent; one row per selected axis) ----
        int dims[3];
        int rowCount = SelectedDims(dims);
        double visSpan = (double)(GRAPH_R - GRAPH_L) / g_pxPerSec;
        static const wchar_t* DL[] = { L"X", L"Y", L"Z" };

        if (p.graphMode == GRAPH_SPEED) {
            AEGP_SuiteHandler suites(g_sp);
            double h = visSpan / (CURVE_SAMPLES - 1);
            if (h <= 0.0) h = 1.0;
            for (int r = 0; r < rowCount; r++) {
                int d = dims[r];
                RECT row = RowRectForDim(d);
                std::vector<double> spd(CURVE_SAMPLES, 0.0);
                double maxS = 1e-9;
                for (int i = 0; i < CURVE_SAMPLES; i++) {
                    double t = g_viewT0 + visSpan * i / (CURVE_SAMPLES - 1);
                    double vp = SampleValue(suites, pi.streamH, pi.type, d, t - h * 0.5);
                    double vn = SampleValue(suites, pi.streamH, pi.type, d, t + h * 0.5);
                    spd[i] = fabs((vn - vp) / h);
                    if (spd[i] > maxS) maxS = spd[i];
                }
                for (auto& kf : g_kfs) {
                    double s = fabs(kf.easeIn[d].speedF);
                    if (s > maxS) maxS = s;
                    s = fabs(kf.easeOut[d].speedF);
                    if (s > maxS) maxS = s;
                }
                g_sMaxD[d] = maxS * 1.12;
                if (g_sMaxD[d] < 1e-6) g_sMaxD[d] = 1e-6;

                const int H_DIVS = 2;
                for (int i = 0; i <= H_DIVS; i++) {
                    float y = (float)row.top + (row.bottom - row.top) * i / (float)H_DIVS;
                    wchar_t buf[32];
                    _snwprintf_s(buf, 32, _TRUNCATE, L"%.3g",
                                 (double)(g_sMaxD[d] * (H_DIVS - i) / H_DIVS));
                    DrawTextRight(g, buf, smallFont, dimBrush, (float)(row.left - 4), y - 6.0f);
                }
                DrawText(g, DL[d], uiFont, accentBrush, (float)(row.left + 4), (float)(row.top + 2));

                std::vector<PointF> pts;
                pts.reserve(CURVE_SAMPLES);
                for (int i = 0; i < CURVE_SAMPLES; i++) {
                    double t = g_viewT0 + visSpan * i / (CURVE_SAMPLES - 1);
                    pts.push_back(PointF((float)TimeToX(t), (float)RowSpeedToY(row, d, spd[i])));
                }
                Pen speedPen(Color(255, 120, 220, 120), 2.0f);
                g.DrawLines(&speedPen, pts.data(), (INT)pts.size());

                for (int i = 0; i < (int)g_kfs.size(); i++) {
                    KfInfo& kf = g_kfs[i];
                    double kx = TimeToX(kf.time);
                    double inY = RowSpeedToY(row, d, fabs(kf.easeIn[d].speedF));
                    double outY = RowSpeedToY(row, d, fabs(kf.easeOut[d].speedF));
                    bool sel = (i == p.selKf);
                    g.FillEllipse(&handleBrush, (float)(kx - 12), (float)(inY - 3), 7.0f, 7.0f);
                    g.FillEllipse(&accentBrush, (float)(kx + 5), (float)(outY - 3), 7.0f, 7.0f);
                    if (sel) {
                        g.DrawEllipse(&ringPen, (float)(kx - 13), (float)(inY - 9), 18.0f, 18.0f);
                        g.DrawEllipse(&ringPen, (float)(kx - 7), (float)(outY - 9), 18.0f, 18.0f);
                    }
                }
            }
        } else {
            AEGP_SuiteHandler suites(g_sp);
            for (int r = 0; r < rowCount; r++) {
                int d = dims[r];
                RECT row = RowRectForDim(d);
                const int H_DIVS = 2;
                for (int i = 0; i <= H_DIVS; i++) {
                    float y = (float)row.top + (row.bottom - row.top) * i / (float)H_DIVS;
                    wchar_t buf[32];
                    _snwprintf_s(buf, 32, _TRUNCATE, L"%.3g",
                                 (double)(g_vMaxD[d] - (g_vMaxD[d] - g_vMinD[d]) * i / H_DIVS));
                    DrawTextRight(g, buf, smallFont, dimBrush, (float)(row.left - 4), y - 6.0f);
                }
                DrawText(g, DL[d], uiFont, accentBrush, (float)(row.left + 4), (float)(row.top + 2));

                if (!g_kfs.empty()) {
                    std::vector<PointF> pts;
                    pts.reserve(CURVE_SAMPLES);
                    for (int i = 0; i < CURVE_SAMPLES; i++) {
                        double t = g_viewT0 + visSpan * i / (CURVE_SAMPLES - 1);
                        double v = SampleValue(suites, pi.streamH, pi.type, d, t);
                        pts.push_back(PointF((float)TimeToX(t), (float)RowValueToY(row, d, v)));
                    }
                    g.DrawLines(&curvePen, pts.data(), (INT)pts.size());
                }

                for (int i = 0; i < (int)g_kfs.size(); i++) {
                    KfInfo& kf = g_kfs[i];
                    double kx = TimeToX(kf.time), ky = RowValueToY(row, d, kf.curValue[d]);
                    bool sel = (i == p.selKf);
                    if (sel) {
                        if (i > 0) {
                            double hx, hy;
                            GetInHandle(row, d, i, hx, hy);
                            g.DrawLine(&handlePen, (float)kx, (float)ky, (float)hx, (float)hy);
                            g.FillEllipse(&handleBrush, (float)(hx - 4), (float)(hy - 4), 9.0f, 9.0f);
                        }
                        if (i < (int)g_kfs.size() - 1) {
                            double hx, hy;
                            GetOutHandle(row, d, i, hx, hy);
                            g.DrawLine(&handlePen, (float)kx, (float)ky, (float)hx, (float)hy);
                            g.FillEllipse(&handleBrush, (float)(hx - 4), (float)(hy - 4), 9.0f, 9.0f);
                        }
                        g.FillEllipse(&selBrush, (float)(kx - 6), (float)(ky - 6), 12.0f, 12.0f);
                        g.DrawEllipse(&ringPen, (float)(kx - 9), (float)(ky - 9), 18.0f, 18.0f);
                    } else {
                        g.FillEllipse(&keyBrush, (float)(kx - 4), (float)(ky - 4), 9.0f, 9.0f);
                    }
                    // key time label for direct comparison with the timeline
                    wchar_t tb[24];
                    _snwprintf_s(tb, 24, _TRUNCATE, L"%.2f", (double)kf.time);
                    DrawText(g, tb, smallFont, dimBrush, (float)kx - 9, (float)(ky + 8));
                }
            }
        }

        // ---- time ruler labels (reference, matches the AE timeline scale) ----
        {
            double step = 1.0;
            double pxPerStep = g_pxPerSec * step;
            while (pxPerStep < 55.0) { step *= 2.0; pxPerStep = g_pxPerSec * step; }
            while (pxPerStep > 120.0) { step /= 2.0; pxPerStep = g_pxPerSec * step; }
            if (step < 0.001) step = 0.001;
            double t0 = floor(g_viewT0 / step) * step;
            for (double t = t0; t <= g_viewT0 + VisibleSpanSec() + step; t += step) {
                double x = TimeToX(t);
                if (x < GRAPH_L + 10 || x > GRAPH_R - 8) continue;
                wchar_t tb[32];
                _snwprintf_s(tb, 32, _TRUNCATE, step >= 1.0 ? L"%.0f" : L"%.1f", t);
                DrawText(g, tb, smallFont, dimBrush, (float)x - 6.0f, (float)(GRAPH_T + 2));
            }
        }

        // ---- dropdown (popup over graph) ----
        if (p.dropdownOpen && !g_props.empty()) {
            RECT dd = DropdownRect();
            g.FillRectangle(&dropBrush, (float)dd.left, (float)dd.top,
                            (float)(dd.right - dd.left), (float)(dd.bottom - dd.top));
            g.DrawRectangle(&borderPen, (float)dd.left, (float)dd.top,
                            (float)(dd.right - dd.left - 1), (float)(dd.bottom - dd.top - 1));
            int n = (int)g_props.size();
            int rows = ClampI(n - p.ddScroll, 0, DD_MAX_VISIBLE);
            for (int r = 0; r < rows; r++) {
                int idx = p.ddScroll + r;
                RECT row = { dd.left, dd.top + r * DD_ROW_H,
                             dd.right, dd.top + (r + 1) * DD_ROW_H };
                if (idx == p.ddHover)
                    g.FillRectangle(&hoverBrush, (float)row.left, (float)row.top,
                                    (float)(row.right - row.left),
                                    (float)(row.bottom - row.top));
                std::wstring txt = g_props[idx].name;
                if (txt.size() > 48) txt = txt.substr(0, 47) + L"…";
                DrawText(g, txt, smallFont, idx == p.curProp ? accentBrush : textBrush,
                         (float)(row.left + 6), (float)(row.top + 4));
            }
        }

        // ---- bottom hints ----
        if (p.graphMode == GRAPH_SPEED) {
            DrawText(g, L"速度曲线 · 拖点改速度 · 滑条/滚轮平移",
                     smallFont, dimBrush, 16.0f, (float)(BOTTOM_T + 4));
        } else {
            DrawText(g, L"拖关键帧=值/时间 · 拖手柄=影响/速度 · 滑条/滚轮平移",
                     smallFont, dimBrush, 16.0f, (float)(BOTTOM_T + 4));
        }
        // manual scale fine-tune buttons
        {
            RECT mr = ScaleMinusRect(), pr = ScalePlusRect();
            g.FillRectangle(&panelBrush, (float)mr.left, (float)mr.top,
                            (float)(mr.right - mr.left), (float)(mr.bottom - mr.top));
            g.DrawRectangle(&borderPen, (float)mr.left, (float)mr.top,
                            (float)(mr.right - mr.left - 1), (float)(mr.bottom - mr.top - 1));
            g.FillRectangle(&panelBrush, (float)pr.left, (float)pr.top,
                            (float)(pr.right - pr.left), (float)(pr.bottom - pr.top));
            g.DrawRectangle(&borderPen, (float)pr.left, (float)pr.top,
                            (float)(pr.right - pr.left - 1), (float)(pr.bottom - pr.top - 1));
            DrawTextCentered(g, L"−", uiFont, textBrush, mr);
            DrawTextCentered(g, L"+", uiFont, textBrush, pr);
        }
        wchar_t zbuf[48];
        if (g_tl_hwnd) {
            _snwprintf_s(zbuf, 48, _TRUNCATE, L"比例 %.0fpx/s·时间线", (double)g_pxPerSec);
        } else {
            _snwprintf_s(zbuf, 48, _TRUNCATE, L"比例 %.0fpx/s·自适应", (double)g_pxPerSec);
        }
        DrawTextRight(g, zbuf, smallFont, accentBrush, (float)(CP_W - 64),
                      (float)(BOTTOM_T + 4));
        if (p.selKf >= 0 && p.selKf < (int)g_kfs.size()) {
            KfInfo& kf = g_kfs[p.selKf];
            wchar_t buf[256];
            if (p.graphMode == GRAPH_SPEED) {
                _snwprintf_s(buf, 256, _TRUNCATE,
                             L"关键帧 %d/%d  入速 %.3g  出速 %.3g  影响 %.0f%%/%.0f%%",
                             p.selKf + 1, (int)g_kfs.size(),
                             (double)kf.easeIn[dims[0]].speedF, (double)kf.easeOut[dims[0]].speedF,
                             kf.easeIn[dims[0]].influenceF * 100.0,
                             kf.easeOut[dims[0]].influenceF * 100.0);
            } else {
                wchar_t vtxt[128] = L"";
                for (int r = 0; r < rowCount && r < 3; r++) {
                    wchar_t one[48];
                    _snwprintf_s(one, 48, _TRUNCATE, L"%s=%.3g ",
                                 DL[dims[r]], (double)kf.curValue[dims[r]]);
                    wcscat_s(vtxt, 128, one);
                }
                _snwprintf_s(buf, 256, _TRUNCATE,
                             L"关键帧 %d/%d  t=%.2fs  %s 入 %.0f%%/%.2f  出 %.0f%%/%.2f",
                             p.selKf + 1, (int)g_kfs.size(), kf.time, vtxt,
                             kf.easeIn[dims[0]].influenceF * 100.0,
                             (double)kf.easeIn[dims[0]].speedF,
                             kf.easeOut[dims[0]].influenceF * 100.0,
                             (double)kf.easeOut[dims[0]].speedF);
            }
            DrawText(g, buf, smallFont, accentBrush, 16.0f, (float)(BOTTOM_T + 22));
        }

        // ---- scrollbar ----
        {
            RECT tr = ScrollRect();
            g.FillRectangle(&panelBrush, (float)tr.left, (float)tr.top,
                            (float)(tr.right - tr.left), (float)(tr.bottom - tr.top));
            g.DrawRectangle(&borderPen, (float)tr.left, (float)tr.top,
                            (float)(tr.right - tr.left - 1), (float)(tr.bottom - tr.top - 1));
            double tl, tw;
            ScrollThumb(tl, tw);
            g.FillRectangle(&accentBrush, (float)tl, (float)tr.top + 2,
                            (float)tw, (float)(tr.bottom - tr.top - 4));
        }
    }

    ApplyPanelShadow((unsigned char*)bits, CP_W, CP_H, 3, 5, 7, 150);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { 0, 0 };
    RECT wr = {};
    GetWindowRect(p.hwnd, &wr);
    ptDst.x = wr.left;
    ptDst.y = wr.top;
    SIZE sz = { CP_W, CP_H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(p.hwnd, hdcScreen, &ptDst, &sz, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(hdcMem, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

// ---------------- Interaction ----------------
static void ApplyInterpType(int type) {
    PanelState& p = g_panel;
    if (p.selKf < 0 || p.selKf >= (int)g_kfs.size()) return;
    KfInfo& kf = g_kfs[p.selKf];
    PropertyInfo& pi = g_props[p.curProp];
    int nd = pi.numDims > 3 ? 3 : pi.numDims;
    int te = pi.temporalDims > 0 ? pi.temporalDims : nd;
    if (te > nd) te = nd;
    switch (type) {
        case 0:  // linear
            kf.inInterp = AEGP_KeyInterp_LINEAR;
            kf.outInterp = AEGP_KeyInterp_LINEAR;
            break;
        case 1: { // bezier (keep the current line shape when converting)
            bool wasBezier = (kf.inInterp == AEGP_KeyInterp_BEZIER &&
                              kf.outInterp == AEGP_KeyInterp_BEZIER);
            kf.inInterp = AEGP_KeyInterp_BEZIER;
            kf.outInterp = AEGP_KeyInterp_BEZIER;
            if (!wasBezier) {
                for (int d = 0; d < te; d++) {
                    double slopeIn = 0.0, slopeOut = 0.0;
                    if (p.selKf > 0)
                        slopeIn = (kf.curValue[d] - g_kfs[p.selKf - 1].curValue[d]) /
                                  (kf.time - g_kfs[p.selKf - 1].time);
                    if (p.selKf < (int)g_kfs.size() - 1)
                        slopeOut = (g_kfs[p.selKf + 1].curValue[d] - kf.curValue[d]) /
                                   (g_kfs[p.selKf + 1].time - kf.time);
                    kf.easeIn[d].speedF = (A_FpLong)slopeIn;
                    kf.easeIn[d].influenceF = 0.333f;
                    kf.easeOut[d].speedF = (A_FpLong)slopeOut;
                    kf.easeOut[d].influenceF = 0.333f;
                }
            }
            break;
        }
        case 2:  // hold (no speed)
            kf.outInterp = AEGP_KeyInterp_HOLD;
            break;
        case 3:  // easy ease
            kf.inInterp = AEGP_KeyInterp_BEZIER;
            kf.outInterp = AEGP_KeyInterp_BEZIER;
            for (int d = 0; d < te; d++) {
                kf.easeIn[d].speedF = 0;
                kf.easeIn[d].influenceF = 1.0f / 3.0f;
                kf.easeOut[d].speedF = 0;
                kf.easeOut[d].influenceF = 1.0f / 3.0f;
            }
            break;
        case 4:  // easy ease in
            kf.inInterp = AEGP_KeyInterp_BEZIER;
            for (int d = 0; d < te; d++) {
                kf.easeIn[d].speedF = 0;
                kf.easeIn[d].influenceF = 1.0f / 3.0f;
            }
            break;
        case 5:  // easy ease out
            kf.outInterp = AEGP_KeyInterp_BEZIER;
            for (int d = 0; d < te; d++) {
                kf.easeOut[d].speedF = 0;
                kf.easeOut[d].influenceF = 1.0f / 3.0f;
            }
            break;
    }
    AEGP_SuiteHandler suites(g_sp);
    suites.KeyframeSuite5()->AEGP_SetKeyframeInterpolation(
        pi.streamH, p.selKf, kf.inInterp, kf.outInterp);
    for (int d = 0; d < te; d++) {
        AEGP_KeyframeEase in = kf.easeIn[d], out = kf.easeOut[d];
        suites.KeyframeSuite5()->AEGP_SetKeyframeTemporalEase(
            pi.streamH, p.selKf, d, &in, &out);
    }
    RenderCurvePanel();
}

static void StartDragHandle(int mode, int idx, int x, int y) {
    PanelState& p = g_panel;
    KfInfo& kf = g_kfs[idx];
    int d = p.dragDim;
    RECT row = RowRectForDim(d);
    p.dragging = true;
    p.dragMode = mode;
    p.selKf = idx;
    p.dragStartX = x;
    p.dragStartY = y;
    if (mode == DRAG_IN) {
        p.dragStartInfluence = kf.easeIn[d].influenceF;
        p.dragStartSpeed = kf.easeIn[d].speedF;
        p.segWpx = (idx > 0) ? (TimeToX(kf.time) - TimeToX(g_kfs[idx - 1].time)) : 1.0;
        if (kf.inInterp != AEGP_KeyInterp_BEZIER) {
            kf.inInterp = AEGP_KeyInterp_BEZIER;
        }
    } else {
        p.dragStartInfluence = kf.easeOut[d].influenceF;
        p.dragStartSpeed = kf.easeOut[d].speedF;
        p.segWpx = (idx < (int)g_kfs.size() - 1)
                       ? (TimeToX(g_kfs[idx + 1].time) - TimeToX(kf.time))
                       : 1.0;
        if (kf.outInterp != AEGP_KeyInterp_BEZIER) {
            kf.outInterp = AEGP_KeyInterp_BEZIER;
        }
    }
    if (p.segWpx < 10.0) p.segWpx = 10.0;
    p.pxPerSpeed = PxPerSpeedRow(row, d);
    if (p.pxPerSpeed < 0.001) p.pxPerSpeed = 0.001;
}

static void StartSpeedDrag(int mode, int idx, int x, int y) {
    PanelState& p = g_panel;
    KfInfo& kf = g_kfs[idx];
    int d = p.dragDim;
    RECT row = RowRectForDim(d);
    p.dragging = true;
    p.dragMode = mode;
    p.selKf = idx;
    p.dragStartX = x;
    p.dragStartY = y;
    if (mode == DRAG_SPEED_IN) {
        p.dragStartInfluence = kf.easeIn[d].influenceF;
        p.dragStartSpeed = kf.easeIn[d].speedF;
        if (kf.inInterp != AEGP_KeyInterp_BEZIER) kf.inInterp = AEGP_KeyInterp_BEZIER;
    } else {
        p.dragStartInfluence = kf.easeOut[d].influenceF;
        p.dragStartSpeed = kf.easeOut[d].speedF;
        if (kf.outInterp != AEGP_KeyInterp_BEZIER) kf.outInterp = AEGP_KeyInterp_BEZIER;
    }
    p.segWpx = 160.0;
    p.pxPerSpeed = (double)(row.bottom - row.top) / g_sMaxD[d];
    if (p.pxPerSpeed < 0.001) p.pxPerSpeed = 0.001;
}

static void UpdateDrag(int x, int y) {
    PanelState& p = g_panel;
    int d = p.dragDim;
    if (p.dragMode == DRAG_SCROLL) {
        RECT tr = ScrollRect();
        double trackW = (double)(tr.right - tr.left);
        double tl, tw;
        ScrollThumb(tl, tw);
        double thumbLeft = (double)(x - tr.left) - p.segWpx;
        double f = (trackW - tw > 1.0) ? thumbLeft / (trackW - tw) : 0.0;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        double lo = ScrollRangeMin();
        double hi = ScrollRangeMax() - VisibleSpanSec();
        g_viewT0 = lo + f * (hi - lo);
        RenderCurvePanel();
    } else if (p.dragMode == DRAG_PAN) {
        double dx = (double)(x - p.dragStartX);
        g_viewT0 -= dx / g_pxPerSec;
        p.dragStartX = x;
        RenderCurvePanel();
    } else if (p.dragMode == DRAG_KEY) {
        int dx = x - p.dragStartX, dy = y - p.dragStartY;
        if (p.dragAxis == 0 && (abs(dx) + abs(dy) > 3)) {
            p.dragAxis = (abs(dx) > abs(dy)) ? 2 : 1;
        }
        if (p.dragAxis == 2) {
            // move the keyframe in time
            double tNew = p.dragStartTime + (double)dx / g_pxPerSec;
            int idx = p.selKf;
            if (idx > 0) {
                double lo = g_kfs[idx - 1].time + 0.001;
                if (tNew < lo) tNew = lo;
            }
            if (idx < (int)g_kfs.size() - 1) {
                double hi = g_kfs[idx + 1].time - 0.001;
                if (tNew > hi) tNew = hi;
            }
            g_kfs[idx].time = tNew;
            RenderCurvePanel();
        } else {
            double dv = (double)(y - p.dragStartY) / p.pxPerValue;
            SetKeyValue(p.selKf, d, p.dragStartVal - dv);
        }
    } else if (p.dragMode == DRAG_IN || p.dragMode == DRAG_OUT) {
        KfInfo& kf = g_kfs[p.selKf];
        double dx = (double)(x - p.dragStartX);
        if (p.dragMode == DRAG_IN) {
            // incoming handle: drag away from the keyframe (left) = more influence
            double inf = ClampD(p.dragStartInfluence + (p.dragStartX - x) / p.segWpx, 0.0, 1.0);
            double speed = p.dragStartSpeed + (double)(y - p.dragStartY) / p.pxPerSpeed;
            kf.easeIn[d].influenceF = (A_FpLong)inf;
            kf.easeIn[d].speedF = (A_FpLong)speed;
        } else {
            double inf = ClampD(p.dragStartInfluence + dx / p.segWpx, 0.0, 1.0);
            double speed = p.dragStartSpeed + (double)(p.dragStartY - y) / p.pxPerSpeed;
            kf.easeOut[d].influenceF = (A_FpLong)inf;
            kf.easeOut[d].speedF = (A_FpLong)speed;
        }
        ApplyEase(p.selKf, d);
    } else if (p.dragMode == DRAG_SPEED_IN || p.dragMode == DRAG_SPEED_OUT) {
        KfInfo& kf = g_kfs[p.selKf];
        double dx = (double)(x - p.dragStartX);
        double inf = ClampD(p.dragStartInfluence + dx / p.segWpx, 0.0, 1.0);
        double speed = p.dragStartSpeed + (double)(p.dragStartY - y) / p.pxPerSpeed;
        if (p.dragMode == DRAG_SPEED_IN) {
            kf.easeIn[d].influenceF = (A_FpLong)inf;
            kf.easeIn[d].speedF = (A_FpLong)speed;
        } else {
            kf.easeOut[d].influenceF = (A_FpLong)inf;
            kf.easeOut[d].speedF = (A_FpLong)speed;
        }
        ApplyEase(p.selKf, d);
    }
}

static int RowDimAtY(int y) {
    int dims[3];
    int n = SelectedDims(dims);
    int h = GRAPH_B - GRAPH_T;
    for (int r = 0; r < n; r++) {
        int top = GRAPH_T + MulDiv(r, h, n);
        int bot = GRAPH_T + MulDiv(r + 1, h, n);
        if (y >= top && y < bot) return dims[r];
    }
    return dims[0];
}

static void HitTestGraph(int x, int y) {
    if (g_kfs.empty()) return;
    PanelState& p = g_panel;
    p.dragDim = RowDimAtY(y);
    int d = p.dragDim;
    RECT row = RowRectForDim(d);
    if (p.graphMode == GRAPH_SPEED) {
        for (int i = 0; i < (int)g_kfs.size(); i++) {
            double kx = TimeToX(g_kfs[i].time);
            double inY = RowSpeedToY(row, d, fabs(g_kfs[i].easeIn[d].speedF));
            double outY = RowSpeedToY(row, d, fabs(g_kfs[i].easeOut[d].speedF));
            if (Dist(x, y, kx - 8, inY) <= 12.0) {
                StartSpeedDrag(DRAG_SPEED_IN, i, x, y);
                RenderCurvePanel();
                return;
            }
            if (Dist(x, y, kx + 8, outY) <= 12.0) {
                StartSpeedDrag(DRAG_SPEED_OUT, i, x, y);
                RenderCurvePanel();
                return;
            }
        }
        int best = -1;
        double bestD = 13.0;
        for (int i = 0; i < (int)g_kfs.size(); i++) {
            double dist = Dist(x, y, TimeToX(g_kfs[i].time),
                               RowSpeedToY(row, d, fabs(g_kfs[i].easeOut[d].speedF)));
            if (dist < bestD) { bestD = dist; best = i; }
        }
        if (best >= 0) p.selKf = best;
        RECT garea = { GRAPH_L, GRAPH_T, GRAPH_R, GRAPH_B };
        if (best < 0 && PointInRect(garea, x, y)) {
            p.dragging = true;
            p.dragMode = DRAG_PAN;
            p.dragStartX = x;
            p.dragStartY = y;
        }
        RenderCurvePanel();
        return;
    }
    int sel = p.selKf;
    if (sel >= 0 && sel < (int)g_kfs.size()) {
        if (sel > 0) {
            double hx, hy;
            GetInHandle(row, d, sel, hx, hy);
            if (Dist(x, y, hx, hy) <= 11.0) {
                StartDragHandle(DRAG_IN, sel, x, y);
                RenderCurvePanel();
                return;
            }
        }
        if (sel < (int)g_kfs.size() - 1) {
            double hx, hy;
            GetOutHandle(row, d, sel, hx, hy);
            if (Dist(x, y, hx, hy) <= 11.0) {
                StartDragHandle(DRAG_OUT, sel, x, y);
                RenderCurvePanel();
                return;
            }
        }
    }
    int best = -1;
    double bestD = 13.0;
    for (int i = 0; i < (int)g_kfs.size(); i++) {
        double dist = Dist(x, y, TimeToX(g_kfs[i].time),
                           RowValueToY(row, d, g_kfs[i].curValue[d]));
        if (dist < bestD) { bestD = dist; best = i; }
    }
    if (best >= 0) {
        p.selKf = best;
        p.dragging = true;
        p.dragMode = DRAG_KEY;
        p.dragStartX = x;
        p.dragStartY = y;
        p.dragStartVal = g_kfs[best].curValue[d];
        p.dragStartTime = g_kfs[best].time;
        p.dragAxis = 0;
        double vmin = g_vMinD[d], vmax = g_vMaxD[d];
        if (vmax - vmin < 1e-9) vmax = vmin + 1.0;
        p.pxPerValue = (double)(row.bottom - row.top) / (vmax - vmin);
        if (p.pxPerValue < 0.001) p.pxPerValue = 0.001;
    } else {
        RECT garea = { GRAPH_L, GRAPH_T, GRAPH_R, GRAPH_B };
        if (PointInRect(garea, x, y)) {
            p.dragging = true;
            p.dragMode = DRAG_PAN;
            p.dragStartX = x;
            p.dragStartY = y;
        }
    }
    RenderCurvePanel();
}

static void SelectProperty(int idx) {
    g_panel.curProp = idx;
    g_panel.dimMask = 3;
    g_panel.dropdownOpen = false;
    g_panel.ddScroll = 0;
    ReloadKeyframes();
}

static void ClosePanel() {
    PanelState& p = g_panel;
    if (GetCapture() == p.hwnd) ReleaseCapture();
    if (p.hwnd) ShowWindow(p.hwnd, SW_HIDE);
    p.finished = true;
}

// ---------------- Shortcut toggle ----------------
struct ShortcutCombo {
    int vk = 0;
    bool ctrl = false, shift = false, alt = false;
};
static std::vector<ShortcutCombo> g_shortcuts;

static int ParseKeyName(const std::wstring& s) {
    if (s.size() == 1) {
        wchar_t c = s[0];
        if (c >= L'a' && c <= L'z') return c - L'a' + 'A';
        if (c >= L'A' && c <= L'Z') return c;
        if (c >= L'0' && c <= L'9') return c;
        return 0;
    }
    struct { const wchar_t* n; int vk; } names[] = {
        { L"Enter", VK_RETURN }, { L"Return", VK_RETURN }, { L"Esc", VK_ESCAPE },
        { L"Space", VK_SPACE }, { L"Tab", VK_TAB }, { L"Delete", VK_DELETE },
        { L"Backspace", VK_BACK }, { L"Insert", VK_INSERT },
        { L"LeftArrow", VK_LEFT }, { L"RightArrow", VK_RIGHT },
        { L"UpArrow", VK_UP }, { L"DownArrow", VK_DOWN },
        { L"Home", VK_HOME }, { L"End", VK_END },
        { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT },
    };
    for (auto& e : names) if (s == e.n) return e.vk;
    if (s.size() >= 2 && (s[0] == L'F' || s[0] == L'f')) {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return 0;
}

// Read the shortcut AE has assigned to this command from its shortcut file, so
// the panel can close itself when the same key is pressed while it is open.
static void LoadPanelShortcuts() {
    g_shortcuts.clear();
    wchar_t appdata[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH * 2) == 0) return;
    std::wstring file = std::wstring(appdata) + L"\\Adobe\\After Effects\\24.6\\aeks\\Custom.txt";
    FILE* f = NULL;
    _wfopen_s(&f, file.c_str(), L"r, ccs=UTF-8");
    if (!f) return;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring s = line;
        if (s.find(L"DalimaoCurves") == std::wstring::npos) continue;
        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) break;
        size_t q1 = s.find(L'"', eq);
        if (q1 == std::wstring::npos) break;
        size_t q2 = s.find(L'"', q1 + 1);
        if (q2 == std::wstring::npos) break;
        std::wstring val = s.substr(q1 + 1, q2 - q1 - 1);
        size_t pos = 0;
        while ((pos = val.find(L'(', pos)) != std::wstring::npos) {
            size_t end = val.find(L')', pos);
            if (end == std::wstring::npos) break;
            std::wstring combo = val.substr(pos + 1, end - pos - 1);
            pos = end + 1;
            if (combo.empty()) continue;
            ShortcutCombo sc;
            size_t st = 0;
            while ((st = combo.find_first_not_of(L' ', st)) != std::wstring::npos) {
                size_t plus = combo.find(L'+', st);
                std::wstring part = (plus == std::wstring::npos)
                                        ? combo.substr(st)
                                        : combo.substr(st, plus - st);
                if (part == L"Ctrl" || part == L"Control") sc.ctrl = true;
                else if (part == L"Shift") sc.shift = true;
                else if (part == L"Alt" || part == L"Option") sc.alt = true;
                else if (part == L"Cmd" || part == L"Command") { /* mac only */ }
                else sc.vk = ParseKeyName(part);
                if (plus == std::wstring::npos) break;
                st = plus + 1;
            }
            if (sc.vk != 0) g_shortcuts.push_back(sc);
        }
        break;
    }
    fclose(f);
}

static bool MatchPanelShortcut(int vk, bool ctrl, bool shift, bool alt) {
    for (auto& sc : g_shortcuts) {
        if (sc.vk == vk && sc.ctrl == ctrl && sc.shift == shift && sc.alt == alt)
            return true;
    }
    return false;
}

// ---------------- Window proc ----------------
static void SessionCleanup();

static LRESULT CALLBACK CurvesWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PanelState& p = g_panel;
    try {
        switch (msg) {
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (p.toast) {
            ClosePanel();
            return 0;
        }
        // scrollbar
        RECT srect = ScrollRect();
        if (PointInRect(srect, x, y)) {
            double tl, tw;
            ScrollThumb(tl, tw);
            p.dragging = true;
            p.dragMode = DRAG_SCROLL;
            p.dragStartX = x;
            p.dragStartY = y;
            if (x >= tl && x <= tl + tw) {
                p.segWpx = (double)(x - srect.left) - tl;   // grab offset within the thumb
            } else {
                // clicked on the track: center the thumb at the click
                p.segWpx = tw * 0.5;
                double trackW = (double)(srect.right - srect.left);
                double f = (trackW - tw > 1.0)
                               ? ((double)(x - srect.left) - tw * 0.5) / (trackW - tw)
                               : 0.0;
                if (f < 0.0) f = 0.0;
                if (f > 1.0) f = 1.0;
                double lo = ScrollRangeMin();
                double hi = ScrollRangeMax() - VisibleSpanSec();
                g_viewT0 = lo + f * (hi - lo);
            }
            RenderCurvePanel();
            return 0;
        }
        // manual scale fine-tune
        if (PointInRect(ScaleMinusRect(), x, y)) {
            g_pxPerSec *= 0.98;
            if (g_pxPerSec < 1.0) g_pxPerSec = 1.0;
            if (g_auto_px > 0.0) {
                g_manual_ratio = g_pxPerSec / g_auto_px;
                SavePanelSettings();
            }
            RenderCurvePanel();
            return 0;
        }
        if (PointInRect(ScalePlusRect(), x, y)) {
            g_pxPerSec *= 1.02;
            if (g_pxPerSec > 4000.0) g_pxPerSec = 4000.0;
            if (g_auto_px > 0.0) {
                g_manual_ratio = g_pxPerSec / g_auto_px;
                SavePanelSettings();
            }
            RenderCurvePanel();
            return 0;
        }
        if (p.dropdownOpen) {
            RECT dd = DropdownRect();
            if (PointInRect(dd, x, y)) {
                int row = (y - dd.top) / DD_ROW_H + p.ddScroll;
                if (row >= 0 && row < (int)g_props.size()) SelectProperty(row);
                RenderCurvePanel();
            } else {
                p.dropdownOpen = false;
                RenderCurvePanel();
            }
            return 0;
        }
        PropertyInfo* pi = (p.curProp >= 0 && p.curProp < (int)g_props.size())
                               ? &g_props[p.curProp] : nullptr;
        // graph mode toggle
        for (int m = 0; m < 2; m++) {
            if (PointInRect(ModeButtonRect(m), x, y)) {
                p.graphMode = m;
                RenderCurvePanel();
                return 0;
            }
        }
        // keyframe interpolation type buttons
        if (p.selKf >= 0 && p.selKf < (int)g_kfs.size()) {
            for (int i = 0; i < 6; i++) {
                if (PointInRect(InterpButtonRect(i), x, y)) {
                    ApplyInterpType(i);
                    return 0;
                }
            }
        }
        if (pi && pi->numDims > 1) {
            int show = pi->numDims > 3 ? 3 : pi->numDims;
            for (int d = 0; d < show; d++) {
                if (PointInRect(DimButtonRect(d), x, y)) {
                    if (p.dimMask & (1 << d)) {
                        // allow turning off only if at least one axis stays selected
                        if (p.dimMask != (1 << d)) p.dimMask &= ~(1 << d);
                    } else {
                        p.dimMask |= (1 << d);
                    }
                    RenderCurvePanel();
                    return 0;
                }
            }
        }
        if (PointInRect(PropButtonRect(), x, y)) {
            p.dropdownOpen = !p.dropdownOpen;
            p.ddHover = -1;
            RenderCurvePanel();
            return 0;
        }
        HitTestGraph(x, y);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        p.mx = x;
        p.my = y;
        if (p.moving) {
            POINT cur;
            GetCursorPos(&cur);
            SetWindowPos(p.hwnd, NULL,
                         p.winStartX + (cur.x - p.moveStartX),
                         p.winStartY + (cur.y - p.moveStartY),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (p.dropdownOpen) {
            RECT dd = DropdownRect();
            p.ddHover = PointInRect(dd, x, y)
                            ? ((y - dd.top) / DD_ROW_H + p.ddScroll)
                            : -1;
        }
        if (p.dragging) {
            UpdateDrag(x, y);
            RenderCurvePanel();
        } else {
            RECT zone = { -LEAVE_MARGIN, -LEAVE_MARGIN,
                          CP_W + LEAVE_MARGIN, CP_H + LEAVE_MARGIN };
            if (!PointInRect(zone, x, y)) {
                ClosePanel();
                return 0;
            }
            if (p.dropdownOpen) RenderCurvePanel();
        }
        return 0;
    }
    case WM_MBUTTONDOWN: {
        SetCapture(hwnd);
        p.moving = true;
        POINT cur;
        GetCursorPos(&cur);
        p.moveStartX = cur.x;
        p.moveStartY = cur.y;
        RECT wr;
        GetWindowRect(p.hwnd, &wr);
        p.winStartX = wr.left;
        p.winStartY = wr.top;
        return 0;
    }
    case WM_MBUTTONUP:
        p.moving = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_LBUTTONUP: {
        if (p.dragging) {
            if (p.dragMode == DRAG_KEY && p.dragAxis == 2 &&
                p.selKf >= 0 && p.selKf < (int)g_kfs.size()) {
                ApplyKeyTimeMove(p.selKf);
            }
            p.dragging = false;
            p.dragAxis = 0;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            RECT zone = { -LEAVE_MARGIN * 2, -LEAVE_MARGIN * 2,
                          CP_W + LEAVE_MARGIN * 2, CP_H + LEAVE_MARGIN * 2 };
            if (!PointInRect(zone, x, y)) ClosePanel();
            else RenderCurvePanel();
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONDOWN:
        ClosePanel();
        return 0;
    case WM_TIMER:
        if (p.hwnd && !p.finished && !p.dragging && !p.moving) {
            POINT cur;
            GetCursorPos(&cur);
            RECT wr;
            GetWindowRect(p.hwnd, &wr);
            RECT zone = { wr.left - LEAVE_MARGIN, wr.top - LEAVE_MARGIN,
                          wr.right + LEAVE_MARGIN, wr.bottom + LEAVE_MARGIN };
            if (!PointInRect(zone, cur.x, cur.y)) {
                ClosePanel();
            }
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (p.dropdownOpen && !g_props.empty()) {
            int maxScroll = (int)g_props.size() - DD_MAX_VISIBLE;
            if (maxScroll < 0) maxScroll = 0;
            p.ddScroll = ClampI(p.ddScroll + (delta > 0 ? -1 : 1), 0, maxScroll);
            RenderCurvePanel();
        } else {
            double visible = (double)(GRAPH_R - GRAPH_L) / g_pxPerSec;
            g_viewT0 += (double)delta / 120.0 * visible * 0.12;
            RenderCurvePanel();
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (MatchPanelShortcut((int)wp, ctrl, shift, alt)) {
            ClosePanel();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            ClosePanel();
            return 0;
        }
        if (p.dropdownOpen && !g_props.empty()) {
            int n = (int)g_props.size();
            if (wp == VK_UP || wp == VK_DOWN) {
                int cur = (p.ddHover >= 0 && p.ddHover < n) ? p.ddHover : p.curProp;
                cur = (cur + (wp == VK_UP ? n - 1 : 1)) % n;
                p.ddHover = cur;
                if (cur < p.ddScroll) p.ddScroll = cur;
                if (cur >= p.ddScroll + DD_MAX_VISIBLE)
                    p.ddScroll = cur - DD_MAX_VISIBLE + 1;
                RenderCurvePanel();
            } else if (wp == VK_RETURN && p.ddHover >= 0 && p.ddHover < n) {
                SelectProperty(p.ddHover);
                RenderCurvePanel();
            }
        }
        return 0;
    }
        case WM_DESTROY:
            if (GetCapture() == hwnd) ReleaseCapture();
            KillTimer(hwnd, 1);
            SessionCleanup();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    } catch (...) {
        CurvesDebugLog(L"exception in CurvesWndProc");
        ClosePanel();
        return 0;
    }
}

// ---------------- Panel lifecycle ----------------
static void SessionCleanup() {
    PanelState& p = g_panel;
    Gdiplus::GdiplusShutdown(p.gdipToken);
    try {
        AEGP_SuiteHandler suites(g_sp);
        DisposeKeyframes(suites);
        DisposeProperties(suites);
    } catch (...) {
        CurvesDebugLog(L"cleanup suites failed");
    }
    g_kfs.clear();
    s_panel_active = false;
    p = PanelState();
}

// Modal panel loop running inside the command hook, so AEGP suite calls are
// made inside a valid AEGP callback context.
static void CreatePanel(bool toast, const std::wstring& msg) {
    PanelState& p = g_panel;
    p.toast = toast;
    p.toastMsg = msg;

    try {
        Gdiplus::GdiplusStartupInput gdiplusInput;
        GdiplusStartup(&p.gdipToken, &gdiplusInput, NULL);

        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = CurvesWndProc;
            wc.hInstance = GetModuleHandle(NULL);
            wc.lpszClassName = L"DalimaoCurvesPanel";
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            RegisterClassExW(&wc);
            registered = true;
        }

        POINT cursor;
        GetCursorPos(&cursor);
        int wx = cursor.x - CP_W / 2, wy = cursor.y - CP_H / 2;
        int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
        wx = ClampI(wx, 0, scrW - CP_W);
        wy = ClampI(wy, 0, scrH - CP_H);

        p.hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"DalimaoCurvesPanel", L"", WS_POPUP,
            wx, wy, CP_W, CP_H,
            NULL, NULL, GetModuleHandle(NULL), NULL);
        if (!p.hwnd) {
            CurvesDebugLog(L"panel window creation failed");
            SessionCleanup();
            return;
        }

        RenderCurvePanel();
        ShowWindow(p.hwnd, SW_SHOW);
        SetTimer(p.hwnd, 1, 60, NULL);
        SetForegroundWindow(p.hwnd);
        SetFocus(p.hwnd);
        SetCapture(p.hwnd);

        MSG msgloop;
        while (!p.finished && GetMessage(&msgloop, NULL, 0, 0)) {
            if (msgloop.message == WM_QUIT) break;
            TranslateMessage(&msgloop);
            DispatchMessage(&msgloop);
            if (g_ae_main && !IsWindow(g_ae_main)) {
                p.finished = true;
                break;
            }
        }

        if (p.hwnd) DestroyWindow(p.hwnd);   // WM_DESTROY runs SessionCleanup
    } catch (...) {
        CurvesDebugLog(L"exception in CreatePanel");
        if (p.hwnd) {
            DestroyWindow(p.hwnd);   // WM_DESTROY runs SessionCleanup
        } else {
            SessionCleanup();
        }
    }
}

// Capture a strip of a window and look for the AE playhead: a thin, solid
// blue vertical line. Returns the column (relative to the window left) and run.
static bool DetectPlayhead(const RECT& wr, int stripTop, int stripH,
                           int w, int& outX, int& outRun) {
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -stripH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(mem, hbm);
    BitBlt(mem, 0, 0, w, stripH, scr, wr.left, stripTop, SRCCOPY | CAPTUREBLT);

    auto isBlue = [](unsigned char* px) {
        unsigned char B = px[0], G = px[1], R = px[2];
        return B >= 180 && R <= 110 && B >= R + 90 && B >= G + 30;
    };
    int bestX = -1, bestRun = 0;
    for (int x = 0; x < w; x++) {
        int run = 0, maxRun = 0, gap = 0;
        for (int y = 0; y < stripH; y++) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            if (isBlue(px)) {
                run += gap + 1;
                gap = 0;
                if (run > maxRun) maxRun = run;
            } else if (run > 0 && gap < 2) {
                gap++;
            } else {
                run = 0;
            }
        }
        if (maxRun > bestRun) { bestRun = maxRun; bestX = x; }
    }
    if (bestX <= 2 || bestRun < stripH / 3) {
        SelectObject(mem, old);
        DeleteObject(hbm);
        DeleteDC(mem);
        ReleaseDC(NULL, scr);
        return false;
    }
    // reject wide blue regions: the playhead is a thin line (<= 4 columns)
    int thick = 0;
    for (int x = bestX - 6; x <= bestX + 6; x++) {
        if (x < 0 || x >= w) continue;
        int run = 0, maxRun = 0, gap = 0;
        for (int y = 0; y < stripH; y++) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            if (isBlue(px)) {
                run += gap + 1;
                gap = 0;
                if (run > maxRun) maxRun = run;
            } else if (run > 0 && gap < 2) {
                gap++;
            } else {
                run = 0;
            }
        }
        if (maxRun >= stripH / 3) thick++;
    }
    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    if (thick > 4) return false;
    outX = bestX;
    outRun = bestRun;
    return true;
}

// Detect the yellow keyframe diamonds in a captured timeline window and return
// their x centers grouped by row.
static bool DetectKeyDiamonds(const RECT& wr, int w, int h,
                              std::vector<std::vector<int>>& rowsOut) {
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(mem, hbm);
    BitBlt(mem, 0, 0, w, h, scr, wr.left, wr.top, SRCCOPY | CAPTUREBLT);

    // collect bright small clusters below the ruler, skipping the layer-name
    // column on the left (diamonds live in the time area, x >= 150)
    struct C { int cx, cy, n, minX, maxX, minY, maxY; };
    std::vector<C> cls;
    for (int y = 26; y < h; y++) {
        for (int x = 30; x < w; x++) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            unsigned char B = px[0], G = px[1], R = px[2];
            int lum = (R * 299 + G * 587 + B * 114) / 1000;
            if (lum <= 110) continue;
            int best = -1, bestD = 10 * 10;
            for (int i = 0; i < (int)cls.size(); i++) {
                int dx = x - cls[i].cx, dy = y - cls[i].cy;
                int d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0) {
                C& c = cls[best];
                c.n++;
                c.cx = (c.cx * (c.n - 1) + x) / c.n;
                c.cy = (c.cy * (c.n - 1) + y) / c.n;
                if (x < c.minX) c.minX = x;
                if (x > c.maxX) c.maxX = x;
                if (y < c.minY) c.minY = y;
                if (y > c.maxY) c.maxY = y;
            } else {
                cls.push_back({ x, y, 1, x, x, y, y });
            }
        }
    }
    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);

    // diamond-like: small bounding box and a handful of pixels
    std::vector<C> keep;
    for (auto& c : cls) {
        if (c.n >= 4 && c.n <= 80 &&
            (c.maxX - c.minX) <= 14 && (c.maxY - c.minY) <= 12) {
            keep.push_back(c);
        }
    }
    if (keep.size() < 2) return false;

    // group into rows by y
    std::vector<std::vector<C>> rows;
    for (auto& c : keep) {
        int ri = -1;
        for (int i = 0; i < (int)rows.size(); i++) {
            if (abs(c.cy - rows[i][0].cy) <= 9) { ri = i; break; }
        }
        if (ri < 0) rows.push_back({ c });
        else rows[ri].push_back(c);
    }
    rowsOut.clear();
    for (auto& row : rows) {
        if (row.size() < 2) continue;
        std::vector<int> xs;
        for (auto& c : row) xs.push_back(c.cx);
        std::sort(xs.begin(), xs.end());
        rowsOut.push_back(xs);
    }
    return !rowsOut.empty();
}

// Find the time ruler row near the top of a window (light horizontal band).
static int FindRulerRow(int winLeft, int winTop, int w) {
    int top = winTop - 45;
    if (top < 0) top = 0;
    int h = winTop + 35 - top;
    if (h <= 0) return winTop + 26;
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(mem, hbm);
    BitBlt(mem, 0, 0, w, h, scr, winLeft, top, SRCCOPY | CAPTUREBLT);
    int bestY = winTop + 26, bestCnt = 0;
    for (int y = 0; y < h; y++) {
        int cnt = 0;
        for (int x = 0; x < w; x += 2) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            if (px[2] > 110 && px[1] > 110 && px[0] > 110) cnt++;
        }
        if (cnt > bestCnt) { bestCnt = cnt; bestY = top + y; }
    }
    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
    return bestY;
}

static void CollectTimelineCandidates(std::vector<HWND>& out) {
    if (g_ae_under && IsWindow(g_ae_under)) out.push_back(g_ae_under);
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        std::vector<HWND>* v = (std::vector<HWND>*)lp;
        RECT r;
        if (GetWindowRect(h, &r) &&
            (r.right - r.left) >= 350 && (r.bottom - r.top) >= 150) {
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (wcsstr(cls, L"DroverLord")) v->push_back(h);
        }
        EnumChildWindows(h, [](HWND c, LPARAM lp2) -> BOOL {
            std::vector<HWND>* v2 = (std::vector<HWND>*)lp2;
            RECT r2;
            if (GetWindowRect(c, &r2) &&
                (r2.right - r2.left) >= 350 && (r2.bottom - r2.top) >= 150) {
                wchar_t cls2[64] = {};
                GetClassNameW(c, cls2, 64);
                if (wcsstr(cls2, L"DroverLord")) v2->push_back(c);
            }
            return TRUE;
        }, lp);
        return TRUE;
    }, (LPARAM)&out);
}

static void EnsureTimelineWindow() {
    if (g_tl_hwnd && IsWindow(g_tl_hwnd)) return;
    std::vector<HWND> cands;
    CollectTimelineCandidates(cands);
    for (HWND wnd : cands) {
        RECT wr;
        if (!GetWindowRect(wnd, &wr)) continue;
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w < 350 || h < 150) continue;
        int stripTop = wr.top - 40;
        if (stripTop < 0) stripTop = 0;
        int stripH = h + 40;
        if (stripH > 200) stripH = 200;
        if (stripH < 60) continue;
        int x = 0, run = 0;
        if (!DetectPlayhead(wr, stripTop, stripH, w, x, run)) continue;
        g_tl_hwnd = wnd;
        g_tl_left = wr.left;
        g_tl_rulerY = FindRulerRow(wr.left, wr.top, w);
        break;
    }
}

// Map a y position inside the timeline window to a layer index (0 = topmost)
// by detecting the A/V features column rows (only layer rows have icons there).
static int FindLayerRowIndex(HWND wnd, int relY) {
    RECT wr;
    if (!GetWindowRect(wnd, &wr)) return -1;
    int w = 160, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return -1;
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(mem, hbm);
    BitBlt(mem, 0, 0, w, h, scr, wr.left, wr.top, SRCCOPY | CAPTUREBLT);

    std::vector<char> av(h, 0), name(h, 0);
    for (int y = 45; y < h - 4; y++) {
        int avCnt = 0, nameCnt = 0;
        for (int x = 4; x < 58; x++) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            int lum = (px[2] * 299 + px[1] * 587 + px[0] * 114) / 1000;
            if (lum > 90) avCnt++;
        }
        for (int x = 62; x < 140; x++) {
            unsigned char* px = (unsigned char*)bits + (y * w + x) * 4;
            int lum = (px[2] * 299 + px[1] * 587 + px[0] * 114) / 1000;
            if (lum > 90) nameCnt++;
        }
        av[y] = (avCnt >= 1);
        name[y] = (nameCnt >= 1);
    }
    // find text bands in the name column, tolerating small gaps
    std::vector<int> tops, bottoms;
    int y = 45;
    while (y < h - 4) {
        while (y < h - 4 && !name[y]) y++;
        if (y >= h - 4) break;
        int top = y, gap = 0;
        while (y < h - 4) {
            if (name[y]) { gap = 0; y++; }
            else { gap++; if (gap > 2) break; y++; }
        }
        int bottom = y - gap;
        if (bottom - top >= 5) { tops.push_back(top); bottoms.push_back(bottom); }
    }
    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);

    // drop the bottom scrollbar band and a leading header band
    while (!tops.empty() && bottoms.back() > h - 14) {
        tops.pop_back();
        bottoms.pop_back();
    }
    // keep only rows that also have A/V icons (layer rows, not property rows)
    std::vector<int> layerTops;
    for (int i = 0; i < (int)tops.size(); i++) {
        bool hasAV = false;
        for (int yy = tops[i]; yy < bottoms[i] && !hasAV; yy++) if (av[yy]) hasAV = true;
        if (hasAV) layerTops.push_back(tops[i]);
    }
    if (layerTops.empty()) return -1;
    wchar_t logbuf[256];
    wchar_t detail[128] = L"";
    for (int i = 0; i < (int)layerTops.size() && i < 12; i++) {
        wchar_t one[32];
        _snwprintf_s(one, 32, _TRUNCATE, L" %d", layerTops[i]);
        wcscat_s(detail, 128, one);
    }
    _snwprintf_s(logbuf, 256, _TRUNCATE, L"layer rows: %d (relY=%d) tops:%s",
                 (int)layerTops.size(), relY, detail);
    CurvesDebugLog(logbuf);
    // typical row pitch = median of consecutive top distances
    double pitch = 17.0;
    {
        std::vector<double> diffs;
        for (int i = 1; i < (int)layerTops.size(); i++) {
            double d = layerTops[i] - layerTops[i - 1];
            if (d >= 8.0 && d <= 40.0) diffs.push_back(d);
        }
        if (!diffs.empty()) {
            std::sort(diffs.begin(), diffs.end());
            pitch = diffs[diffs.size() / 2];
        }
    }
    // find the first layer top that aligns with the pitch (the timeline header
    // usually sits above and does not align)
    int firstTop = layerTops[0];
    int bestCount = -1;
    for (int t : layerTops) {
        int cnt = 0;
        for (int k = 0; k < (int)layerTops.size(); k++) {
            double expected = t + k * pitch;
            for (int tt : layerTops) {
                if (fabs((double)tt - expected) <= 4.0) { cnt++; break; }
            }
        }
        if (cnt > bestCount || (cnt == bestCount && t > firstTop)) {
            bestCount = cnt;
            firstTop = t;
        }
    }
    int idx = (int)floor((double)(relY - firstTop) / pitch);
    if (idx < 0) idx = 0;
    if (idx >= (int)layerTops.size()) idx = (int)layerTops.size() - 1;
    wchar_t l2[96];
    _snwprintf_s(l2, 96, _TRUNCATE, L"row map: firstTop=%d pitch=%.1f idx=%d",
                 firstTop, pitch, idx);
    CurvesDebugLog(l2);
    return idx;
}

// Select the layer whose timeline row is nearest the mouse, then reveal its
// keyframed properties (like pressing U) via the same JSX approach used by
// DalimaoShortcuts.
static void SelectLayerUnderMouse() {
    POINT cur;
    GetCursorPos(&cur);
    // use the window under the mouse first (the user hovers the timeline they
    // are looking at), otherwise fall back to the calibrated timeline
    HWND wnd = g_ae_under;
    if (!wnd || !IsWindow(wnd)) wnd = g_tl_hwnd;
    if (!wnd || !IsWindow(wnd)) {
        EnsureTimelineWindow();
        wnd = g_tl_hwnd;
    }
    if (!wnd || !IsWindow(wnd)) return;
    RECT wr;
    GetWindowRect(wnd, &wr);
    if (cur.x < wr.left || cur.x > wr.right) {
        // cursor is not over this window; fall back to the calibrated timeline
        if (wnd != g_tl_hwnd && g_tl_hwnd && IsWindow(g_tl_hwnd)) {
            wnd = g_tl_hwnd;
            GetWindowRect(wnd, &wr);
        }
    }
    wchar_t wdbg[160];
    _snwprintf_s(wdbg, 160, _TRUNCATE, L"sel: window %p rect=%d,%d,%d,%d mouse=%d,%d",
                 (void*)wnd, wr.left, wr.top, wr.right, wr.bottom, cur.x, cur.y);
    CurvesDebugLog(wdbg);
    int idx = FindLayerRowIndex(wnd, cur.y - wr.top);
    if (idx < 0) return;

    AEGP_SuiteHandler suites(g_sp);
    AEGP_CompH compH = NULL;
    if (suites.CompSuite12()->AEGP_GetMostRecentlyUsedComp(&compH) || !compH) {
        CurvesDebugLog(L"sel: no mru comp");
        return;
    }
    {
        AEGP_ItemH mruItem = NULL;
        if (!suites.CompSuite12()->AEGP_GetItemFromComp(compH, &mruItem)) {
            std::wstring nm = ItemName(suites, mruItem);
            wchar_t lb[192];
            _snwprintf_s(lb, 192, _TRUNCATE, L"sel: mru comp=%s", nm.c_str());
            CurvesDebugLog(lb);
        }
    }
    A_long num = 0;
    if (suites.LayerSuite9()->AEGP_GetCompNumLayers(compH, &num)) {
        CurvesDebugLog(L"sel: GetCompNumLayers failed");
        return;
    }
    wchar_t dbg[160];
    _snwprintf_s(dbg, 160, _TRUNCATE, L"sel: numLayers=%d idx=%d", (int)num, idx);
    CurvesDebugLog(dbg);
    if (num <= 0) return;
    if (idx >= num) idx = (int)num - 1;   // best effort: clamp to the last layer
    AEGP_LayerH layerH = NULL;
    if (suites.LayerSuite9()->AEGP_GetCompLayerByIndex(compH, idx, &layerH) || !layerH) {
        CurvesDebugLog(L"sel: GetCompLayerByIndex failed");
        return;
    }

    // native selection (AEGP_SetSelection), which GetActiveLayer can see
    AEGP_Collection2H coll = NULL;
    if (!suites.CollectionSuite2()->AEGP_NewCollection(g_plugin_id, &coll) && coll) {
        AEGP_CollectionItemV2 item = {};
        item.type = AEGP_CollectionItemType_LAYER;
        item.u.layer.layerH = layerH;
        suites.CollectionSuite2()->AEGP_CollectionPushBack(coll, &item);
        A_Err se = suites.CompSuite12()->AEGP_SetSelection(compH, coll);
        if (se) {
            wchar_t s2[96];
            _snwprintf_s(s2, 96, _TRUNCATE, L"sel: SetSelection err %d", (int)se);
            CurvesDebugLog(s2);
        }
        suites.CollectionSuite2()->AEGP_DisposeCollection(coll);
    } else {
        CurvesDebugLog(L"sel: NewCollection failed");
    }

    wchar_t logbuf[160];
    _snwprintf_s(logbuf, 160, _TRUNCATE, L"selected layer idx %d", idx);
    CurvesDebugLog(logbuf);
}

// Reveal (twirl open) the active layer's keyframed properties, like pressing U.
// Uses the same JSX idea as DalimaoShortcuts but without the PropertyType enum.
static void RevealActiveLayerKeyframes() {
    const char* jsx =
        "try{"
        "var it=app.project.activeItem;"
        "if(it){var L=it.selectedLayers;"
        "if(L.length>0){(function rev(o){"
        "if(!o)return;var n=0;try{n=o.numProperties;}catch(e){return;}"
        "for(var i=1;i<=n;i++){"
        "var p=null;try{p=o.property(i);}catch(e){continue;}"
        "if(!p)continue;"
        "var k=0,s=0;try{k=p.numKeys;}catch(e){}"
        "try{s=p.numProperties;}catch(e){}"
        "if(k>0){try{p.selected=true;}catch(e){}}"
        "if(s>0){rev(p);}"
        "}"
        "})(L[0]);}}"
        "}catch(e){}";
    AEGP_SuiteHandler suites(g_sp);
    AEGP_MemHandle resultMH = NULL, errorMH = NULL;
    suites.UtilitySuite6()->AEGP_ExecuteScript(
        g_plugin_id, jsx, FALSE, &resultMH, &errorMH);
    if (errorMH) {
        void* buf = NULL;
        if (!suites.MemorySuite1()->AEGP_LockMemHandle(errorMH, &buf) && buf) {
            CurvesDebugLog((const wchar_t*)buf);
            suites.MemorySuite1()->AEGP_UnlockMemHandle(errorMH);
        }
        suites.MemorySuite1()->AEGP_FreeMemHandle(errorMH);
    }
    if (resultMH) suites.MemorySuite1()->AEGP_FreeMemHandle(resultMH);
}

// Match the panel's horizontal scale to the AE timeline by measuring the
// playhead's pixel position on screen (thin solid blue line) against the
// known time range (display start time -> current time).
static bool CalibrateToTimeline() {
    AEGP_SuiteHandler suites(g_sp);
    {
        // diagnostics: list every comp and its current time
        AEGP_ProjectH proj = NULL;
        if (!suites.ProjSuite6()->AEGP_GetProjectByIndex(0, &proj) && proj) {
            AEGP_ItemH it = NULL;
            if (!suites.ItemSuite9()->AEGP_GetFirstProjItem(proj, &it)) {
                while (it) {
                    AEGP_ItemType type = AEGP_ItemType_NONE;
                    if (!suites.ItemSuite9()->AEGP_GetItemType(it, &type) &&
                        type == AEGP_ItemType_COMP) {
                        std::wstring nm = ItemName(suites, it);
                        A_Time tc{};
                        suites.ItemSuite9()->AEGP_GetItemCurrentTime(it, &tc);
                        wchar_t lb[256];
                        _snwprintf_s(lb, 256, _TRUNCATE, L"comp item: %s TC=%.2f",
                                     nm.c_str(), (double)TimeToSec(tc));
                        CurvesDebugLog(lb);
                    }
                    AEGP_ItemH next = NULL;
                    if (suites.ItemSuite9()->AEGP_GetNextProjItem(proj, it, &next)) break;
                    it = next;
                }
            }
        }
        AEGP_ItemH active = NULL;
        if (!suites.ItemSuite9()->AEGP_GetActiveItem(&active) && active) {
            std::wstring nm = ItemName(suites, active);
            A_Time tc{};
            suites.ItemSuite9()->AEGP_GetItemCurrentTime(active, &tc);
            wchar_t lb[256];
            _snwprintf_s(lb, 256, _TRUNCATE, L"active item: %s TC=%.2f",
                         nm.c_str(), (double)TimeToSec(tc));
            CurvesDebugLog(lb);
        }
    }
    AEGP_CompH compH = NULL;
    // the timeline shows the comp that owns the active layer; do NOT use the
    // most-recently-used comp, it may be a different comp (wrong current time)
    if (!g_layerH || suites.LayerSuite9()->AEGP_GetLayerParentComp(g_layerH, &compH) || !compH) {
        if (suites.CompSuite12()->AEGP_GetMostRecentlyUsedComp(&compH) || !compH) return false;
    }
    AEGP_ItemH itemH = NULL;
    if (suites.CompSuite12()->AEGP_GetItemFromComp(compH, &itemH) || !itemH) return false;
    A_Time t0{}, tc{};
    if (suites.CompSuite12()->AEGP_GetCompDisplayStartTime(compH, &t0)) return false;
    if (suites.ItemSuite9()->AEGP_GetItemCurrentTime(itemH, &tc)) return false;
    double T0 = TimeToSec(t0), TC = TimeToSec(tc);
    {
        std::wstring nm = ItemName(suites, itemH);
        wchar_t lb[320];
        _snwprintf_s(lb, 320, _TRUNCATE, L"cal: comp=%s T0=%.2f TC=%.2f",
                     nm.c_str(), (double)T0, (double)TC);
        CurvesDebugLog(lb);
    }
    double dt = TC - T0;
    if (dt < 0.05) {
        CurvesDebugLog(L"calibration skipped: playhead too close to ruler start");
        return false;
    }

    std::vector<HWND> cands;
    CollectTimelineCandidates(cands);
    int tried = 0;
    for (HWND wnd : cands) {
        RECT wr;
        if (!GetWindowRect(wnd, &wr)) continue;
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w < 350 || h < 150) continue;
        int stripTop = wr.top - 40;
        if (stripTop < 0) stripTop = 0;
        int stripH = h + 40;
        if (stripH > 200) stripH = 200;
        if (stripH < 60) continue;
        tried++;
        int x = 0, run = 0;
        if (!DetectPlayhead(wr, stripTop, stripH, w, x, run)) continue;
        double pxPerSec = (double)x / dt;
        if (pxPerSec < 1.0 || pxPerSec > 4000.0) continue;

        // Primary: direct measurement of the displayed keyframe diamonds
        // (pixel distance between diamonds / their time distance). This is
        // offset-free: it matches the timeline exactly even with a left margin.
        double diamondPx = 0.0;
        double diamondMargin = 0.0;
        std::vector<std::vector<int>> rows;
        if (DetectKeyDiamonds(wr, w, h, rows) && g_kfs.size() >= 2) {
            std::vector<double> samples;
            for (auto& xs : rows) {
                int kc = (int)g_kfs.size();
                if (abs((int)xs.size() - kc) > 3) continue;
                for (size_t i = 0; i + 1 < xs.size(); i++) {
                    double dtk = g_kfs[i + 1].time - g_kfs[i].time;
                    double dx = (double)(xs[i + 1] - xs[i]);
                    if (dtk > 0.0005 && dx > 2.0) samples.push_back(dx / dtk);
                }
            }
            if (!samples.empty()) {
                std::sort(samples.begin(), samples.end());
                double med = samples[samples.size() / 2];
                // the CTI position must agree with this scale given a plausible
                // left margin (the A/V features column width)
                double margin = (double)x - TC * med;
                if (med >= 1.0 && med <= 4000.0 && margin >= 0.0 && margin <= 150.0) {
                    diamondPx = med;
                    diamondMargin = margin;
                }
            }
        }
        {
            wchar_t rb[256] = L"diamond rows:";
            for (auto& xs : rows) {
                wchar_t one[64];
                _snwprintf_s(one, 64, _TRUNCATE, L" [%d]", (int)xs.size());
                wcscat_s(rb, 256, one);
            }
            CurvesDebugLog(rb);
        }
        if (diamondPx > 0.0) {
            pxPerSec = diamondPx;
            g_ruler_margin = diamondMargin;
        } else {
            // fallback: CTI with the learned left-margin correction
            double m = g_ruler_margin;
            if (m < 0.0) m = 0.0;
            if (m > 150.0) m = 150.0;
            pxPerSec = ((double)x - m) / dt;
        }

        g_tl_hwnd = wnd;
        g_tl_left = wr.left;
        g_tl_rulerY = FindRulerRow(wr.left, wr.top, w);
        // use the timeline's absolute pixels-per-second so key distances match
        g_tl_T0 = T0;
        g_tl_px = pxPerSec;
        g_auto_px = pxPerSec;
        double effPx = EffectivePx(pxPerSec);
        double winW = (double)(wr.right - wr.left);
        double span = (winW - g_ruler_margin) / pxPerSec;
        if (span < 0.01) span = 1.0;
        if (span > 600.0) span = 600.0;
        g_tl_span = span;
        g_viewT0 = g_tl_T0;
        g_pxPerSec = effPx;
        wchar_t logbuf[256];
        _snwprintf_s(logbuf, 256, _TRUNCATE,
                     L"calibrated: px/s=%.2f T0=%.2f TC=%.2f win=%p left=%d x=%d run=%d rulerY=%d diamond=%.2f margin=%.1f",
                     (double)pxPerSec, (double)T0, (double)TC,
                     (void*)wnd, wr.left, x, run, g_tl_rulerY, (double)diamondPx,
                     (double)g_ruler_margin);
        CurvesDebugLog(logbuf);
        return true;
    }
    wchar_t logbuf[128];
    _snwprintf_s(logbuf, 128, _TRUNCATE, L"calibration failed (tried %d windows)", tried);
    CurvesDebugLog(logbuf);
    return false;
}

static void ShowCurvesPanel() {
    AEGP_SuiteHandler suites(g_sp);

    LoadPanelShortcuts();

    POINT cur;
    GetCursorPos(&cur);
    g_ae_under = WindowFromPoint(cur);
    if (!g_ae_under) g_ae_under = g_ae_main;

    A_Err lerr = suites.LayerSuite9()->AEGP_GetActiveLayer(&g_layerH);
    if (lerr || !g_layerH) {
        // no layer selected: pick the layer under the mouse and reveal its keys
        CurvesDebugLog(L"no active layer selected - auto picking layer under mouse");
        SelectLayerUnderMouse();
        lerr = suites.LayerSuite9()->AEGP_GetActiveLayer(&g_layerH);
    }
    if (lerr || !g_layerH) {
        CurvesDebugLog(L"still no active layer");
        CreatePanel(true, L"请先选中一个图层\n\n然后在 AE 键盘快捷键中按下绑定的快捷键呼出本面板。");
        return;
    }

    // expand the layer's keyframed properties in the timeline (like pressing U)
    RevealActiveLayerKeyframes();

    g_props.clear();
    if (!CollectProperties(suites, g_layerH, g_props) || g_props.empty()) {
        CurvesDebugLog(L"layer has no keyframed properties");
        CreatePanel(true, L"当前图层没有带关键帧的属性。\n\n请先给某个属性打上关键帧再呼出本面板。");
        return;
    }

    g_panel.curProp = 0;
    g_panel.dimMask = 3;
    ReloadKeyframes();
    if (g_panel.curProp < 0) {
        CreatePanel(true, L"读取关键帧数据失败。");
        return;
    }
    {
        wchar_t kb[512] = L"keys:";
        for (auto& k : g_kfs) {
            wchar_t one[48];
            _snwprintf_s(one, 48, _TRUNCATE, L" %.3f", (double)k.time);
            wcscat_s(kb, 512, one);
        }
        CurvesDebugLog(kb);
    }
    CalibrateToTimeline();   // match AE timeline scale when the playhead is measurable
    wchar_t logbuf[256];
    _snwprintf_s(logbuf, 256, _TRUNCATE,
                 L"opening curves panel: layer=%p props=%d", (void*)g_layerH, (int)g_props.size());
    CurvesDebugLog(logbuf);
    CreatePanel(false, L"");
}

// ---------------- AE command hooks ----------------
static A_Err
UpdateMenuHook(AEGP_GlobalRefcon, AEGP_UpdateMenuRefcon, AEGP_WindowType) {
    A_Err err = A_Err_NONE;
    try {
        AEGP_SuiteHandler suites(g_sp);
        ERR(suites.CommandSuite1()->AEGP_EnableCommand(g_command));
    } catch (...) {
        CurvesDebugLog(L"exception in UpdateMenuHook");
    }
    return err;
}

static BOOL CALLBACK EnumAeMainWindowProc(HWND h, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcsstr(cls, L"AfterFX")) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

static HWND FindAeMainWindow() {
    HWND h = GetActiveWindow();
    if (h && !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return h;
    HWND found = NULL;
    EnumWindows(EnumAeMainWindowProc, (LPARAM)&found);
    return found;
}

static A_Err
CommandHook(AEGP_GlobalRefcon, AEGP_CommandRefcon, AEGP_Command command,
            AEGP_HookPriority, A_Boolean, A_Boolean* handledPB) {
    A_Err err = A_Err_NONE;
    try {
        if (command == g_command) {
            *handledPB = TRUE;
            if (s_panel_active) {
                CurvesDebugLog(L"command executed - closing curves panel");
                ClosePanel();
                return err;
            }
            s_panel_active = true;
            g_ae_main = FindAeMainWindow();
            CurvesDebugLog(L"command executed - opening curves panel");
            ShowCurvesPanel();
        }
    } catch (...) {
        CurvesDebugLog(L"exception in CommandHook");
        try {
            AEGP_SuiteHandler suites(g_sp);
            DisposeKeyframes(suites);
            DisposeProperties(suites);
        } catch (...) {}
        g_kfs.clear();
        s_panel_active = false;
    }
    return err;
}

// ---------------- Entry point ----------------
static A_Err
DoEntryPoint(SPBasicSuite* pica_basicP, A_long, A_long, AEGP_PluginID aegp_plugin_id) {
    A_Err err = A_Err_NONE;
    AEGP_SuiteHandler suites(pica_basicP);
    ERR(suites.CommandSuite1()->AEGP_GetUniqueCommand(&g_command));
    ERR(suites.CommandSuite1()->AEGP_InsertMenuCommand(
        g_command, "DalimaoCurves", AEGP_Menu_WINDOW, AEGP_MENU_INSERT_SORTED));
    ERR(suites.RegisterSuite5()->AEGP_RegisterCommandHook(
        g_plugin_id, AEGP_HP_BeforeAE, g_command, CommandHook, 0));
    ERR(suites.RegisterSuite5()->AEGP_RegisterUpdateMenuHook(
        g_plugin_id, UpdateMenuHook, NULL));
    CurvesDebugLog(L"plugin loaded");
    return err;
}

extern "C" {

DllExport A_Err
EntryPointFunc(
    struct SPBasicSuite* pica_basicP,
    A_long major_versionL,
    A_long minor_versionL,
    AEGP_PluginID aegp_plugin_id,
    AEGP_GlobalRefcon* global_refconP)
{
    A_Err err = A_Err_NONE;
    try {
        g_sp = pica_basicP;
        g_plugin_id = aegp_plugin_id;

        {
            HMODULE self_mod = NULL;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)(LPVOID)&EntryPointFunc, &self_mod) && self_mod) {
                wchar_t mod_path[MAX_PATH * 2] = {};
                DWORD n = GetModuleFileNameW(self_mod, mod_path, MAX_PATH * 2);
                if (n > 0) {
                    g_plugin_dir.assign(mod_path, mod_path + n);
                    size_t sep = g_plugin_dir.find_last_of(L"\\/");
                    if (sep != std::wstring::npos) g_plugin_dir.erase(sep);
                }
            }
        }
        LoadPanelSettings();
        err = DoEntryPoint(pica_basicP, major_versionL, minor_versionL, aegp_plugin_id);
    } catch (...) {
        CurvesDebugLog(L"exception in EntryPointFunc");
        err = A_Err_GENERIC;
    }
    return err;
}

} // extern "C"
