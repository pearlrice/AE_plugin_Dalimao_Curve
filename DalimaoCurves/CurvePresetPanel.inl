// AE operations are called only from AEGP command/idle callbacks.
static std::vector<curve_presets::Preset> g_userPresets;
static std::wstring g_presetPath;
static std::string g_presetDisk;
static bool g_presetLibraryWritable = false;

static bool ReadPresetFile(std::string& bytes) {
    bytes.clear();
    if (GetFileAttributesW(g_presetPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    std::ifstream file(std::filesystem::path(g_presetPath), std::ios::binary);
    if (!file) return false;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        bytes.append(buffer, (size_t)file.gcount());
        if (bytes.size() > 65536) return false;
    }
    return file.eof();
}

static void LoadPresetLibrary() {
    g_userPresets.clear();
    g_presetLibraryWritable = false;
    g_panel.libraryIndex = 0;
    wchar_t appData[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appData))) {
        g_panel.presetStatus = L"无法定位用户模板目录";
        return;
    }
    std::wstring directory = std::wstring(appData) + L"\\DalimaoCurves";
    if (!CreateDirectoryW(directory.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_panel.presetStatus = L"无法创建用户模板目录";
        return;
    }
    g_presetPath = directory + L"\\curve_presets.txt";
    if (!ReadPresetFile(g_presetDisk)) {
        g_panel.presetStatus = L"模板文件无法读取；已有文件不会被覆盖";
        return;
    }
    if (GetFileAttributesW(g_presetPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::istringstream input(g_presetDisk);
        if (!curve_presets::Read(input, g_userPresets)) {
            g_panel.presetStatus = L"模板文件格式损坏；请备份后修复，当前禁止覆盖";
            return;
        }
    }
    g_presetLibraryWritable = true;
}

static bool SavePresetLibrary(const std::vector<curve_presets::Preset>& next) {
    if (!g_presetLibraryWritable) {
        g_panel.presetStatus = L"模板库不可写；请检查用户目录或模板文件";
        return false;
    }
    // Coordinate multiple AE instances and refuse to overwrite another
    // instance's edits made since this panel opened.
    HANDLE lock = CreateFileW((g_presetPath + L".lock").c_str(), GENERIC_WRITE,
                              0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        g_panel.presetStatus = L"模板库正被占用，请稍后重试";
        return false;
    }
    std::string current;
    bool unchanged = ReadPresetFile(current) && current == g_presetDisk;
    if (!unchanged) {
        CloseHandle(lock);
        g_panel.presetStatus = L"模板库已在别处更新；请重新打开面板后再保存";
        return false;
    }
    std::ostringstream output;
    if (!curve_presets::Write(output, next)) {
        CloseHandle(lock);
        g_panel.presetStatus = L"模板数据无效，未保存";
        return false;
    }
    const std::string bytes = output.str();
    std::wstring temp = g_presetPath + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = false;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, NULL) &&
             written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (ok) ok = MoveFileExW(temp.c_str(), g_presetPath.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok) DeleteFileW(temp.c_str());
    }
    CloseHandle(lock);
    if (!ok) {
        g_panel.presetStatus = L"保存失败，原模板文件保留";
        return false;
    }
    g_presetDisk = bytes;
    g_userPresets = next;
    return true;
}

struct PresetKeyState {
    double compTime = 0, layerTime = 0;
    std::array<double, 3> value{};
    AEGP_KeyframeEase in[3]{}, out[3]{};
    AEGP_KeyframeInterpolationType inType = AEGP_KeyInterp_LINEAR;
    AEGP_KeyframeInterpolationType outType = AEGP_KeyInterp_LINEAR;
    AEGP_KeyframeFlags flags = 0;
};

static bool IsSpatialPresetProperty(const PropertyInfo& pi) {
    return pi.type == AEGP_StreamType_TwoD_SPATIAL || pi.type == AEGP_StreamType_ThreeD_SPATIAL;
}

