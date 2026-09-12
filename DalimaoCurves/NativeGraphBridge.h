#pragma once

#include <windows.h>
#include <unknwn.h>
#include <oaidl.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

namespace NativeGraphBridge {
inline constexpr wchar_t ControlsWindowClass[] = L"DalimaoCurveCursorPopup";
namespace detail {

enum class GraphState {
    LayerBars,
    GraphEditor,
    Ambiguous,
};

struct State {
    HANDLE wakeEvent = nullptr;
    HANDLE worker = nullptr;
    DWORD workerId = 0;
    HMODULE modulePin = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> shutdownStarted{false};
    std::atomic<bool> processing{false};
    std::atomic<bool> failed{false};
    std::atomic<unsigned long long> requestedGeneration{0};
    std::atomic<unsigned long long> processedGeneration{0};
    std::atomic<UINT_PTR> mainWindow{0};
    std::atomic<bool> desiredGraph{false};
    std::mutex statusMutex;
    std::wstring status = L"Native Graph bridge is idle";
    std::wstring performance = L"Native Graph timing is unavailable";
    std::mutex lifecycleMutex;
};

inline State& GetState() {
    // Intentionally process-lifetime storage. If UI Automation is blocked while
    // AE unloads the plugin, bounded Shutdown() can return without invalidating
    // memory still referenced by the worker.
    static State* state = new State();
    return *state;
}

inline void SetStatus(State& state, bool failed, const std::wstring& text) {
    state.failed.store(failed, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state.statusMutex);
    state.status = text;
}

inline std::wstring ElementName(IUIAutomationElement* element) {
    BSTR value = nullptr;
    if (FAILED(element->get_CurrentName(&value)) || !value) {
        return {};
    }
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

inline bool HasVisibleRect(IUIAutomationElement* element) {
    BOOL offscreen = TRUE;
    RECT rect{};
    return SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) && !offscreen &&
        SUCCEEDED(element->get_CurrentBoundingRectangle(&rect)) &&
        rect.right > rect.left && rect.bottom > rect.top;
}

inline bool SameProcessWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

inline bool ForegroundBelongsTo(HWND mainWindow) {
    if (!SameProcessWindow(mainWindow) || !IsWindowVisible(mainWindow)) {
        return false;
    }
    HWND foreground = GetForegroundWindow();
    if (!SameProcessWindow(foreground)) {
        return false;
    }
    if (foreground == mainWindow || IsChild(mainWindow, foreground) ||
        GetAncestor(foreground, GA_ROOT) == mainWindow ||
        GetAncestor(foreground, GA_ROOTOWNER) == mainWindow) {
        return true;
    }
    for (HWND owner = GetWindow(foreground, GW_OWNER); owner;
         owner = GetWindow(owner, GW_OWNER)) {
        if (owner == mainWindow) {
            return true;
        }
    }
    return false;
}

inline bool ModifiersReleased() {
    constexpr int keys[] = {
        VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, VK_F3,
    };
    for (int key : keys) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            return false;
        }
    }
    return true;
}

