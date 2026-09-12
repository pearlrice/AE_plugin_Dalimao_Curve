#pragma once

#include "CurveThumbnail.h"

// Compact, DPI-aware Win32 presentation for the native curve controls popup.
// This file deliberately contains no After Effects SDK calls so it can also be
// included by the standalone preview harness.
namespace popup_ui {

namespace detail {
constexpr int kLogicalWidth = 448;
constexpr int kLogicalHeight = 618;

UINT g_dpi = 96;
HFONT g_uiFont = nullptr;
HFONT g_uiFontSmall = nullptr;
HFONT g_uiFontStrong = nullptr;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_controlBrush = nullptr;
HBRUSH g_cardBrush = nullptr;

constexpr COLORREF kBackground = RGB(25, 27, 31);
constexpr COLORREF kCard = RGB(33, 36, 42);
constexpr COLORREF kControl = RGB(42, 45, 52);
constexpr COLORREF kControlHover = RGB(50, 54, 62);
constexpr COLORREF kPressed = RGB(27, 30, 36);
constexpr COLORREF kBorder = RGB(62, 66, 75);
constexpr COLORREF kText = RGB(232, 234, 239);
constexpr COLORREF kMuted = RGB(145, 151, 162);
constexpr COLORREF kAccent = RGB(78, 151, 255);
constexpr COLORREF kAccentPressed = RGB(57, 126, 224);
constexpr COLORREF kDisabled = RGB(92, 97, 107);

int Scale(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

RECT ScaledRect(int left, int top, int right, int bottom) {
    return { Scale(left), Scale(top), Scale(right), Scale(bottom) };
}

void ReleaseFont(HFONT& font) {
    if (font) DeleteObject(font);
    font = nullptr;
}

void ReleaseBrush(HBRUSH& brush) {
    if (brush) DeleteObject(brush);
    brush = nullptr;
}

HFONT MakeFont(int points, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(points, static_cast<int>(g_dpi), 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return CreateFontIndirectW(&lf);
}

void FillSolid(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void RoundBox(HDC dc, const RECT& rect, COLORREF fill, COLORREF border, int radius) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Scale(radius), Scale(radius));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void Text(HDC dc, const wchar_t* value, RECT rect, COLORREF color, UINT format, HFONT font) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, value, -1, &rect, format);
    SelectObject(dc, oldFont);
}

void SetChildFont(HWND child, HFONT font = nullptr) {
    if (child) SendMessageW(child, WM_SETFONT, WPARAM(font ? font : g_uiFont), FALSE);
}

HWND Add(HWND parent, const wchar_t* type, const wchar_t* text, int id,
    int x, int y, int width, int height, DWORD style, DWORD exStyle = 0) {
    HWND child = CreateWindowExW(exStyle, type, text, WS_CHILD | WS_VISIBLE | style,
        Scale(x), Scale(y), Scale(width), Scale(height), parent,
        HMENU(static_cast<INT_PTR>(id)), g_module, nullptr);
    SetChildFont(child);
    return child;
}

std::wstring ItemText(HWND combo, UINT itemId) {
    if (itemId == UINT(-1)) {
        int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        if (selected == CB_ERR) return {};
        itemId = static_cast<UINT>(selected);
    }
    LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, itemId, 0);
    if (length == CB_ERR || length < 0) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, itemId, LPARAM(text.data()));
    return std::wstring(text.data(), static_cast<size_t>(length));
}

const wchar_t* ComboPlaceholder(int id) {
    switch (id) {
    case PROP: return L"选择属性";
    case SEGMENT: return L"选择关键帧区间";
    case DIMENSION: return L"维度";
    case LIBRARY: return L"已保存模板";
    default: return L"请选择";
    }
}

