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
        if (suites.StreamSuite6()->AEGP_GetUniqueStreamID(s, &pi.id)) return;
        pi.numKFs = (int)nkfs;
        A_short dims = 1, tdims = 1;
        if (suites.KeyframeSuite5()->AEGP_GetStreamValueDimensionality(s, &dims) ||
            suites.KeyframeSuite5()->AEGP_GetStreamTemporalDimensionality(s, &tdims) ||
            dims < 1 || dims > 3 || tdims < 1 || tdims > 3) return;
        pi.numDims = dims;
        pi.temporalDims = tdims;

        std::wstring leaf = StreamName(suites, s);
        pi.name = prefix.empty() ? leaf : (leaf.empty() ? prefix : prefix + L" > " + leaf);
        out.push_back(pi);
        owned.push_back(s);   // retained only until the current AE callback ends
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