inline bool WaitForModifiersReleased(State& state, unsigned long long generation) {
    const ULONGLONG deadline = GetTickCount64() + 400;
    do {
        if (state.stop.load(std::memory_order_acquire) ||
            state.requestedGeneration.load(std::memory_order_acquire) != generation) {
            return false;
        }
        if (ModifiersReleased()) {
            return true;
        }
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

inline HRESULT FindUniqueTimelineFull(IUIAutomation* automation, HWND mainWindow,
                                      IUIAutomationElement** timeline) {
    *timeline = nullptr;
    IUIAutomationElement* root = nullptr;
    HRESULT result = automation->ElementFromHandle(mainWindow, &root);
    if (FAILED(result) || !root) {
        return FAILED(result) ? result : E_FAIL;
    }

    VARIANT nameValue{};
    nameValue.vt = VT_BSTR;
    nameValue.bstrVal = SysAllocString(L"AE Timeline");
    IUIAutomationCondition* nameCondition = nullptr;
    IUIAutomationCondition* paneCondition = nullptr;
    IUIAutomationCondition* combinedCondition = nullptr;
    IUIAutomationElementArray* matches = nullptr;
    if (!nameValue.bstrVal) {
        root->Release();
        return E_OUTOFMEMORY;
    }
    result = automation->CreatePropertyCondition(UIA_NamePropertyId, nameValue,
                                                  &nameCondition);
    VariantClear(&nameValue);
    if (SUCCEEDED(result)) {
        VARIANT paneValue{};
        paneValue.vt = VT_I4;
        paneValue.lVal = UIA_PaneControlTypeId;
        result = automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                                      paneValue, &paneCondition);
    }
    if (SUCCEEDED(result)) {
        result = automation->CreateAndCondition(nameCondition, paneCondition,
                                                &combinedCondition);
    }
    if (SUCCEEDED(result)) {
        result = root->FindAll(TreeScope_Descendants, combinedCondition, &matches);
    }

    int visibleCount = 0;
    if (SUCCEEDED(result) && matches) {
        int length = 0;
        matches->get_Length(&length);
        for (int index = 0; index < length; ++index) {
            IUIAutomationElement* candidate = nullptr;
            if (FAILED(matches->GetElement(index, &candidate)) || !candidate) {
                continue;
            }
            UIA_HWND nativeWindow = 0;
            candidate->get_CurrentNativeWindowHandle(&nativeWindow);
            DWORD pid = 0;
            if (nativeWindow) {
                GetWindowThreadProcessId(reinterpret_cast<HWND>(nativeWindow), &pid);
            }
            if (HasVisibleRect(candidate) && nativeWindow &&
                pid == GetCurrentProcessId()) {
                ++visibleCount;
                if (visibleCount == 1) {
                    *timeline = candidate;
                    candidate = nullptr;
                }
            }
            if (candidate) {
                candidate->Release();
            }
        }
    }
    if (matches) matches->Release();
    if (combinedCondition) combinedCondition->Release();
    if (paneCondition) paneCondition->Release();
    if (nameCondition) nameCondition->Release();
    root->Release();

    if (FAILED(result)) {
        if (*timeline) {
            (*timeline)->Release();
            *timeline = nullptr;
        }
        return result;
    }
    if (visibleCount != 1) {
        if (*timeline) {
            (*timeline)->Release();
            *timeline = nullptr;
        }
        return visibleCount == 0 ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) :
                                   HRESULT_FROM_WIN32(ERROR_DUP_NAME);
    }
    return S_OK;
}

struct NativeWindowCandidates {
    HWND mainWindow = nullptr;
    DWORD processId = 0;
    std::vector<HWND> windows;
};

inline BOOL CALLBACK AddVisibleChildWindow(HWND hwnd, LPARAM contextValue) {
    auto& context = *reinterpret_cast<NativeWindowCandidates*>(contextValue);
    if (IsWindowVisible(hwnd) && SameProcessWindow(hwnd)) {
        context.windows.push_back(hwnd);
    }
    return TRUE;
}

inline bool IsOwnedBy(HWND hwnd, HWND ownerRoot) {
    for (HWND owner = GetWindow(hwnd, GW_OWNER); owner;
         owner = GetWindow(owner, GW_OWNER)) {
        if (owner == ownerRoot) return true;
    }
    return false;
}

inline BOOL CALLBACK AddVisibleOwnedWindow(HWND hwnd, LPARAM contextValue) {
    auto& context = *reinterpret_cast<NativeWindowCandidates*>(contextValue);
    if (hwnd == context.mainWindow || !IsWindowVisible(hwnd) ||
        !SameProcessWindow(hwnd) || !IsOwnedBy(hwnd, context.mainWindow)) {
        return TRUE;
    }
    // This class is registered by our own controls; it cannot contain an AE timeline.
    // Querying its many standard Win32 controls through UIA adds substantial latency.
    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) &&
        wcscmp(className, ControlsWindowClass) == 0) return TRUE;
    context.windows.push_back(hwnd);
    EnumChildWindows(hwnd, AddVisibleChildWindow, contextValue);
    return TRUE;
}

enum class NativeTimelineResult {
    Unique,
    Unresolved,
    Ambiguous,
};

enum class TimelineDiscoveryPath {
    NativeWindows,
    FullTree,
    Ambiguous,
};

inline thread_local TimelineDiscoveryPath g_lastDiscoveryPath =
    TimelineDiscoveryPath::FullTree;