void DrawCurveThumbnail(HDC dc, RECT rect, const curve_presets::Preset& preset, COLORREF color) {
    if (rect.right - rect.left < Scale(12) || rect.bottom - rect.top < Scale(12)) return;
    const auto samples = curve_thumbnail::Sample(preset, 49);
    if (samples.count < 2) return;

    HPEN guidePen = CreatePen(PS_SOLID, 1, RGB(57, 61, 69));
    HGDIOBJ oldPen = SelectObject(dc, guidePen);
    const int middleX = (rect.left + rect.right) / 2;
    const int middleY = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, rect.left, middleY, nullptr);
    LineTo(dc, rect.right, middleY);
    MoveToEx(dc, middleX, rect.top, nullptr);
    LineTo(dc, middleX, rect.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(guidePen);

    const double range = std::max(0.001, samples.maxY - samples.minY);
    const double padding = std::max(0.04, range * 0.08);
    const double minY = samples.minY - padding;
    const double maxY = samples.maxY + padding;
    const double visibleRange = maxY - minY;
    std::array<POINT, curve_thumbnail::kMaxSamples> points{};
    for (std::size_t index = 0; index < samples.count; ++index) {
        const auto& point = samples.points[index];
        points[index].x = rect.left + static_cast<LONG>(std::lround(
            point.x * static_cast<double>(rect.right - rect.left - 1)));
        points[index].y = rect.bottom - 1 - static_cast<LONG>(std::lround(
            (point.y - minY) / visibleRange * static_cast<double>(rect.bottom - rect.top - 1)));
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    HPEN curvePen = CreatePen(PS_SOLID, std::max(1, Scale(2)), color);
    oldPen = SelectObject(dc, curvePen);
    Polyline(dc, points.data(), static_cast<int>(samples.count));
    SelectObject(dc, oldPen);
    DeleteObject(curvePen);
    RestoreDC(dc, saved);
}

const curve_presets::Preset* BuiltinPreset(int id) {
    static const std::vector<curve_presets::Preset> presets = curve_presets::Builtins();
    const int index = id - PRESET;
    return index >= 0 && index < static_cast<int>(presets.size()) ? &presets[index] : nullptr;
}

const curve_presets::Preset* UserPreset(int index) {
    return index >= 0 && index < static_cast<int>(g_userPresets.size()) ? &g_userPresets[index] : nullptr;
}

void PaintCombo(HWND hwnd, HDC dc) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    FillSolid(dc, rect, kCard);
    RoundBox(dc, rect, kControl, GetFocus() == hwnd ? kAccent : kBorder, 5);
    const int id = GetDlgCtrlID(hwnd);
    const int selected = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    std::wstring text = ItemText(hwnd, UINT(-1));
    const bool hasText = !text.empty();
    if (!hasText) text = ComboPlaceholder(id);
    RECT label = rect;
    label.left += Scale(8);
    label.right -= Scale(24);
    if (id == LIBRARY && hasText) {
        if (const auto* preset = UserPreset(selected)) {
            RECT graph = label;
            graph.right = std::min(label.right, graph.left + Scale(78));
            graph.top += Scale(4);
            graph.bottom -= Scale(4);
            DrawCurveThumbnail(dc, graph, *preset, IsWindowEnabled(hwnd) ? kAccent : kDisabled);
            label.left = graph.right + Scale(10);
        }
    }
    Text(dc, text.c_str(), label, !hasText ? kMuted : IsWindowEnabled(hwnd) ? kText : kDisabled,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, g_uiFont);
    const int x = rect.right - Scale(13), y = (rect.top + rect.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, Scale(1), kMuted);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x - Scale(3), y - Scale(1), nullptr);
    LineTo(dc, x, y + Scale(2)); LineTo(dc, x + Scale(4), y - Scale(2));
    SelectObject(dc, old); DeleteObject(pen);
}

LRESULT CALLBACK ComboProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(hwnd, &paint);
        PaintCombo(hwnd, dc); EndPaint(hwnd, &paint); return 0;
    }
    if (message == WM_PRINTCLIENT) { PaintCombo(hwnd, reinterpret_cast<HDC>(wp)); return 0; }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ComboProc, id);
    LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (message == CB_SETCURSEL || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        InvalidateRect(hwnd, nullptr, FALSE);
    return result;
}

} // namespace detail

SIZE SizeAtDpi(UINT dpi) {
    if (!dpi) dpi = 96;
    return { MulDiv(detail::kLogicalWidth, static_cast<int>(dpi), 96),
        MulDiv(detail::kLogicalHeight, static_cast<int>(dpi), 96) };
}

int HeaderHeight() {
    return detail::Scale(48);
}

