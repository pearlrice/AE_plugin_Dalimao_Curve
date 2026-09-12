#define NOMINMAX
#include <windows.h>
#include <uiautomation.h>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>
static DWORD g_probePid = 0;
static DWORD ProbeProcessId() { return g_probePid; }
// Read-only benchmark: reuse the exact bridge discovery functions against the
// selected AE process. Never call Open/Close/PerformRequest or SendInput here.
#define GetCurrentProcessId ProbeProcessId
#include "../DalimaoCurves/NativeGraphBridge.h"
#undef GetCurrentProcessId
static double Millis(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
int wmain(int argc, wchar_t** argv) {
    if(argc != 2) return 2;
    HWND hwnd = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    GetWindowThreadProcessId(hwnd, &g_probePid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_probePid);
    wchar_t path[2048]{}; DWORD size=2048;
    bool valid = process && QueryFullProcessImageNameW(process,0,path,&size) && std::wstring(path).ends_with(L"\\AfterFX.exe");
    if(process) CloseHandle(process);
    if(!valid) return 3;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
    if(FAILED(hr)) return 4;
    std::vector<HWND> panels;
    EnumChildWindows(hwnd, [](HWND child, LPARAM data)->BOOL {
        wchar_t title[128]{}; GetClassNameW(child,title,128);
        if(IsWindowVisible(child))
            reinterpret_cast<std::vector<HWND>*>(data)->push_back(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&panels));
    auto panelStart=std::chrono::steady_clock::now(); int found=0;
    for(HWND panel:panels) {
        IUIAutomationElement* element=nullptr;
        if(SUCCEEDED(automation->ElementFromHandle(panel,&element)) && element) {
            if(NativeGraphBridge::detail::ElementName(element)==L"AE Timeline" && NativeGraphBridge::detail::HasVisibleRect(element)) ++found;
            element->Release();
        }
    }
    std::cout << "native_panels="<<panels.size()<<" timelines="<<found<<" panel_scan_ms="<<Millis(panelStart)<<std::endl;
    for(int i=0;i<3;i++) {
        IUIAutomationElement* timeline=nullptr;
        auto start=std::chrono::steady_clock::now();
        hr=NativeGraphBridge::detail::FindUniqueTimeline(automation,hwnd,&timeline);
        double findMs=Millis(start);
        if (i == 0 && timeline) {
            UIA_HWND native=0; timeline->get_CurrentNativeWindowHandle(&native);
            wchar_t cls[256]{}, title[256]{};
            GetClassNameW(reinterpret_cast<HWND>(native),cls,256);
            GetWindowTextW(reinterpret_cast<HWND>(native),title,256);
            std::wcout << L"timeline_hwnd=" << native << L" class=" << cls << L" title=" << title << std::endl;
        }
        start=std::chrono::steady_clock::now();
        auto state=timeline ? NativeGraphBridge::detail::ReadGraphState(automation,timeline) : NativeGraphBridge::detail::GraphState::Ambiguous;
        std::cout << "probe="<<i<<" find_ms="<<findMs<<" read_ms="<<Millis(start)<<" state="<<int(state)<<" hr="<<hr<<std::endl;
        if(timeline) timeline->Release();
    }
    automation->Release(); CoUninitialize();
}