inline NativeTimelineResult FindTimelineFromNativeWindows(
        IUIAutomation* automation, HWND mainWindow,
        IUIAutomationElement** timeline) {
    *timeline = nullptr;
    NativeWindowCandidates context;
    context.mainWindow = mainWindow;
    context.processId = GetCurrentProcessId();
    EnumChildWindows(mainWindow, AddVisibleChildWindow,
                     reinterpret_cast<LPARAM>(&context));
    EnumWindows(AddVisibleOwnedWindow, reinterpret_cast<LPARAM>(&context));
    std::sort(context.windows.begin(), context.windows.end(), [](HWND first, HWND second) {
        return reinterpret_cast<UINT_PTR>(first) < reinterpret_cast<UINT_PTR>(second);
    });
    context.windows.erase(std::unique(context.windows.begin(), context.windows.end()),
                          context.windows.end());

    IUIAutomationCacheRequest* request = nullptr;
    HRESULT result = automation->CreateCacheRequest(&request);
    if (FAILED(result) || !request) return NativeTimelineResult::Unresolved;
    result = request->put_TreeScope(TreeScope_Element);
    for (PROPERTYID property : {
             UIA_NamePropertyId, UIA_ControlTypePropertyId,
             UIA_NativeWindowHandlePropertyId, UIA_ProcessIdPropertyId,
             UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId }) {
        if (SUCCEEDED(result)) result = request->AddProperty(property);
    }
    if (FAILED(result)) {
        request->Release();
        return NativeTimelineResult::Unresolved;
    }

    int matches = 0;
    bool metadataFailed = false;
    for (HWND hwnd : context.windows) {
        IUIAutomationElement* element = nullptr;
        result = automation->ElementFromHandleBuildCache(hwnd, request, &element);
        if (FAILED(result) || !element) {
            metadataFailed = true;
            if (element) element->Release();
            continue;
        }
        BSTR nameValue = nullptr;
        CONTROLTYPEID controlType = 0;
        UIA_HWND nativeWindow = 0;
        int processId = 0;
        BOOL offscreen = TRUE;
        RECT rect{};
        bool valid = SUCCEEDED(element->get_CachedName(&nameValue)) && nameValue &&
            SUCCEEDED(element->get_CachedControlType(&controlType)) &&
            SUCCEEDED(element->get_CachedNativeWindowHandle(&nativeWindow)) &&
            SUCCEEDED(element->get_CachedProcessId(&processId)) &&
            SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) &&
            SUCCEEDED(element->get_CachedBoundingRectangle(&rect));
        if (!valid) metadataFailed = true;
        const bool match = valid && std::wstring(nameValue, SysStringLen(nameValue)) == L"AE Timeline" &&
            controlType == UIA_PaneControlTypeId &&
            reinterpret_cast<HWND>(nativeWindow) == hwnd &&
            processId == static_cast<int>(context.processId) && !offscreen &&
            rect.right > rect.left && rect.bottom > rect.top;
        if (nameValue) SysFreeString(nameValue);
        if (match) {
            ++matches;
            if (matches == 1) {
                *timeline = element;
                element = nullptr;
            }
        }
        if (element) element->Release();
    }
    request->Release();
    if (matches > 1) {
        if (*timeline) { (*timeline)->Release(); *timeline = nullptr; }
        return NativeTimelineResult::Ambiguous;
    }
    if (matches == 1 && !metadataFailed) return NativeTimelineResult::Unique;
    if (*timeline) { (*timeline)->Release(); *timeline = nullptr; }
    return NativeTimelineResult::Unresolved;
}

inline HRESULT FindUniqueTimeline(IUIAutomation* automation, HWND mainWindow,
                                  IUIAutomationElement** timeline) {
    NativeTimelineResult nativeResult =
        FindTimelineFromNativeWindows(automation, mainWindow, timeline);
    if (nativeResult == NativeTimelineResult::Unique) {
        g_lastDiscoveryPath = TimelineDiscoveryPath::NativeWindows;
        return S_OK;
    }
    if (nativeResult == NativeTimelineResult::Ambiguous) {
        g_lastDiscoveryPath = TimelineDiscoveryPath::Ambiguous;
        return HRESULT_FROM_WIN32(ERROR_DUP_NAME);
    }
    g_lastDiscoveryPath = TimelineDiscoveryPath::FullTree;
    return FindUniqueTimelineFull(automation, mainWindow, timeline);
}