static bool ReadPresetKey(AEGP_SuiteHandler& suites, const PropertyInfo& pi,
                          int index, PresetKeyState& state) {
    auto* ks = suites.KeyframeSuite5();
    A_Time comp{}, layer{};
    if (ks->AEGP_GetKeyframeTime(pi.streamH, index, AEGP_LTimeMode_CompTime, &comp) ||
        ks->AEGP_GetKeyframeTime(pi.streamH, index, AEGP_LTimeMode_LayerTime, &layer) ||
        ks->AEGP_GetKeyframeFlags(pi.streamH, index, &state.flags) ||
        ks->AEGP_GetKeyframeInterpolation(pi.streamH, index, &state.inType, &state.outType)) return false;
    state.compTime = TimeToSec(comp);
    state.layerTime = TimeToSec(layer);
    AEGP_StreamValue2 value{};
    if (ks->AEGP_GetNewKeyframeValue(g_plugin_id, pi.streamH, index, &value)) return false;
    for (int d = 0; d < pi.numDims; ++d) state.value[d] = GetDimValue(value.val, pi.type, d);
    suites.StreamSuite6()->AEGP_DisposeStreamValue(&value);
    for (int d = 0; d < pi.temporalDims; ++d)
        if (ks->AEGP_GetKeyframeTemporalEase(pi.streamH, index, d, &state.in[d], &state.out[d])) return false;
    return true;
}

