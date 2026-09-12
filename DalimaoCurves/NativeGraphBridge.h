#pragma once

#include <windows.h>
#include <unknwn.h>
#include <oaidl.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <objbase.h>

#include <atomic>
#include <iterator>
#include <mutex>
#include <string>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

namespace NativeGraphBridge {
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

inline HRESULT FindUniqueTimeline(IUIAutomation* automation, HWND mainWindow,
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

inline GraphState ReadGraphState(IUIAutomation* automation,
                                 IUIAutomationElement* timeline) {
    IUIAutomationCondition* trueCondition = nullptr;
    IUIAutomationElementArray* descendants = nullptr;
    if (FAILED(automation->CreateTrueCondition(&trueCondition)) || !trueCondition) {
        return GraphState::Ambiguous;
    }
    HRESULT result = timeline->FindAll(TreeScope_Descendants, trueCondition,
                                       &descendants);
    trueCondition->Release();
    if (FAILED(result) || !descendants) {
        if (descendants) descendants->Release();
        return GraphState::Ambiguous;
    }

    bool graphOptions = false;
    bool autoZoom = false;
    int length = 0;
    descendants->get_Length(&length);
    for (int index = 0; index < length; ++index) {
        IUIAutomationElement* element = nullptr;
        if (FAILED(descendants->GetElement(index, &element)) || !element) {
            continue;
        }
        if (HasVisibleRect(element)) {
            const std::wstring name = ElementName(element);
            graphOptions = graphOptions || name == L"Choose graph type and options";
            autoZoom = autoZoom || name == L"Auto-zoom graph height";
        }
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

inline bool ResolveAndRead(IUIAutomation* automation, HWND mainWindow,
                           GraphState& graphState,
                           IUIAutomationElement** timeline = nullptr) {
    IUIAutomationElement* foundTimeline = nullptr;
    if (FAILED(FindUniqueTimeline(automation, mainWindow, &foundTimeline)) ||
        !foundTimeline) {
        return false;
    }
    graphState = ReadGraphState(automation, foundTimeline);
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
                           bool desiredGraph, std::wstring& resultStatus) {
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
    if (!ResolveAndRead(automation, mainWindow, current, &timeline)) {
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
    if (state.requestedGeneration.load(std::memory_order_acquire) != generation) {
        timeline->Release();
        resultStatus = L"Request superseded";
        return true;
    }
    if (!WaitForModifiersReleased(state, generation)) {
        timeline->Release();
        resultStatus = L"Shortcut modifiers are held or request was superseded";
        return false;
    }
    if (FAILED(timeline->SetFocus()) || !IsTimelineFocused(automation, timeline) ||
        !ForegroundBelongsTo(mainWindow)) {
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
    IUIAutomationElement* confirmedTimeline = nullptr;
    if (!ResolveAndRead(automation, mainWindow, current, &confirmedTimeline)) {
        resultStatus = L"Graph Editor state became ambiguous after focus";
        return false;
    }
    if (current == desired) {
        confirmedTimeline->Release();
        resultStatus = desiredGraph ? L"Graph Editor is open" :
                                      L"Layer bars are open";
        return true;
    }
    const bool focusStillVerified =
        IsTimelineFocused(automation, confirmedTimeline);
    confirmedTimeline->Release();
    if (!focusStillVerified || !ForegroundBelongsTo(mainWindow) ||
        state.requestedGeneration.load(std::memory_order_acquire) != generation ||
        !ModifiersReleased()) {
        resultStatus = L"Focus, foreground, request, or keyboard state changed before shortcut";
        return false;
    }
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
        if (!ForegroundBelongsTo(mainWindow)) {
            resultStatus = L"AE lost foreground ownership after shortcut";
            return false;
        }
        if (ResolveAndRead(automation, mainWindow, current) && current == desired) {
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
        if (automation) {
            ok = ProcessRequest(state, automation, generation, mainWindow, desiredGraph, status);
        } else {
            status = L"UI Automation worker is unavailable";
        }
        {
            std::lock_guard<std::mutex> lock(state.statusMutex);
            if (generation == state.requestedGeneration.load(std::memory_order_acquire)) {
                state.failed.store(!ok, std::memory_order_release);
                state.status = status;
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

}  // namespace NativeGraphBridge

namespace native_graph = NativeGraphBridge;