inline bool ValidateTimeline(IUIAutomation* automation, IUIAutomationElement* timeline) {
    if (!automation || !timeline) return false;
    IUIAutomationCacheRequest* request = nullptr;
    IUIAutomationElement* refreshed = nullptr;
    HRESULT result = automation->CreateCacheRequest(&request);
    if (SUCCEEDED(result) && !request) result = E_FAIL;
    if (SUCCEEDED(result)) result = request->put_TreeScope(TreeScope_Element);
    for (PROPERTYID property : { UIA_NamePropertyId, UIA_ControlTypePropertyId,
             UIA_NativeWindowHandlePropertyId, UIA_ProcessIdPropertyId,
             UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId }) {
        if (SUCCEEDED(result)) result = request->AddProperty(property);
    }
    if (SUCCEEDED(result)) result = timeline->BuildUpdatedCache(request, &refreshed);
    if (request) request->Release();
    BSTR name = nullptr;
    CONTROLTYPEID type = 0;
    UIA_HWND window = nullptr;
    int pid = 0;
    BOOL offscreen = TRUE;
    RECT rect{};
    const bool valid = SUCCEEDED(result) && refreshed &&
        SUCCEEDED(refreshed->get_CachedName(&name)) && name &&
        std::wstring(name, SysStringLen(name)) == L"AE Timeline" &&
        SUCCEEDED(refreshed->get_CachedControlType(&type)) && type == UIA_PaneControlTypeId &&
        SUCCEEDED(refreshed->get_CachedNativeWindowHandle(&window)) && SameProcessWindow(HWND(window)) &&
        IsWindowVisible(HWND(window)) &&
        SUCCEEDED(refreshed->get_CachedProcessId(&pid)) && DWORD(pid) == GetCurrentProcessId() &&
        SUCCEEDED(refreshed->get_CachedIsOffscreen(&offscreen)) && !offscreen &&
        SUCCEEDED(refreshed->get_CachedBoundingRectangle(&rect)) &&
        rect.right > rect.left && rect.bottom > rect.top;
    if (name) SysFreeString(name);
    if (refreshed) refreshed->Release();
    return valid;
}

inline GraphState ReadGraphState(IUIAutomation* automation,
                                 IUIAutomationElement* timeline) {
    // Retain COM identity only; refresh pane metadata and markers at every proof point.
    if (!ValidateTimeline(automation, timeline)) return GraphState::Ambiguous;
    IUIAutomationCondition* graphCondition = nullptr;
    IUIAutomationCondition* zoomCondition = nullptr;
    IUIAutomationCondition* markerCondition = nullptr;
    IUIAutomationCacheRequest* request = nullptr;
    IUIAutomationElementArray* descendants = nullptr;
    auto createNameCondition = [&](const wchar_t* name,
                                   IUIAutomationCondition** condition) {
        VARIANT value{};
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocString(name);
        if (!value.bstrVal) return E_OUTOFMEMORY;
        HRESULT conditionResult = automation->CreatePropertyCondition(
            UIA_NamePropertyId, value, condition);
        VariantClear(&value);
        return conditionResult;
    };
    HRESULT result = createNameCondition(L"Choose graph type and options",
                                         &graphCondition);
    if (SUCCEEDED(result)) {
        result = createNameCondition(L"Auto-zoom graph height", &zoomCondition);
    }
    if (SUCCEEDED(result)) {
        result = automation->CreateOrCondition(graphCondition, zoomCondition,
                                               &markerCondition);
    }
    if (SUCCEEDED(result)) result = automation->CreateCacheRequest(&request);
    if (SUCCEEDED(result) && !request) result = E_FAIL;
    if (SUCCEEDED(result) && request) {
        result = request->put_TreeScope(TreeScope_Element);
    }
    for (PROPERTYID property : {
             UIA_NamePropertyId, UIA_IsOffscreenPropertyId,
             UIA_BoundingRectanglePropertyId }) {
        if (SUCCEEDED(result)) result = request->AddProperty(property);
    }
    if (SUCCEEDED(result)) {
        result = timeline->FindAllBuildCache(TreeScope_Descendants,
                                             markerCondition, request,
                                             &descendants);
    }
    if (request) request->Release();
    if (markerCondition) markerCondition->Release();
    if (zoomCondition) zoomCondition->Release();
    if (graphCondition) graphCondition->Release();
    if (FAILED(result) || !descendants) {
        if (descendants) descendants->Release();
        return GraphState::Ambiguous;
    }

    bool graphOptions = false;
    bool autoZoom = false;
    int length = 0;
    if (FAILED(descendants->get_Length(&length))) {
        descendants->Release();
        return GraphState::Ambiguous;
    }
    for (int index = 0; index < length; ++index) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(descendants->GetElement(index, &element)) || !element) {
            descendants->Release();
            return GraphState::Ambiguous;
        }
        BSTR nameValue = nullptr;
        BOOL offscreen = TRUE;
        RECT rect{};
        bool metadataValid = SUCCEEDED(element->get_CachedName(&nameValue)) && nameValue &&
            SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) &&
            SUCCEEDED(element->get_CachedBoundingRectangle(&rect));
        if (!metadataValid) {
            if (nameValue) SysFreeString(nameValue);
            element->Release();
            descendants->Release();
            return GraphState::Ambiguous;
        }
        if (!offscreen && rect.right > rect.left && rect.bottom > rect.top) {
            const std::wstring name(nameValue, SysStringLen(nameValue));
            graphOptions = graphOptions || name == L"Choose graph type and options";
            autoZoom = autoZoom || name == L"Auto-zoom graph height";
        }
        if (nameValue) SysFreeString(nameValue);
        element->Release();
    }
    descendants->Release();
    if (graphOptions && autoZoom) {
        return GraphState::GraphEditor;
    }
    if (!graphOptions && !autoZoom) {
        return GraphState::LayerBars;
    }
    return GraphState::Ambiguous;
}