UINT FitDpi(RECT work, UINT dpi) {
    const int available = std::min(int((int64_t(work.right) - work.left) * 96 / detail::kLogicalWidth),
        int((int64_t(work.bottom) - work.top) * 96 / detail::kLogicalHeight));
    return std::min(dpi ? dpi : 96U, static_cast<UINT>(std::max(1, available)));
}

RECT CursorPlacement(POINT cursor, RECT work, UINT dpi) {
    dpi = FitDpi(work, dpi);
    SIZE size = SizeAtDpi(dpi);
    int gap = MulDiv(14, static_cast<int>(dpi ? dpi : 96), 96);
    const int cursorX = static_cast<int>(cursor.x);
    const int cursorY = static_cast<int>(cursor.y);
    int x = cursorX + gap;
    int y = cursorY + gap;
    if (x + size.cx > static_cast<int>(work.right)) x = cursorX - gap - size.cx;
    if (y + size.cy > static_cast<int>(work.bottom)) y = cursorY - gap - size.cy;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int maxX = std::max(workLeft, static_cast<int>(work.right - size.cx));
    const int maxY = std::max(workTop, static_cast<int>(work.bottom - size.cy));
    x = std::clamp(x, workLeft, maxX);
    y = std::clamp(y, workTop, maxY);
    return { x, y, x + size.cx, y + size.cy };
}

void Cleanup() {
    detail::ReleaseFont(detail::g_uiFont);
    detail::ReleaseFont(detail::g_uiFontSmall);
    detail::ReleaseFont(detail::g_uiFontStrong);
    detail::ReleaseBrush(detail::g_backgroundBrush);
    detail::ReleaseBrush(detail::g_controlBrush);
    detail::ReleaseBrush(detail::g_cardBrush);
}

void Initialize(UINT dpi) {
    Cleanup();
    detail::g_dpi = dpi ? dpi : 96;
    detail::g_uiFont = detail::MakeFont(9, FW_NORMAL);
    detail::g_uiFontSmall = detail::MakeFont(8, FW_NORMAL);
    detail::g_uiFontStrong = detail::MakeFont(9, FW_SEMIBOLD);
    detail::g_backgroundBrush = CreateSolidBrush(detail::kBackground);
    detail::g_controlBrush = CreateSolidBrush(detail::kControl);
    detail::g_cardBrush = CreateSolidBrush(detail::kCard);
}

