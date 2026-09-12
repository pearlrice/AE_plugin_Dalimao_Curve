#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HINSTANCE g_module = GetModuleHandleW(nullptr);
enum ControlId { PROP = 101, SEGMENT, DIMENSION, REFRESH, OUT_SLIDER, IN_SLIDER,
    OUT_LABEL, IN_LABEL, LIBRARY, SAVE, APPLY, DELETE_PRESET, RELOAD, STATUS, RETURN_LAYERS, PRESET = 200 };
#include "../DalimaoCurves/CurvePopupUI.inl"

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: popup_ui::Paint(hwnd); return 0;
    case WM_PRINTCLIENT: {
        RECT client{}; GetClientRect(hwnd, &client);
        popup_ui::PaintContent(reinterpret_cast<HDC>(wp), client); return 0;
    }
    case WM_DRAWITEM: return popup_ui::DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
    case WM_MEASUREITEM: return popup_ui::MeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(popup_ui::ControlColor(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp), msg));
    case WM_NOTIFY: if (reinterpret_cast<NMHDR*>(lp)->code == NM_CUSTOMDRAW)
        return popup_ui::CustomDraw(reinterpret_cast<NMHDR*>(lp)); break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t** argv) {
    const UINT dpi = argc > 2 ? static_cast<UINT>(_wtoi(argv[2])) : 96;
    for (UINT testDpi : { 96U, 144U, 192U }) {
        for (RECT work : { RECT{0, 0, 1920, 1080}, RECT{-1920, -1080, 0, 0} }) {
            for (POINT p : { POINT{work.left, work.top}, POINT{work.right - 1, work.bottom - 1},
                POINT{(work.left + work.right) / 2, (work.top + work.bottom) / 2} }) {
                RECT r = popup_ui::CursorPlacement(p, work, testDpi);
                assert(r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom);
            }
        }
    }
    popup_ui::Initialize(96);
    WNDCLASSW wc{}; wc.hInstance = g_module; wc.lpfnWndProc = PreviewProc; wc.lpszClassName = L"DalimaoPreview";
    RegisterClassW(&wc);
    SIZE size = popup_ui::SizeAtDpi(96);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Preview", WS_POPUP, 0, 0, size.cx, size.cy, nullptr, nullptr, g_module, nullptr);
    assert(hwnd);
    popup_ui::CreateChildren(hwnd);
    for (auto item : { std::pair{PROP, L"变换 / 不透明度"}, {SEGMENT, L"关键帧 1 → 2  ·  0.00s – 1.00s"},
        {DIMENSION, L"全部维度"}, {LIBRARY, L"我的缓动模板"} }) {
        HWND combo = GetDlgItem(hwnd, item.first);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.second));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }
    SendMessageW(GetDlgItem(hwnd, OUT_SLIDER), TBM_SETPOS, TRUE, 1000);
    SendMessageW(GetDlgItem(hwnd, IN_SLIDER), TBM_SETPOS, TRUE, 1);
    SetWindowTextW(GetDlgItem(hwnd, OUT_LABEL), L"起点出手柄长度  100.0%");
    SetWindowTextW(GetDlgItem(hwnd, IN_LABEL), L"终点入手柄长度  0.1%");
    SetWindowTextW(GetDlgItem(hwnd, STATUS), L"已读取 AE 选择。直接在 AE 原生图形编辑器中调整曲线。");
    if (dpi != 96) {
        popup_ui::ChangeDpi(hwnd, dpi, {0, 0, 0, 0});
        assert(SendMessageW(GetDlgItem(hwnd, PROP), CB_GETCURSEL, 0, 0) == 0);
        assert(SendMessageW(GetDlgItem(hwnd, OUT_SLIDER), TBM_GETPOS, 0, 0) == 1000);
    }
    if (argc > 3) SetWindowTextW(GetDlgItem(hwnd, STATUS), L"未能切换 AE 图形编辑器：检测到多个可见时间轴，请保留一个时间轴并重试。当前工具仍保留，可再次按快捷键返回图层滑条。");
    size = popup_ui::SizeAtDpi(dpi);
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, size.cx, size.cy);
    HGDIOBJ old = SelectObject(dc, bitmap);
    SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    Gdiplus::GdiplusStartupInput input; ULONG_PTR token{};
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    int result = 0;
    {
        Gdiplus::Bitmap output(bitmap, nullptr);
        CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        result = output.Save(argc > 1 ? argv[1] : L"popup-preview.png", &png) == Gdiplus::Ok ? 0 : 1;
    }
    Gdiplus::GdiplusShutdown(token);
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
    DestroyWindow(hwnd); popup_ui::Cleanup();
    std::cout << "Popup placement checks passed; render status=" << result << '\n';
    return result;
}