inline bool IsTimelineFocused(IUIAutomation* automation,
                              IUIAutomationElement* timeline) {
    IUIAutomationElement* focused = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(automation->GetFocusedElement(&focused)) || !focused ||
        FAILED(automation->get_RawViewWalker(&walker)) || !walker) {
        if (walker) walker->Release();
        if (focused) focused->Release();
        return false;
    }
    bool found = false;
    IUIAutomationElement* current = focused;
    for (int depth = 0; current && depth < 64; ++depth) {
        BOOL same = FALSE;
        if (SUCCEEDED(automation->CompareElements(current, timeline, &same)) && same) {
            found = true;
            break;
        }
        IUIAutomationElement* parent = nullptr;
        walker->GetParentElement(current, &parent);
        if (current != focused) current->Release();
        current = parent;
    }
    if (current && current != focused) current->Release();
    walker->Release();
    focused->Release();
    return found;
}

inline bool SendGraphShortcut() {
    INPUT input[4]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_SHIFT;
    input[1].type = INPUT_KEYBOARD;
    input[1].ki.wVk = VK_F3;
    input[2].type = INPUT_KEYBOARD;
    input[2].ki.wVk = VK_F3;
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].type = INPUT_KEYBOARD;
    input[3].ki.wVk = VK_SHIFT;
    input[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(static_cast<UINT>(std::size(input)), input,
                     sizeof(INPUT)) == std::size(input);
}

inline double PerformanceNowMs() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? 1000.0 * counter.QuadPart / frequency.QuadPart : 0.0;
}

struct PerformanceMetrics {
    double discoveryMs = 0;
    double stateMs = 0;
    double modifierMs = 0;
    double focusMs = 0;
    double confirmationMs = 0;
    double totalMs = 0;
    int resolutions = 0;
    int polls = 0;
    int nativeDiscoveries = 0;
    int fullDiscoveries = 0;
};

struct PerformanceTimer {
    double& elapsed;
    double started = PerformanceNowMs();
    ~PerformanceTimer() { elapsed += PerformanceNowMs() - started; }
};

struct ElementReference {
    IUIAutomationElement* element = nullptr;
    explicit ElementReference(IUIAutomationElement* value) : element(value) {
        if (element) element->AddRef();
    }
    ~ElementReference() { if (element) element->Release(); }
    ElementReference(const ElementReference&) = delete;
    ElementReference& operator=(const ElementReference&) = delete;
};

inline GraphState ReadGraphStateMeasured(IUIAutomation* automation,
                                         IUIAutomationElement* timeline,
                                         PerformanceMetrics* metrics) {
    if (metrics) {
        PerformanceTimer timer(metrics->stateMs);
        return ReadGraphState(automation, timeline);
    }
    return ReadGraphState(automation, timeline);
}

inline std::wstring FormatPerformance(const PerformanceMetrics& metrics,
                                      bool desiredGraph) {
    wchar_t text[384]{};
    swprintf_s(text,
        L"Native Graph %s timing: total=%.1fms discovery=%.1fms state=%.1fms "
        L"modifiers=%.1fms focus=%.1fms confirm=%.1fms resolves=%d polls=%d "
        L"native=%d fallback=%d",
        desiredGraph ? L"OPEN" : L"CLOSE", metrics.totalMs,
        metrics.discoveryMs, metrics.stateMs, metrics.modifierMs,
        metrics.focusMs, metrics.confirmationMs, metrics.resolutions,
        metrics.polls, metrics.nativeDiscoveries, metrics.fullDiscoveries);
    return text;
}