void CreateChildren(HWND parent) {
    using namespace detail;
    INITCOMMONCONTROLSEX common{ sizeof(common), ICC_BAR_CLASSES };
    InitCommonControlsEx(&common);

    SIZE size = SizeAtDpi(g_dpi);
    HRGN region = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, Scale(12), Scale(12));
    if (region && !SetWindowRgn(parent, region, TRUE)) DeleteObject(region);

    Add(parent, L"BUTTON", L"×", RETURN_LAYERS, 404, 10, 28, 28,
        BS_OWNERDRAW | WS_TABSTOP);
    Add(parent, L"BUTTON", L"读取", REFRESH, 350, 68, 70, 26,
        BS_OWNERDRAW | WS_TABSTOP);
    Add(parent, L"COMBOBOX", L"", PROP, 20, 98, 400, 220,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    Add(parent, L"COMBOBOX", L"", SEGMENT, 20, 126, 270, 220,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    Add(parent, L"COMBOBOX", L"", DIMENSION, 296, 126, 124, 140,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);

    const wchar_t* presetNames[] = {
        L"线性", L"标准缓动", L"出长 / 入短", L"出短 / 入长", L"双侧较长", L"双侧最长"
    };
    for (int i = 0; i < 6; ++i) {
        Add(parent, L"BUTTON", presetNames[i], PRESET + i,
            20 + (i % 3) * 136, 190 + (i / 3) * 64, 128, 58, BS_OWNERDRAW | WS_TABSTOP);
    }

    HWND outLabel = Add(parent, L"STATIC", L"起点出手柄长度  33.3%", OUT_LABEL,
        20, 355, 400, 17, SS_LEFT | SS_CENTERIMAGE);
    HWND outSlider = Add(parent, TRACKBAR_CLASSW, L"", OUT_SLIDER,
        20, 372, 400, 20, TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
    HWND inLabel = Add(parent, L"STATIC", L"终点入手柄长度  33.3%", IN_LABEL,
        20, 394, 400, 17, SS_LEFT | SS_CENTERIMAGE);
    HWND inSlider = Add(parent, TRACKBAR_CLASSW, L"", IN_SLIDER,
        20, 411, 400, 20, TBS_HORZ | TBS_NOTICKS | WS_TABSTOP);
    SetChildFont(outLabel, g_uiFontStrong);
    SetChildFont(inLabel, g_uiFontStrong);
    for (HWND slider : { outSlider, inSlider }) {
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(1, 1000));
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 100);
    }

    Add(parent, L"COMBOBOX", L"", LIBRARY, 20, 470, 400, 260,
        CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP);
    Add(parent, L"BUTTON", L"保存", SAVE, 20, 510, 94, 28, BS_OWNERDRAW | WS_TABSTOP);
    Add(parent, L"BUTTON", L"应用", APPLY, 122, 510, 94, 28, BS_OWNERDRAW | WS_TABSTOP);
    Add(parent, L"BUTTON", L"删除", DELETE_PRESET, 224, 510, 94, 28, BS_OWNERDRAW | WS_TABSTOP);
    Add(parent, L"BUTTON", L"重载", RELOAD, 326, 510, 94, 28, BS_OWNERDRAW | WS_TABSTOP);
    HWND status = Add(parent, L"STATIC", L"", STATUS, 20, 550, 400, 56,
        SS_LEFT | SS_NOPREFIX);
    SetChildFont(status, g_uiFontSmall);
    for (int id : { PROP, SEGMENT, DIMENSION, LIBRARY }) {
        HWND combo = GetDlgItem(parent, id);
        SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), Scale(id == LIBRARY ? 34 : 24));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, Scale(id == LIBRARY ? 48 : 24));
        SetWindowSubclass(combo, ComboProc, 1, 0);
    }
}

void ChangeDpi(HWND parent, UINT dpi, RECT suggested) {
    using namespace detail;
    const UINT oldDpi = g_dpi;
    struct ChildLayout { HWND hwnd; RECT rect; };
    std::vector<ChildLayout> children;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect{}; GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
        int id = GetDlgCtrlID(child);
        if (id == PROP || id == SEGMENT || id == DIMENSION || id == LIBRARY) {
            RECT dropdown{};
            if (SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, reinterpret_cast<LPARAM>(&dropdown)))
                rect.bottom = rect.top + dropdown.bottom - dropdown.top;
        }
        children.push_back({ child, rect });
    }
    Initialize(dpi);
    SIZE size = SizeAtDpi(g_dpi);
    SetWindowPos(parent, nullptr, suggested.left, suggested.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    for (const auto& child : children) {
        const RECT& r = child.rect;
        int id = GetDlgCtrlID(child.hwnd);
        SetChildFont(child.hwnd, id == STATUS ? g_uiFontSmall :
            (id == OUT_LABEL || id == IN_LABEL) ? g_uiFontStrong : g_uiFont);
        if (id == PROP || id == SEGMENT || id == DIMENSION || id == LIBRARY) {
            SendMessageW(child.hwnd, CB_SETITEMHEIGHT, WPARAM(-1), Scale(id == LIBRARY ? 34 : 24));
            SendMessageW(child.hwnd, CB_SETITEMHEIGHT, 0, Scale(id == LIBRARY ? 48 : 24));
        }
        MoveWindow(child.hwnd, MulDiv(r.left, g_dpi, oldDpi), MulDiv(r.top, g_dpi, oldDpi),
            MulDiv(r.right - r.left, g_dpi, oldDpi), MulDiv(r.bottom - r.top, g_dpi, oldDpi), TRUE);
    }
    HRGN region = CreateRoundRectRgn(0, 0, size.cx + 1, size.cy + 1, Scale(12), Scale(12));
    if (region && !SetWindowRgn(parent, region, TRUE)) DeleteObject(region);
    RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void PaintContent(HDC dc, RECT client) {
    using namespace detail;
    FillRect(dc, &client, g_backgroundBrush);

    Text(dc, L"DALIMAO CURVES", ScaledRect(16, 10, 215, 32), kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontStrong);
    RECT badge = ScaledRect(155, 13, 205, 29);
    RoundBox(dc, badge, RGB(39, 45, 55), RGB(55, 69, 88), 5);
    Text(dc, L"NATIVE", badge, RGB(143, 183, 238),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);
    Text(dc, L"AE KEYFRAME EASING", ScaledRect(16, 29, 260, 44), kMuted,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);

    RoundBox(dc, ScaledRect(12, 52, 436, 156), kCard, kBorder, 8);
    Text(dc, L"SELECTION  ·  AE 选择", ScaledRect(20, 62, 340, 92), kMuted,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);
    RoundBox(dc, ScaledRect(12, 164, 436, 320), kCard, kBorder, 8);
    Text(dc, L"EASING PRESETS  ·  缓动预设", ScaledRect(20, 169, 420, 188), kMuted,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);
    RoundBox(dc, ScaledRect(12, 328, 436, 438), kCard, kBorder, 8);
    Text(dc, L"HANDLE INFLUENCE  ·  手柄长度", ScaledRect(20, 334, 420, 353), kMuted,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);
    RoundBox(dc, ScaledRect(12, 446, 436, 546), kCard, kBorder, 8);
    Text(dc, L"MY TEMPLATES  ·  已保存模板", ScaledRect(20, 450, 320, 468), kMuted,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_uiFontSmall);
}

bool Paint(HWND hwnd) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    PaintContent(dc, client);
    EndPaint(hwnd, &paint);
    return true;
}