static bool ReadPresetSegment(AEGP_SuiteHandler& suites, PresetKeyState (&keys)[2],
                              double& delta, double& duration, int& dim) {
    auto& p = g_panel;
    if (p.curProp < 0 || p.curProp >= (int)g_props.size() ||
        p.presetSegment < 0 || p.presetSegment + 1 >= (int)g_kfs.size()) {
        p.presetStatus = L"当前属性至少需要两个关键帧";
        return false;
    }
    const auto& pi = g_props[p.curProp];
    if (pi.numDims < 1 || pi.numDims > 3 || pi.temporalDims < 1 || pi.temporalDims > 3) {
        p.presetStatus = L"此属性不支持数值曲线模板";
        return false;
    }
    auto* ks = suites.KeyframeSuite5();
    A_long count = 0;
    if (ks->AEGP_GetStreamNumKFs(pi.streamH, &count) || count != (A_long)g_kfs.size()) {
        p.presetStatus = L"关键帧数量已变化，请重新打开面板";
        return false;
    }
    // A temporal edit can redistribute roving keys outside this pair.
    for (int i = 0; i < count; ++i) {
        AEGP_KeyframeFlags flags = 0;
        if (ks->AEGP_GetKeyframeFlags(pi.streamH, i, &flags) || (flags & AEGP_KeyframeFlag_ROVING)) {
            p.presetStatus = L"此属性含游走关键帧或读取失败；请先关闭游走";
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (!ReadPresetKey(suites, pi, p.presetSegment + i, keys[i]) ||
            fabs(keys[i].compTime - g_kfs[p.presetSegment + i].time) > 1e-7) {
            p.presetStatus = L"关键帧已变化或读取失败，请重新打开面板";
            return false;
        }
    }
    dim = IsSpatialPresetProperty(pi) ? 0 : ClampI(p.presetDim, 0, pi.temporalDims - 1);
    duration = keys[1].layerTime - keys[0].layerTime;
    if (duration <= 0 || keys[1].compTime <= keys[0].compTime) {
        p.presetStatus = L"关键帧顺序无效；暂不支持负时间伸缩图层";
        return false;
    }
    delta = keys[1].value[dim] - keys[0].value[dim];
    if (IsSpatialPresetProperty(pi)) {
        AEGP_StreamValue2 out{}, in{};
        if (ks->AEGP_GetNewKeyframeSpatialTangents(g_plugin_id, pi.streamH,
                p.presetSegment, NULL, &out)) {
            p.presetStatus = L"无法读取空间路径，未修改";
            return false;
        }
        A_Err err = ks->AEGP_GetNewKeyframeSpatialTangents(g_plugin_id, pi.streamH,
                p.presetSegment + 1, &in, NULL);
        if (!err) {
            auto c1 = keys[0].value, c2 = keys[1].value;
            for (int d = 0; d < pi.numDims; ++d) {
                c1[d] += GetDimValue(out.val, pi.type, d);
                c2[d] += GetDimValue(in.val, pi.type, d);
            }
            delta = curve_presets::SpatialLength(keys[0].value, c1, c2, keys[1].value);
            suites.StreamSuite6()->AEGP_DisposeStreamValue(&in);
        }
        suites.StreamSuite6()->AEGP_DisposeStreamValue(&out);
        if (err) { p.presetStatus = L"无法读取空间路径，未修改"; return false; }
    }
    if (!std::isfinite(delta) || !std::isfinite(duration)) {
        p.presetStatus = L"曲线包含无效数值，未修改";
        return false;
    }
    return true;
}

static curve_presets::Interpolation PresetInterpolation(AEGP_KeyframeInterpolationType type) {
    if (type == AEGP_KeyInterp_LINEAR) return curve_presets::Interpolation::Linear;
    if (type == AEGP_KeyInterp_HOLD) return curve_presets::Interpolation::Hold;
    return curve_presets::Interpolation::Bezier;
}

static AEGP_KeyframeInterpolationType AEInterpolation(curve_presets::Interpolation type) {
    if (type == curve_presets::Interpolation::Linear) return AEGP_KeyInterp_LINEAR;
    if (type == curve_presets::Interpolation::Hold) return AEGP_KeyInterp_HOLD;
    return AEGP_KeyInterp_BEZIER;
}

static A_Err WritePresetKey(AEGP_KeyframeSuite5* ks, const PropertyInfo& pi,
                            int index, const PresetKeyState& state, bool restoreFlags) {
    A_Err err = ks->AEGP_SetKeyframeFlag(pi.streamH, index, AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER, FALSE);
    if (!err) err = ks->AEGP_SetKeyframeFlag(pi.streamH, index, AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS, FALSE);
    if (!err) err = ks->AEGP_SetKeyframeInterpolation(pi.streamH, index, state.inType, state.outType);
    for (int d = 0; !err && d < pi.temporalDims; ++d)
        err = ks->AEGP_SetKeyframeTemporalEase(pi.streamH, index, d, &state.in[d], &state.out[d]);
    if (restoreFlags) {
        A_Err flagErr = ks->AEGP_SetKeyframeFlag(pi.streamH, index, AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS,
                              (state.flags & AEGP_KeyframeFlag_TEMPORAL_CONTINUOUS) != 0);
        if (!err) err = flagErr;
        flagErr = ks->AEGP_SetKeyframeFlag(pi.streamH, index, AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER,
                              (state.flags & AEGP_KeyframeFlag_TEMPORAL_AUTOBEZIER) != 0);
        if (!err) err = flagErr;
    }
    return err;
}

static bool SamePresetKey(const PresetKeyState& actual, const PresetKeyState& expected,
                           const PropertyInfo& pi) {
    auto close = [](double a, double b) { return std::isfinite(a) && std::isfinite(b) &&
        fabs(a - b) <= 1e-6 * (1.0 + fabs(b)); };
    if (!close(actual.compTime, expected.compTime) || !close(actual.layerTime, expected.layerTime) ||
        actual.flags != expected.flags || actual.inType != expected.inType || actual.outType != expected.outType) return false;
    for (int d = 0; d < pi.numDims; ++d) if (!close(actual.value[d], expected.value[d])) return false;
    for (int d = 0; d < pi.temporalDims; ++d) {
        if (expected.inType == AEGP_KeyInterp_BEZIER &&
            (!close(actual.in[d].speedF, expected.in[d].speedF) || !close(actual.in[d].influenceF, expected.in[d].influenceF))) return false;
        if (expected.outType == AEGP_KeyInterp_BEZIER &&
            (!close(actual.out[d].speedF, expected.out[d].speedF) || !close(actual.out[d].influenceF, expected.out[d].influenceF))) return false;
    }
    return true;
}

static void ApplySegmentPreset(const curve_presets::Preset& preset,
                               const double* rawOut = nullptr, const double* rawIn = nullptr) {
    AEGP_SuiteHandler suites(g_sp);
    PresetKeyState before[2];
    double delta = 0, duration = 0, outSpeed = 0, inSpeed = 0;
    int dim = 0;
    if (!ReadPresetSegment(suites, before, delta, duration, dim)) return;
    auto& p = g_panel;
    const auto& pi = g_props[p.curProp];
    // Linear/hold sides do not need a slope when a segment has equal values.
    auto effective = preset;
    if (effective.outType != curve_presets::Interpolation::Bezier) effective.outSlope = 0;
    if (effective.inType != curve_presets::Interpolation::Bezier) effective.inSlope = 0;
    bool valid = curve_presets::IsValid(effective);
    if (rawOut && rawIn) { outSpeed = *rawOut; inSpeed = *rawIn;
        valid = valid && std::isfinite(outSpeed) && std::isfinite(inSpeed);
    } else valid = valid && curve_presets::Speeds(effective, delta, duration, outSpeed, inSpeed);
    if (!valid ||
        (IsSpatialPresetProperty(pi) && (outSpeed < 0 || inSpeed < 0))) {
        p.presetStatus = L"模板无法用于此段：值差为零或空间速度为负";
        return;
    }
    PresetKeyState after[2] = { before[0], before[1] };
    after[0].outType = AEInterpolation(preset.outType);
    after[1].inType = AEInterpolation(preset.inType);
    after[0].out[dim] = { outSpeed, preset.outInfluence / 100.0 };
    after[1].in[dim] = { inSpeed, preset.inInfluence / 100.0 };
    auto* ks = suites.KeyframeSuite5();
    auto* utility = suites.UtilitySuite6();
    if (utility->AEGP_StartUndoGroup("Dalimao Curves - Apply Curve Preset")) {
        p.presetStatus = L"无法创建撤销组，未修改";
        return;
    }
    bool failed = false, rollbackFailed = false;
    try {
        for (int i = 0; i < 2 && !failed; ++i)
            failed = WritePresetKey(ks, pi, p.presetSegment + i, after[i], false) != A_Err_NONE;
        // Confirm that the target sides were accepted and unrelated endpoint
        // values/times and the opposite ease sides stayed intact.
        for (int i = 0; i < 2 && !failed; ++i) {
            PresetKeyState actual;
            failed = !ReadPresetKey(suites, pi, p.presetSegment + i, actual);
            if (failed) break;
            auto close = [](double a, double b) { return fabs(a - b) <= 1e-6 * (1.0 + fabs(b)); };
            failed = !close(actual.compTime, before[i].compTime) ||
                     actual.inType != after[i].inType || actual.outType != after[i].outType;
            for (int d = 0; d < pi.numDims; ++d) failed |= !close(actual.value[d], before[i].value[d]);
            for (int d = 0; d < pi.temporalDims; ++d) {
                if (after[i].inType == AEGP_KeyInterp_BEZIER)
                    failed |= !close(actual.in[d].speedF, after[i].in[d].speedF) ||
                              !close(actual.in[d].influenceF, after[i].in[d].influenceF);
                if (after[i].outType == AEGP_KeyInterp_BEZIER)
                    failed |= !close(actual.out[d].speedF, after[i].out[d].speedF) ||
                              !close(actual.out[d].influenceF, after[i].out[d].influenceF);
            }
        }
    } catch (...) { failed = true; }
    if (failed) {
        try {
            for (int i = 0; i < 2; ++i)
                rollbackFailed |= WritePresetKey(ks, pi, p.presetSegment + i, before[i], true) != A_Err_NONE;
            for (int i = 0; i < 2; ++i) {
                PresetKeyState restored;
                rollbackFailed |= !ReadPresetKey(suites, pi, p.presetSegment + i, restored) ||
                                  !SamePresetKey(restored, before[i], pi);
            }
        } catch (...) { rollbackFailed = true; }
    }
    A_Err endError = utility->AEGP_EndUndoGroup();
    int segment = p.presetSegment;
    ReloadKeyframes();
    if (p.curProp < 0) {
        p.toast = true;
        p.toastMsg = L"应用后无法刷新关键帧。请关闭面板，检查 AE；如需恢复请按 Ctrl+Z。";
        return;
    }
    p.presetSegment = segment;
    p.selKf = segment;
    p.presetStatus = rollbackFailed ? L"应用及恢复失败，请关闭面板后在 AE 中撤销" :
        failed ? L"AE 未接受此曲线，已恢复原样" :
        endError ? L"已应用，但撤销组关闭失败，请检查 AE" :
        L"已应用到所选两帧 · 在 AE 中 Ctrl+Z 撤销";
    CurvesDebugLog(p.presetStatus.c_str());
}

static void CaptureSegmentPreset() {
    AEGP_SuiteHandler suites(g_sp);
    PresetKeyState keys[2];
    double delta = 0, duration = 0;
    int dim = 0;
    if (!ReadPresetSegment(suites, keys, delta, duration, dim)) return;
    if (g_userPresets.size() >= 100) { g_panel.presetStatus = L"最多保存 100 个模板，请先删除不需要的模板"; return; }
    curve_presets::Preset preset;
    for (int n = 1; n <= 101; ++n) {
        char name[32]; sprintf_s(name, "Curve %03d", n);
        preset.name = name;
        bool used = false;
        for (const auto& item : g_userPresets) used |= item.name == preset.name;
        if (!used) break;
    }
    preset.outType = PresetInterpolation(keys[0].outType);
    preset.inType = PresetInterpolation(keys[1].inType);
    preset.outInfluence = preset.outType == curve_presets::Interpolation::Bezier ? keys[0].out[dim].influenceF * 100.0 : 100.0 / 3;
    preset.inInfluence = preset.inType == curve_presets::Interpolation::Bezier ? keys[1].in[dim].influenceF * 100.0 : 100.0 / 3;
    double outSpeed = preset.outType == curve_presets::Interpolation::Bezier ? keys[0].out[dim].speedF : 0;
    double inSpeed = preset.inType == curve_presets::Interpolation::Bezier ? keys[1].in[dim].speedF : 0;
    if (!curve_presets::Capture(preset, outSpeed, inSpeed, delta, duration)) {
        g_panel.presetStatus = L"无法归一化此曲线：零值差且存在非零速度，或数据无效";
        return;
    }
    auto next = g_userPresets;
    next.push_back(preset);
    if (SavePresetLibrary(next)) {
        g_panel.libraryIndex = (int)next.size() - 1;
        g_panel.presetStatus = L"已保存当前轴的两帧曲线模板 · 重启 AE 后仍可使用";
    }
}


static void ApplyHandleInfluences(double outPercent, double inPercent) {
    AEGP_SuiteHandler suites(g_sp);
    PresetKeyState keys[2]; double delta = 0, duration = 0; int dim = 0;
    if (!ReadPresetSegment(suites, keys, delta, duration, dim)) return;
    curve_presets::Preset preset;
    preset.name = "Handle length";
    preset.outInfluence = outPercent; preset.inInfluence = inPercent;
    double outSpeed = keys[0].outType == AEGP_KeyInterp_BEZIER ? keys[0].out[dim].speedF : 0;
    double inSpeed = keys[1].inType == AEGP_KeyInterp_BEZIER ? keys[1].in[dim].speedF : 0;
    ApplySegmentPreset(preset, &outSpeed, &inSpeed);
}