inline bool ResolveAndRead(IUIAutomation* automation, HWND mainWindow,
                           GraphState& graphState,
                           IUIAutomationElement** timeline = nullptr,
                           PerformanceMetrics* metrics = nullptr) {
    IUIAutomationElement* foundTimeline = nullptr;
    HRESULT findResult = E_FAIL;
    if (metrics) {
        ++metrics->resolutions;
        PerformanceTimer timer(metrics->discoveryMs);
        findResult = FindUniqueTimeline(automation, mainWindow, &foundTimeline);
    } else {
        findResult = FindUniqueTimeline(automation, mainWindow, &foundTimeline);
    }
    if (metrics) {
        if (g_lastDiscoveryPath == TimelineDiscoveryPath::NativeWindows)
            ++metrics->nativeDiscoveries;
        else if (g_lastDiscoveryPath == TimelineDiscoveryPath::FullTree)
            ++metrics->fullDiscoveries;
    }
    if (FAILED(findResult) || !foundTimeline) {
        return false;
    }
    graphState = ReadGraphStateMeasured(automation, foundTimeline, metrics);
    if (graphState == GraphState::Ambiguous) {
        foundTimeline->Release();
        if (timeline) {
            *timeline = nullptr;
        }
        return false;
    }
    if (timeline) {
        *timeline = foundTimeline;
    } else {
        foundTimeline->Release();
    }
    return true;
}