bool DrawItem(const DRAWITEMSTRUCT* item) {
    using namespace detail;
    if (!item || !item->hwndItem) return false;
    int id = GetDlgCtrlID(item->hwndItem);
    wchar_t className[24]{};
    GetClassNameW(item->hwndItem, className, static_cast<int>(std::size(className)));
    const bool combo = _wcsicmp(className, L"ComboBox") == 0;
    RECT rect = item->rcItem;

    if (combo) {
        bool selected = (item->itemState & ODS_SELECTED) != 0 && (item->itemState & ODS_COMBOBOXEDIT) == 0;
        FillSolid(item->hDC, rect, selected ? kControlHover : kControl);
        std::wstring value = ItemText(item->hwndItem, item->itemID);
        COLORREF color = kText;
        if (value.empty()) { value = ComboPlaceholder(id); color = kMuted; }
        rect.left += Scale(9);
        rect.right -= Scale(19);
        if (id == LIBRARY && item->itemID != UINT(-1)) {
            if (const auto* preset = UserPreset(static_cast<int>(item->itemID))) {
                RECT graph = rect;
                graph.right = std::min(rect.right, graph.left + Scale(82));
                graph.top += Scale(4);
                graph.bottom -= Scale(4);
                DrawCurveThumbnail(item->hDC, graph, *preset,
                    (item->itemState & ODS_DISABLED) ? kDisabled : kAccent);
                rect.left = graph.right + Scale(10);
            }
        }
        Text(item->hDC, value.c_str(), rect, color,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, g_uiFont);
        if (item->itemState & ODS_COMBOBOXEDIT) {
            POINT arrow[] = {
                { item->rcItem.right - Scale(14), item->rcItem.top + Scale(11) },
                { item->rcItem.right - Scale(8), item->rcItem.top + Scale(11) },
                { item->rcItem.right - Scale(11), item->rcItem.top + Scale(15) }
            };
            HBRUSH arrowBrush = CreateSolidBrush(kMuted);
            HPEN arrowPen = CreatePen(PS_SOLID, 1, kMuted);
            HGDIOBJ oldBrush = SelectObject(item->hDC, arrowBrush);
            HGDIOBJ oldPen = SelectObject(item->hDC, arrowPen);
            Polygon(item->hDC, arrow, 3);
            SelectObject(item->hDC, oldPen);
            SelectObject(item->hDC, oldBrush);
            DeleteObject(arrowPen);
            DeleteObject(arrowBrush);
        }
        return true;
    }

    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool close = id == RETURN_LAYERS;
    const bool preset = id >= PRESET && id < PRESET + 6;
    COLORREF fill = pressed ? kPressed : (preset ? RGB(40, 44, 51) : kControl);
    COLORREF border = pressed ? kAccentPressed : (close ? kBackground : kBorder);
    RoundBox(item->hDC, rect, fill, border, close ? 7 : 6);

    wchar_t title[128]{};
    GetWindowTextW(item->hwndItem, title, static_cast<int>(std::size(title)));
    COLORREF foreground = disabled ? kDisabled : (close ? kMuted : kText);
    if (preset) {
        RECT graph = rect;
        graph.left += Scale(8);
        graph.right -= Scale(8);
        graph.top += Scale(5);
        graph.bottom -= Scale(18);
        if (const auto* builtin = BuiltinPreset(id))
            DrawCurveThumbnail(item->hDC, graph, *builtin, disabled ? kDisabled : kAccent);
        RECT label = rect;
        label.top = label.bottom - Scale(18);
        label.left += Scale(6);
        label.right -= Scale(6);
        Text(item->hDC, title, label, foreground,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, g_uiFontSmall);
    } else {
        Text(item->hDC, title, rect, foreground,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, close ? g_uiFontStrong : g_uiFont);
    }
    if ((item->itemState & ODS_FOCUS) && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -Scale(3), -Scale(3));
        DrawFocusRect(item->hDC, &focus);
    }
    return true;
}

