#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../DalimaoCurves/DalimaoCurves.cpp"

namespace {

struct FakeKey {
    double compTime = 0.0;
    double layerTime = 0.0;
    std::array<double, 3> value{};
    std::array<AEGP_KeyframeEase, 3> in{};
    std::array<AEGP_KeyframeEase, 3> out{};
    AEGP_KeyframeInterpolationType inType = AEGP_KeyInterp_BEZIER;
    AEGP_KeyframeInterpolationType outType = AEGP_KeyInterp_BEZIER;
    AEGP_KeyframeFlags flags = 0;
};

struct FakeAE {
    std::vector<FakeKey> keys;
    int dimensions = 2;
    int temporalDimensions = 2;
    int startUndo = 0;
    int endUndo = 0;
    int writes = 0;
    int countCalls = 0;
    int failCountOnCall = -1;
    int failTimeIndex = -1;
    int failEaseIndex = -1;
    int failEaseDimension = -1;
    bool failEaseOnce = false;
};

FakeAE fake;
AEGP_KeyframeSuite5 keyframeSuite{};
AEGP_StreamSuite6 streamSuite{};
AEGP_UtilitySuite6 utilitySuite{};
SPBasicSuite basicSuite{};
int failures = 0;
int suiteAcquisitions = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

bool Near(double actual, double expected) {
    return std::abs(actual - expected) <= 1.0e-9 * (1.0 + std::abs(expected));
}

FakeKey& Key(AEGP_KeyframeIndex index) {
    return fake.keys.at(static_cast<std::size_t>(index));
}

A_Err SPAPI GetStreamNumKFs(AEGP_StreamRefH, A_long* count) {
    ++fake.countCalls;
    if (fake.countCalls == fake.failCountOnCall) return 1;
    *count = static_cast<A_long>(fake.keys.size());
    return A_Err_NONE;
}

A_Err SPAPI GetKeyframeTime(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    AEGP_LTimeMode mode,
    A_Time* time) {
    if (index == fake.failTimeIndex) return 1;
    const double seconds = mode == AEGP_LTimeMode_LayerTime ? Key(index).layerTime : Key(index).compTime;
    time->scale = 1000000;
    time->value = static_cast<A_long>(seconds * time->scale);
    return A_Err_NONE;
}

A_Err SPAPI GetNewKeyframeValue(
    AEGP_PluginID,
    AEGP_StreamRefH stream,
    AEGP_KeyframeIndex index,
    AEGP_StreamValue2* value) {
    value->streamH = stream;
    value->val.two_d.x = Key(index).value[0];
    value->val.two_d.y = Key(index).value[1];
    return A_Err_NONE;
}

A_Err SPAPI GetValueDimensionality(AEGP_StreamRefH, A_short* dimensions) {
    *dimensions = static_cast<A_short>(fake.dimensions);
    return A_Err_NONE;
}

A_Err SPAPI GetTemporalDimensionality(AEGP_StreamRefH, A_short* dimensions) {
    *dimensions = static_cast<A_short>(fake.temporalDimensions);
    return A_Err_NONE;
}

A_Err SPAPI GetTemporalEase(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    A_long dimension,
    AEGP_KeyframeEase* in,
    AEGP_KeyframeEase* out) {
    *in = Key(index).in.at(static_cast<std::size_t>(dimension));
    *out = Key(index).out.at(static_cast<std::size_t>(dimension));
    return A_Err_NONE;
}

A_Err SPAPI SetTemporalEase(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    A_long dimension,
    const AEGP_KeyframeEase* in,
    const AEGP_KeyframeEase* out) {
    ++fake.writes;
    if (fake.failEaseOnce && index == fake.failEaseIndex && dimension == fake.failEaseDimension) {
        fake.failEaseOnce = false;
        return 1;
    }
    Key(index).in.at(static_cast<std::size_t>(dimension)) = *in;
    Key(index).out.at(static_cast<std::size_t>(dimension)) = *out;
    return A_Err_NONE;
}

A_Err SPAPI GetKeyframeFlags(AEGP_StreamRefH, AEGP_KeyframeIndex index, AEGP_KeyframeFlags* flags) {
    *flags = Key(index).flags;
    return A_Err_NONE;
}

A_Err SPAPI SetKeyframeFlag(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    AEGP_KeyframeFlags flag,
    A_Boolean enabled) {
    ++fake.writes;
    if (enabled) Key(index).flags |= flag;
    else Key(index).flags &= ~flag;
    return A_Err_NONE;
}

A_Err SPAPI GetInterpolation(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    AEGP_KeyframeInterpolationType* in,
    AEGP_KeyframeInterpolationType* out) {
    *in = Key(index).inType;
    *out = Key(index).outType;
    return A_Err_NONE;
}

A_Err SPAPI SetInterpolation(
    AEGP_StreamRefH,
    AEGP_KeyframeIndex index,
    AEGP_KeyframeInterpolationType in,
    AEGP_KeyframeInterpolationType out) {
    ++fake.writes;
    Key(index).inType = in;
    Key(index).outType = out;
    return A_Err_NONE;
}

A_Err SPAPI DisposeStreamValue(AEGP_StreamValue2*) {
    return A_Err_NONE;
}

A_Err SPAPI GetStreamProperties(AEGP_StreamRefH, AEGP_StreamFlags* flags, A_FpLong* minimum, A_FpLong* maximum) {
    *flags = 0;
    *minimum = 0.0;
    *maximum = 0.0;
    return A_Err_NONE;
}

A_Err SPAPI StartUndoGroup(const A_char*) {
    ++fake.startUndo;
    return A_Err_NONE;
}

A_Err SPAPI EndUndoGroup() {
    ++fake.endUndo;
    return A_Err_NONE;
}

SPErr SPAPI AcquireSuite(const char* name, int32 version, const void** suite) {
    ++suiteAcquisitions;
    if (std::strcmp(name, kAEGPKeyframeSuite) == 0 && version == kAEGPKeyframeSuiteVersion5) {
        *suite = &keyframeSuite;
    } else if (std::strcmp(name, kAEGPStreamSuite) == 0 && version == kAEGPStreamSuiteVersion6) {
        *suite = &streamSuite;
    } else if (std::strcmp(name, kAEGPUtilitySuite) == 0 && version == kAEGPUtilitySuiteVersion6) {
        *suite = &utilitySuite;
    } else {
        *suite = nullptr;
        return 1;
    }
    return 0;
}

SPErr SPAPI ReleaseSuite(const char*, int32) {
    return 0;
}

void InitializeSuites() {
    keyframeSuite = {};
    keyframeSuite.AEGP_GetStreamNumKFs = GetStreamNumKFs;
    keyframeSuite.AEGP_GetKeyframeTime = GetKeyframeTime;
    keyframeSuite.AEGP_GetNewKeyframeValue = GetNewKeyframeValue;
    keyframeSuite.AEGP_GetStreamValueDimensionality = GetValueDimensionality;
    keyframeSuite.AEGP_GetStreamTemporalDimensionality = GetTemporalDimensionality;
    keyframeSuite.AEGP_GetKeyframeTemporalEase = GetTemporalEase;
    keyframeSuite.AEGP_SetKeyframeTemporalEase = SetTemporalEase;
    keyframeSuite.AEGP_GetKeyframeFlags = GetKeyframeFlags;
    keyframeSuite.AEGP_SetKeyframeFlag = SetKeyframeFlag;
    keyframeSuite.AEGP_GetKeyframeInterpolation = GetInterpolation;
    keyframeSuite.AEGP_SetKeyframeInterpolation = SetInterpolation;

    streamSuite = {};
    streamSuite.AEGP_DisposeStreamValue = DisposeStreamValue;
    streamSuite.AEGP_GetStreamProperties = GetStreamProperties;

    utilitySuite = {};
    utilitySuite.AEGP_StartUndoGroup = StartUndoGroup;
    utilitySuite.AEGP_EndUndoGroup = EndUndoGroup;

    basicSuite = {};
    basicSuite.AcquireSuite = AcquireSuite;
    basicSuite.ReleaseSuite = ReleaseSuite;
}

FakeKey MakeKey(double time, double x, double y, double seed) {
    FakeKey key;
    key.compTime = time;
    key.layerTime = time;
    key.value = {x, y, 0.0};
    for (int dimension = 0; dimension < 3; ++dimension) {
        key.in[dimension] = {seed + dimension + 1.0, 0.20 + dimension * 0.05};
        key.out[dimension] = {seed + dimension + 11.0, 0.60 + dimension * 0.05};
    }
    key.flags = AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER | AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS;
    return key;
}

void ResetFixture() {
    fake = {};
    fake.keys = {
        MakeKey(1.0, 10.0, 100.0, 10.0),
        MakeKey(3.0, 30.0, 160.0, 30.0),
        MakeKey(6.0, 90.0, 250.0, 50.0),
    };
    g_sp = &basicSuite;
    g_plugin_id = 7;
    g_plugin_dir.clear();
    g_props.clear();
    PropertyInfo property;
    property.streamH = reinterpret_cast<AEGP_StreamRefH>(&fake);
    property.type = AEGP_StreamType_TwoD;
    property.numDims = 2;
    property.temporalDims = 2;
    property.numKFs = static_cast<int>(fake.keys.size());
    g_props.push_back(property);
    g_panel = {};
    g_panel.curProp = 0;
    g_panel.presetSegment = 0;
    g_panel.presetDim = 1;
    ReloadKeyframes();
    fake.writes = 0;
}

bool SameEase(const AEGP_KeyframeEase& first, const AEGP_KeyframeEase& second) {
    return first.speedF == second.speedF && first.influenceF == second.influenceF;
}

bool SameKey(const FakeKey& first, const FakeKey& second) {
    if (first.compTime != second.compTime || first.layerTime != second.layerTime ||
        first.value != second.value || first.inType != second.inType ||
        first.outType != second.outType || first.flags != second.flags) return false;
    for (int dimension = 0; dimension < 3; ++dimension) {
        if (!SameEase(first.in[dimension], second.in[dimension]) ||
            !SameEase(first.out[dimension], second.out[dimension])) return false;
    }
    return true;
}

void TestAsymmetricAndExtremeInfluences() {
    ResetFixture();
    const auto original = fake.keys;
    ApplySegmentPreset(curve_presets::Builtins()[2]);
    Check(Near(fake.keys[0].out[1].influenceF, 1.0), "100 percent outgoing influence becomes 1.0");
    Check(Near(fake.keys[1].in[1].influenceF, 0.001), "0.1 percent incoming influence becomes 0.001");
    Check(fake.keys[0].out[1].speedF == 0.0 && fake.keys[1].in[1].speedF == 0.0,
        "slow-start preset applies zero endpoint speeds");
    Check(SameEase(fake.keys[0].in[1], original[0].in[1]) &&
        SameEase(fake.keys[1].out[1], original[1].out[1]), "opposite ease sides are preserved");
    Check(SameEase(fake.keys[0].in[0], original[0].in[0]) &&
        SameEase(fake.keys[0].out[0], original[0].out[0]) &&
        SameEase(fake.keys[1].in[0], original[1].in[0]) &&
        SameEase(fake.keys[1].out[0], original[1].out[0]), "nonselected dimension is preserved");
    Check(fake.keys[0].value == original[0].value && fake.keys[1].value == original[1].value &&
        fake.keys[0].compTime == original[0].compTime && fake.keys[1].compTime == original[1].compTime,
        "endpoint values and times are preserved");
    Check((fake.keys[0].flags & (AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER |
        AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS)) == 0 &&
        (fake.keys[1].flags & (AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER |
        AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS)) == 0, "automatic and continuous flags are cleared");
    Check(fake.startUndo == 1 && fake.endUndo == 1, "successful apply balances the undo group");

    ResetFixture();
    ApplySegmentPreset(curve_presets::Builtins()[5]);
    Check(Near(fake.keys[0].out[1].influenceF, 1.0) &&
        Near(fake.keys[1].in[1].influenceF, 1.0), "100/100 preset remains full influence on both sides");
}

void TestFailuresAndRollback() {
    ResetFixture();
    const auto original = fake.keys;
    fake.failTimeIndex = 0;
    ApplySegmentPreset(curve_presets::Builtins()[1]);
    Check(fake.writes == 0 && fake.startUndo == 0 && fake.endUndo == 0,
        "source read failure stops before writes and undo");
    Check(SameKey(fake.keys[0], original[0]) && SameKey(fake.keys[1], original[1]),
        "source read failure preserves endpoints");

    ResetFixture();
    const auto beforeFailure = fake.keys;
    fake.failEaseIndex = 1;
    fake.failEaseDimension = 0;
    fake.failEaseOnce = true;
    ApplySegmentPreset(curve_presets::Builtins()[4]);
    Check(SameKey(fake.keys[0], beforeFailure[0]) && SameKey(fake.keys[1], beforeFailure[1]),
        "second endpoint ease failure rolls back both endpoints and flags");
    Check(fake.startUndo == 1 && fake.endUndo == 1, "failed apply still balances the undo group");
}

void TestRovingAndEqualValues() {
    ResetFixture();
    fake.keys[2].flags |= AEGP_KeyframeFlag_ROVING;
    ApplySegmentPreset(curve_presets::Builtins()[1]);
    Check(fake.writes == 0 && fake.startUndo == 0, "roving key anywhere refuses before writes");

    ResetFixture();
    fake.keys[1].value[1] = fake.keys[0].value[1];
    ReloadKeyframes();
    fake.writes = 0;
    ApplySegmentPreset(curve_presets::Builtins()[1]);
    Check(fake.startUndo == 1 && fake.endUndo == 1, "equal-value zero-speed builtin applies");
    Check(fake.keys[0].out[1].speedF == 0.0 && fake.keys[1].in[1].speedF == 0.0,
        "equal-value builtin writes zero speeds");
    Check(Near(fake.keys[0].out[1].influenceF, 0.33333) &&
        Near(fake.keys[1].in[1].influenceF, 0.33333), "equal-value builtin still writes influence fractions");
}

void TestRefreshFailureAfterWrite() {
    ResetFixture();
    fake.failCountOnCall = fake.countCalls + 2;
    ApplySegmentPreset(curve_presets::Builtins()[1]);
    Check(fake.startUndo == 1 && fake.endUndo == 1 && fake.writes > 0,
        "refresh failure is injected after a completed write and balanced undo");
    Check(g_panel.toast && !g_panel.toastMsg.empty(), "refresh failure raises a visible recovery toast");
    Check(g_panel.presetStatus.find(L"已应用") == std::wstring::npos,
        "refresh failure does not report successful application");
    Check(g_panel.curProp == -1 && g_kfs.empty(), "refresh failure invalidates stale panel keyframe state");
}

void TestHandleSliders() {
    ResetFixture();
    fake.keys[1].value[1] = fake.keys[0].value[1];
    ReloadKeyframes();
    const auto original = fake.keys;
    ApplyHandleInfluences(100.0, 0.1);
    Check(Near(fake.keys[0].out[1].influenceF, 1.0) && Near(fake.keys[1].in[1].influenceF, .001),
        "sliders use the entire supported influence range");
    Check(fake.keys[0].out[1].speedF == original[0].out[1].speedF &&
        fake.keys[1].in[1].speedF == original[1].in[1].speedF,
        "sliders preserve nonzero speeds even when endpoint values are equal");
    Check(SameEase(fake.keys[0].in[1], original[0].in[1]) && SameEase(fake.keys[1].out[1], original[1].out[1]) &&
        SameEase(fake.keys[0].out[0], original[0].out[0]) && SameEase(fake.keys[1].in[0], original[1].in[0]),
        "sliders preserve opposite sides and nonselected dimensions");
    Check(fake.startUndo == 1 && fake.endUndo == 1, "slider gesture commits one balanced undo group");
    ResetFixture();
    ApplyHandleInfluences(0, 50);
    Check(fake.writes == 0 && fake.startUndo == 0, "out-of-range slider influence cannot write");
}

void TestStaleQueuedPair() {
    ResetFixture();
    Action action; action.hasPair = true; action.segment = 0;
    action.keys = {g_kfs[0], g_kfs[1]};
    Check(PairUnchanged(action), "unchanged queued pair is accepted");
    fake.keys[0].out[1].speedF += 1; ReloadKeyframes();
    Check(!PairUnchanged(action), "native AE speed edit invalidates queued operation");
    ResetFixture(); action.keys = {g_kfs[0], g_kfs[1]};
    fake.keys[1].compTime += .1; ReloadKeyframes();
    Check(!PairUnchanged(action), "moved endpoint invalidates queued operation");
    ResetFixture(); action.keys = {g_kfs[0], g_kfs[1]};
    fake.keys.erase(fake.keys.begin()); ReloadKeyframes();
    Check(!PairUnchanged(action), "removed endpoint cannot redirect queued edit to another pair");
}

void TestClosePreservesQueuedEdit() {
    ResetFixture();
    g_pending = {}; g_closeAfterPending = false; g_closing = false; g_transition = false;
    g_displayKeys = g_kfs;
    for (auto& key : g_displayKeys) key.value = {};
    Queue(ActionKind::Preset, 5);
    Queue(ActionKind::Close);
    Check(g_pending.kind == ActionKind::Preset && g_pending.index == 5 && g_closeAfterPending,
        "close waits for the committed edit instead of replacing it");
    Check(g_pending.hasPair && g_pending.keys[0].time == g_kfs[0].time,
        "pending edit retains its exact endpoint snapshot while close waits");
    g_pending = {}; g_closeAfterPending = false; g_displayKeys.clear();
}

void TestAutomaticAnimatedProperty() {
    std::vector<PropertyInfo> properties(4);
    properties[0].numKFs = 1;
    properties[1].numKFs = 6;
    properties[2].numKFs = 0;
    properties[3].numKFs = 2;
    Check(ChooseAnimatedProperty(properties, -1) == 1, "no property selection automatically finds first two-key property");
    Check(ChooseAnimatedProperty(properties, 3) == 3, "explicit eligible property selection takes priority");
    Check(ChooseAnimatedProperty(properties, 0) == 1, "single-key selection falls back to an editable segment");
    Check(ChooseAnimatedProperty(properties, 99) == 1, "invalid selection index cannot escape property bounds");
    properties[1].numKFs = 1; properties[3].numKFs = 1;
    Check(ChooseAnimatedProperty(properties, -1) == -1, "no two-key property leaves editing disabled");
    Check(ChooseAnimatedProperty({}, -1) == -1, "empty layer cannot select a nonexistent property");
}

void TestQuiescentIdleDoesNoHostWork() {
    s_panel_active = true; g_transition = false; g_closing = false;
    g_pending = {}; g_closeAfterPending = false;
    const int before = suiteAcquisitions;
    for (int i = 0; i < 10000; ++i) {
        A_long sleep = 500;
        Check(IdleHook(nullptr, nullptr, &sleep) == A_Err_NONE && sleep == 500,
            "quiescent popup keeps AE idle interval and returns without work");
    }
    Check(suiteAcquisitions == before, "10000 quiescent idle calls perform zero AE suite acquisitions/scans");
    s_panel_active = false;
}

int comboResets = 0, comboSelections = 0;
LRESULT CALLBACK CountComboUpdates(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == CB_RESETCONTENT) ++comboResets;
    if (message == CB_SETCURSEL) ++comboSelections;
    return DefSubclassProc(hwnd, message, wp, lp);
}
void TestUnchangedComboDoesNotRebuild() {
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST,
        0, 0, 200, 100, parent, HMENU(PROP), GetModuleHandleW(nullptr), nullptr);
    Check(parent && combo, "combo regression fixture creates Win32 controls");
    if (!parent || !combo) { if (parent) DestroyWindow(parent); return; }
    HWND previous = g_panel.hwnd; g_panel.hwnd = parent;
    g_comboContents = {}; comboResets = comboSelections = 0;
    SetWindowSubclass(combo, CountComboUpdates, 1, 0);
    for (int i = 0; i < 100; ++i) UpdateCombo(PROP, {L"Opacity", L"Position"}, 1);
    Check(comboResets == 1 && comboSelections == 1, "100 unchanged updates rebuild/select the combo only once");
    UpdateCombo(PROP, {L"Opacity", L"Scale"}, 0);
    Check(comboResets == 2 && comboSelections == 2, "changed contents still refresh the control");
    RemoveWindowSubclass(combo, CountComboUpdates, 1); DestroyWindow(parent);
    g_panel.hwnd = previous; g_comboContents = {};
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void TestPresetStorageWrapper() {
    const auto path = std::filesystem::current_path() / "x64" / "tests" / "preset-storage-test.txt";
    const auto lockPath = std::filesystem::path(path.wstring() + L".lock");
    DeleteFileW(path.c_str());
    DeleteFileW(lockPath.c_str());
    g_presetPath = path.wstring();
    g_presetDisk.clear();
    g_presetLibraryWritable = true;
    g_userPresets.clear();

    auto presets = curve_presets::Builtins();
    presets.resize(2);
    Check(SavePresetLibrary(presets), "preset wrapper saves a valid library");
    const std::string saved = ReadBytes(path);
    std::istringstream parsedInput(saved);
    std::vector<curve_presets::Preset> parsed;
    Check(curve_presets::Read(parsedInput, parsed) && parsed.size() == 2,
        "preset wrapper output is readable and complete");
    Check(g_userPresets.size() == 2 && g_presetDisk == saved,
        "successful save updates the in-memory library and disk snapshot");

    {
        std::ofstream external(path, std::ios::binary | std::ios::trunc);
        external << "external change";
    }
    auto replacement = presets;
    replacement.resize(1);
    Check(!SavePresetLibrary(replacement), "external disk change rejects overwrite");
    Check(ReadBytes(path) == "external change" && g_userPresets.size() == 2,
        "stale-disk rejection preserves disk and in-memory library");

    g_presetDisk = "external change";
    HANDLE heldLock = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    Check(heldLock != INVALID_HANDLE_VALUE, "test acquires preset lock");
    Check(!SavePresetLibrary(replacement), "held preset lock rejects save");
    if (heldLock != INVALID_HANDLE_VALUE) CloseHandle(heldLock);
    Check(ReadBytes(path) == "external change", "lock rejection preserves disk");

    auto invalid = replacement;
    invalid.front().outInfluence = 100.1;
    Check(!SavePresetLibrary(invalid), "invalid preset vector is rejected by wrapper");
    Check(ReadBytes(path) == "external change" && g_userPresets.size() == 2,
        "invalid save preserves disk and in-memory library");

    DeleteFileW(path.c_str());
    DeleteFileW(lockPath.c_str());
}

}  // namespace

int main() {
    InitializeSuites();
    TestAsymmetricAndExtremeInfluences();
    TestFailuresAndRollback();
    TestRovingAndEqualValues();
    TestRefreshFailureAfterWrite();
    TestHandleSliders();
    TestStaleQueuedPair();
    TestClosePreservesQueuedEdit();
    TestAutomaticAnimatedProperty();
    TestQuiescentIdleDoesNoHostWork();
    TestUnchangedComboDoesNotRebuild();
    TestPresetStorageWrapper();
    if (failures != 0) {
        std::cerr << failures << " integration test(s) failed\n";
        return 1;
    }
    std::cout << "All curve preset integration tests passed\n";
    return 0;
}