inline bool ProcessRequest(State& state, IUIAutomation* automation,
                           unsigned long long generation, HWND mainWindow,
                           bool desiredGraph, std::wstring& resultStatus,
                           PerformanceMetrics* metrics = nullptr) {
    if (!SameProcessWindow(mainWindow) || !IsWindowVisible(mainWindow)) {
        resultStatus = L"AE main window is unavailable";
        return false;
    }
    if (!ForegroundBelongsTo(mainWindow)) {
        resultStatus = L"AE is not the foreground application";
        return false;
    }

    GraphState current = GraphState::Ambiguous;
    IUIAutomationElement* timeline = nullptr;
    if (!ResolveAndRead(automation, mainWindow, current, &timeline, metrics)) {
        resultStatus = L"Visible AE Timeline or Graph Editor state is ambiguous";
        return false;
    }
    const GraphState desired = desiredGraph ? GraphState::GraphEditor :
                                              GraphState::LayerBars;
    if (current == desired) {
        timeline->Release();
        resultStatus = desiredGraph ? L"Graph Editor is open" :
                                      L"Layer bars are open";
        return true;
    }
    // Resolve uniqueness once per generation. Focus and SendInput may mutate the
    // subtree, so graph markers are still queried fresh at every proof point.
    ElementReference requestTimeline(timeline);
    if (state.requestedGeneration.load(std::memory_order_acquire) != generation) {
        timeline->Release();
        resultStatus = L"Request superseded";
        return true;
    }
    bool modifiersReleased = false;
    if (metrics) {
        PerformanceTimer timer(metrics->modifierMs);
        modifiersReleased = WaitForModifiersReleased(state, generation);
    } else {
        modifiersReleased = WaitForModifiersReleased(state, generation);
    }
    if (!modifiersReleased) {
        timeline->Release();
        resultStatus = L"Shortcut modifiers are held or request was superseded";
        return false;
    }
    bool focusVerified = false;
    if (metrics) {
        PerformanceTimer timer(metrics->focusMs);
        focusVerified = SUCCEEDED(timeline->SetFocus()) &&
            IsTimelineFocused(automation, timeline) &&
            ForegroundBelongsTo(mainWindow);
    } else {
        focusVerified = SUCCEEDED(timeline->SetFocus()) &&
            IsTimelineFocused(automation, timeline) &&
            ForegroundBelongsTo(mainWindow);
    }
    if (!focusVerified) {
        timeline->Release();
        resultStatus = L"AE Timeline focus could not be verified";
        return false;
    }
    timeline->Release();
    if (state.requestedGeneration.load(std::memory_order_acquire) != generation ||
        !ModifiersReleased()) {
        resultStatus = L"Request or keyboard state changed before shortcut";
        return false;
    }

    // Focus may itself change the accessible tree; re-read before emitting input.
    current = ReadGraphStateMeasured(automation, requestTimeline.element, metrics);
    if (current == GraphState::Ambiguous) {
        resultStatus = L"Graph Editor state became ambiguous after focus";
        return false;
    }
    if (current == desired) {
        resultStatus = desiredGraph ? L"Graph Editor is open" :
                                       L"Layer bars are open";
        return true;
    }
    bool focusStillVerified = false;
    if (metrics) {
        PerformanceTimer timer(metrics->focusMs);
        focusStillVerified = IsTimelineFocused(automation, requestTimeline.element);
    } else {
        focusStillVerified = IsTimelineFocused(automation, requestTimeline.element);
    }
    UIA_HWND timelineWindow = nullptr;
    const bool targetForeground = SUCCEEDED(requestTimeline.element->get_CurrentNativeWindowHandle(&timelineWindow)) &&
        SameProcessWindow(HWND(timelineWindow)) &&
        GetAncestor(HWND(timelineWindow), GA_ROOT) == GetForegroundWindow();
    if (!focusStillVerified || !targetForeground || !ForegroundBelongsTo(mainWindow) ||
        state.stop.load(std::memory_order_acquire) ||
        state.requestedGeneration.load(std::memory_order_acquire) != generation ||
        !ModifiersReleased()) {
        resultStatus = L"Focus, foreground, request, or keyboard state changed before shortcut";
        return false;
    }
    double unusedConfirmation = 0;
    PerformanceTimer confirmationTimer(metrics ? metrics->confirmationMs : unusedConfirmation);
    if (!SendGraphShortcut()) {
        resultStatus = L"Shift+F3 could not be sent";
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + 1200;
    do {
        if (state.stop.load(std::memory_order_acquire)) {
            resultStatus = L"Bridge is shutting down";
            return false;
        }
        if (state.requestedGeneration.load(std::memory_order_acquire) != generation) {
            resultStatus = L"Request superseded";
            return true;
        }
        Sleep(40);
        if (metrics) ++metrics->polls;
        if (!ForegroundBelongsTo(mainWindow)) {
            resultStatus = L"AE lost foreground ownership after shortcut";
            return false;
        }
        current = ReadGraphStateMeasured(automation, requestTimeline.element, metrics);
        if (current == desired) {
            resultStatus = desiredGraph ? L"Graph Editor opened" :
                                          L"Layer bars opened";
            return true;
        }
    } while (GetTickCount64() < deadline);
    resultStatus = L"AE did not reach the requested Timeline state";
    return false;
}

inline DWORD WINAPI WorkerMain(void* context) {
    State& state = *static_cast<State*>(context);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr;
    HRESULT automationResult = E_FAIL;
    if (SUCCEEDED(comResult)) {
        CoEnableCallCancellation(nullptr);
        automationResult = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&automation));
    }
    if (FAILED(comResult) || FAILED(automationResult) || !automation) {
        SetStatus(state, true, L"UI Automation worker initialization failed");
    }

    while (!state.stop.load(std::memory_order_acquire)) {
        WaitForSingleObject(state.wakeEvent, INFINITE);
        if (state.stop.load(std::memory_order_acquire)) {
            break;
        }
        unsigned long long generation = 0;
        HWND mainWindow = nullptr;
        bool desiredGraph = false;
        {
            std::lock_guard<std::mutex> lock(state.statusMutex);
            generation = state.requestedGeneration.load(std::memory_order_acquire);
            if (generation == state.processedGeneration.load(std::memory_order_acquire)) continue;
            mainWindow = reinterpret_cast<HWND>(state.mainWindow.load(std::memory_order_acquire));
            desiredGraph = state.desiredGraph.load(std::memory_order_acquire);
            state.processing.store(true, std::memory_order_release);
        }
        std::wstring status;
        bool ok = false;
        PerformanceMetrics performance;
        const double requestStarted = PerformanceNowMs();
        if (automation) {
            ok = ProcessRequest(state, automation, generation, mainWindow,
                                desiredGraph, status, &performance);
        } else {
            status = L"UI Automation worker is unavailable";
        }
        performance.totalMs = PerformanceNowMs() - requestStarted;
        const std::wstring performanceText =
            FormatPerformance(performance, desiredGraph);
        {
            std::lock_guard<std::mutex> lock(state.statusMutex);
            if (generation == state.requestedGeneration.load(std::memory_order_acquire)) {
                state.failed.store(!ok, std::memory_order_release);
                state.status = status;
                state.performance = performanceText;
            } else SetEvent(state.wakeEvent);
            // Outcome and completion belong to one generation. Queue uses the
            // same lock, so an older result cannot overwrite a newer request.
            state.processedGeneration.store(generation, std::memory_order_release);
            state.processing.store(false, std::memory_order_release);
        }
    }

    if (automation) automation->Release();
    if (SUCCEEDED(comResult)) {
        CoDisableCallCancellation(nullptr);
        CoUninitialize();
    }
    HMODULE modulePin = state.modulePin;
    if (modulePin) {
        FreeLibraryAndExitThread(modulePin, 0);
    }
    return 0;
}