bool MeasureItem(MEASUREITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_COMBOBOX) return false;
    item->itemHeight = static_cast<UINT>(detail::Scale(item->CtlID == LIBRARY ? 48 : 24));
    return true;
}

HBRUSH ControlColor(HDC dc, HWND control, UINT message) {
    using namespace detail;
    if (!dc) return nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(control) ? kText : kDisabled);
    if (message == WM_CTLCOLORLISTBOX) {
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, kControl);
        return g_controlBrush;
    }
    if (message == WM_CTLCOLORSTATIC) {
        int id = control ? GetDlgCtrlID(control) : 0;
        return (id == OUT_LABEL || id == IN_LABEL) ? g_cardBrush : g_backgroundBrush;
    }
    return g_controlBrush;
}

LRESULT CustomDraw(NMHDR* header) {
    using namespace detail;
    if (!header || (header->idFrom != OUT_SLIDER && header->idFrom != IN_SLIDER)) return CDRF_DODEFAULT;
    auto draw = reinterpret_cast<NMCUSTOMDRAW*>(header);
    if (draw->dwDrawStage == CDDS_PREPAINT) {
        RECT client{}; GetClientRect(header->hwndFrom, &client);
        FillSolid(draw->hdc, client, kCard);
        return CDRF_NOTIFYITEMDRAW;
    }
    if (draw->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    if (draw->dwItemSpec == TBCD_CHANNEL) {
        RECT channel = draw->rc;
        int middle = (channel.top + channel.bottom) / 2;
        channel.top = middle - Scale(2);
        channel.bottom = middle + Scale(2);
        RoundBox(draw->hdc, channel, RGB(55, 59, 68), RGB(55, 59, 68), 3);
        int minimum = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETRANGEMIN, 0, 0));
        int maximum = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETRANGEMAX, 0, 0));
        int position = static_cast<int>(SendMessageW(header->hwndFrom, TBM_GETPOS, 0, 0));
        RECT fill = channel;
        if (maximum > minimum) fill.right = fill.left + MulDiv(channel.right - channel.left,
            std::clamp(position, minimum, maximum) - minimum, maximum - minimum);
        if (fill.right > fill.left) RoundBox(draw->hdc, fill, kAccent, kAccent, 3);
        return CDRF_SKIPDEFAULT;
    }
    if (draw->dwItemSpec == TBCD_THUMB) {
        RECT thumb = draw->rc;
        int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
        thumb = { x - Scale(5), y - Scale(5), x + Scale(5), y + Scale(5) };
        HBRUSH brush = CreateSolidBrush(IsWindowEnabled(header->hwndFrom) ? kAccent : kDisabled);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(188, 215, 255));
        HGDIOBJ oldBrush = SelectObject(draw->hdc, brush);
        HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
        Ellipse(draw->hdc, thumb.left, thumb.top, thumb.right, thumb.bottom);
        SelectObject(draw->hdc, oldPen);
        SelectObject(draw->hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

} // namespace popup_ui