inline bool EnsureWorker(State& state) {
    std::lock_guard<std::mutex> lock(state.lifecycleMutex);
    if (state.shutdownStarted.load(std::memory_order_acquire)) {
        SetStatus(state, true, L"Native Graph bridge has shut down");
        return false;
    }
    if (state.worker) {
        return true;
    }
    state.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state.wakeEvent) {
        SetStatus(state, true, L"Could not create Native Graph worker event");
        return false;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&WorkerMain), &state.modulePin)) {
        CloseHandle(state.wakeEvent); state.wakeEvent = nullptr;
        SetStatus(state, true, L"Could not retain Native Graph worker module");
        return false;
    }
    state.worker = CreateThread(nullptr, 0, WorkerMain, &state, 0, &state.workerId);
    if (!state.worker) {
        if (state.modulePin) {
            FreeLibrary(state.modulePin);
            state.modulePin = nullptr;
        }
        CloseHandle(state.wakeEvent);
        state.wakeEvent = nullptr;
        SetStatus(state, true, L"Could not create Native Graph worker thread");
        return false;
    }
    return true;
}

inline bool Queue(HWND mainWindow, bool desiredGraph) {
    State& state = GetState();
    if (!SameProcessWindow(mainWindow) || !IsWindowVisible(mainWindow)) {
        SetStatus(state, true, L"Invalid AE main window");
        return false;
    }
    if (!EnsureWorker(state)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state.statusMutex);
        state.mainWindow.store(reinterpret_cast<UINT_PTR>(mainWindow), std::memory_order_release);
        state.desiredGraph.store(desiredGraph, std::memory_order_release);
        state.failed.store(false, std::memory_order_release);
        state.status = desiredGraph ? L"Opening Graph Editor" : L"Opening layer bars";
        state.requestedGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    SetEvent(state.wakeEvent);
    return true;
}

}  // namespace detail

inline bool Open(HWND aeMainWindow) {
    return detail::Queue(aeMainWindow, true);
}

inline bool Close() {
    detail::State& state = detail::GetState();
    HWND mainWindow = reinterpret_cast<HWND>(
        state.mainWindow.load(std::memory_order_acquire));
    if (!mainWindow) {
        detail::SetStatus(state, true, L"No AE main window has been registered");
        return false;
    }
    return detail::Queue(mainWindow, false);
}

inline void Shutdown() {
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.lifecycleMutex);
    if (!state.worker || state.shutdownStarted.exchange(true)) {
        return;
    }
    state.stop.store(true, std::memory_order_release);
    SetEvent(state.wakeEvent);
    if (GetCurrentThreadId() == state.workerId) {
        return;
    }

    DWORD waitResult = WaitForSingleObject(state.worker, 250);
    if (waitResult == WAIT_TIMEOUT) {
        CoCancelCall(state.workerId, 0);
        waitResult = WaitForSingleObject(state.worker, 1250);
    }
    if (waitResult == WAIT_OBJECT_0) {
        CloseHandle(state.worker);
        state.worker = nullptr;
        CloseHandle(state.wakeEvent);
        state.wakeEvent = nullptr;
        detail::SetStatus(state, false, L"Native Graph bridge shut down");
    } else {
        // Worker owns a module reference, so returning here cannot unload code
        // underneath a UIA call. Storage and handles intentionally remain valid.
        detail::SetStatus(state, true,
                          L"Native Graph worker shutdown was deferred");
    }
}

inline bool Busy() {
    detail::State& state = detail::GetState();
    return state.processing.load(std::memory_order_acquire) ||
        state.processedGeneration.load(std::memory_order_acquire) !=
            state.requestedGeneration.load(std::memory_order_acquire);
}

inline bool Failed() {
    return detail::GetState().failed.load(std::memory_order_acquire);
}

inline std::wstring Status() {
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.statusMutex);
    return state.status;
}

inline std::wstring PerformanceStatus() {
    detail::State& state = detail::GetState();
    std::lock_guard<std::mutex> lock(state.statusMutex);
    return state.performance;
}

}  // namespace NativeGraphBridge

namespace native_graph = NativeGraphBridge;
