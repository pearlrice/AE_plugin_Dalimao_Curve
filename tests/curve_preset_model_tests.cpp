#include "../DalimaoCurves/CurvePresetModel.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

bool Near(double actual, double expected) {
    return std::abs(actual - expected) <= 1.0e-12 * std::max(1.0, std::abs(expected));
}

void TestBuiltins() {
    const auto presets = curve_presets::Builtins();
    Check(presets.size() == 6, "all six builtins are present");
    Check(presets.front().name == "Linear", "linear is first");
    Check(presets.front().outType == curve_presets::Interpolation::Linear,
        "linear has linear outgoing interpolation");
    Check(Near(presets.front().outSlope, 1.0) && Near(presets.front().inSlope, 1.0),
        "linear slopes are one");
    Check(presets.back().name == "Extreme Ease", "extreme ease is last");
    Check(presets.back().outInfluence == 100.0 && presets.back().inInfluence == 100.0,
        "extreme ease retains 100 percent influence");
    for (const auto& preset : presets) {
        Check(curve_presets::IsValid(preset), "builtin is valid");
    }
}

void TestCaptureAndReapply() {
    curve_presets::Preset preset{"Captured"};
    Check(curve_presets::Capture(preset, 6.0, 3.0, 12.0, 4.0), "capture succeeds");
    Check(Near(preset.outSlope, 2.0) && Near(preset.inSlope, 1.0), "capture stores ratios");

    double outSpeed = 0.0;
    double inSpeed = 0.0;
    Check(curve_presets::Speeds(preset, 40.0, 5.0, outSpeed, inSpeed),
        "reapply succeeds with a new amplitude and duration");
    Check(Near(outSpeed, 16.0) && Near(inSpeed, 8.0), "reapply scales endpoint speeds");

    curve_presets::Preset reverse{"Reverse"};
    Check(curve_presets::Capture(reverse, 4.0, 2.0, -8.0, 2.0),
        "capture accepts reversed scalar direction");
    Check(Near(reverse.outSlope, -1.0) && Near(reverse.inSlope, -0.5),
        "reversed capture stores signed slopes");
    Check(curve_presets::Speeds(reverse, -20.0, 5.0, outSpeed, inSpeed),
        "reversed reapply succeeds");
    Check(Near(outSpeed, 4.0) && Near(inSpeed, 2.0), "reversed reapply restores positive speeds");
}

void TestDegenerateAndInvalidValues() {
    curve_presets::Preset preset{"Flat"};
    Check(curve_presets::Capture(preset, 0.0, -0.0, 0.0, 2.0),
        "zero delta accepts zero endpoint speeds");
    Check(preset.outSlope == 0.0 && preset.inSlope == 0.0, "zero delta stores zero slopes");
    Check(!curve_presets::Capture(preset, 0.01, 0.0, 0.0, 2.0),
        "zero delta rejects a nonzero endpoint speed");

    double outSpeed = 9.0;
    double inSpeed = 8.0;
    Check(curve_presets::Speeds(preset, 1.0e-14, 2.0, outSpeed, inSpeed),
        "near-zero delta accepts zero stored slopes");
    Check(outSpeed == 0.0 && inSpeed == 0.0, "near-zero conversion produces zero speeds");
    preset.outSlope = 1.0;
    Check(!curve_presets::Speeds(preset, 0.0, 2.0, outSpeed, inSpeed),
        "zero delta rejects nonzero stored slope");

    preset.outSlope = 0.0;
    preset.outInfluence = 0.09;
    Check(!curve_presets::IsValid(preset), "influence below 0.1 is invalid");
    preset.outInfluence = 100.01;
    Check(!curve_presets::IsValid(preset), "influence above 100 is invalid");
    preset.outInfluence = std::numeric_limits<double>::quiet_NaN();
    Check(!curve_presets::IsValid(preset), "nonfinite influence is invalid");
    preset.outInfluence = 50.0;
    preset.outType = static_cast<curve_presets::Interpolation>(99);
    Check(!curve_presets::IsValid(preset), "unknown interpolation is invalid");
    preset.outType = curve_presets::Interpolation::Hold;
    preset.inType = curve_presets::Interpolation::Linear;
    Check(curve_presets::IsValid(preset), "hold and linear interpolation kinds are valid");
    Check(!curve_presets::Capture(preset, 1.0, 1.0, 1.0, 0.0), "zero duration is invalid");
}

void TestPersistence() {
    auto source = curve_presets::Builtins();
    curve_presets::Preset custom{"Quoted \"Name\"", curve_presets::Interpolation::Hold,
        curve_presets::Interpolation::Linear, 0.1, 100.0, -1.0 / 3.0, 9.87654321012345};
    source.push_back(custom);

    std::stringstream storage;
    Check(curve_presets::Write(storage, source), "valid presets serialize");
    std::vector<curve_presets::Preset> restored;
    Check(curve_presets::Read(storage, restored), "serialized presets deserialize");
    Check(restored.size() == source.size(), "roundtrip preserves count");
    Check(restored.back().name == custom.name && restored.back().outType == custom.outType,
        "roundtrip preserves name and interpolation");
    Check(restored.back().outSlope == custom.outSlope && restored.back().inSlope == custom.inSlope,
        "roundtrip preserves doubles exactly");

    const std::vector<curve_presets::Preset> sentinel{{"Keep me"}};
    const std::vector<std::string> corruptions = {
        "DALIMAO_CURVES_PRESETS 2\n0\n",
        "DALIMAO_CURVES_PRESETS 1\n1\n\"Truncated\" 1 1 33.0\n",
        "DALIMAO_CURVES_PRESETS 1\n1\n\"Bad\" 1 1 nan 33 0 0\n",
        "DALIMAO_CURVES_PRESETS 1\n1\n\"Bad\" 1 1 0.09 33 0 0\n",
        "DALIMAO_CURVES_PRESETS 1\n101\n",
        "DALIMAO_CURVES_PRESETS 1\n0\nunexpected\n",
        "DALIMAO_CURVES_PRESETS 1\n1\n\"Bad\nName\" 1 1 33 33 0 0\n",
    };
    for (const std::string& corrupted : corruptions) {
        std::istringstream input(corrupted);
        auto destination = sentinel;
        Check(!curve_presets::Read(input, destination), "corrupted persistence is rejected");
        Check(destination.size() == 1 && destination.front().name == "Keep me",
            "failed read leaves destination unchanged");
    }

    auto invalid = source;
    invalid.front().name.assign(81, 'x');
    std::ostringstream output;
    Check(!curve_presets::Write(output, invalid), "invalid preset cannot be serialized");
}

void TestSpatialLength() {
    using Point = std::array<double, 3>;
    Check(Near(curve_presets::SpatialLength(
        Point{0.0, 0.0, 0.0}, Point{25.0, 0.0, 0.0},
        Point{75.0, 0.0, 0.0}, Point{100.0, 0.0, 0.0}), 100.0),
        "straight cubic length is exact");

    const double curved = curve_presets::SpatialLength(
        Point{0.0, 0.0, 0.0}, Point{0.0, 100.0, 0.0},
        Point{100.0, 100.0, 0.0}, Point{100.0, 0.0, 0.0});
    Check(curved > 100.0, "curved cubic is longer than its chord");

    const double loop = curve_presets::SpatialLength(
        Point{0.0, 0.0, 0.0}, Point{100.0, 100.0, 0.0},
        Point{-100.0, 100.0, 0.0}, Point{0.0, 0.0, 0.0});
    Check(loop > 0.0, "closed cubic retains nonzero traveled length");
    Check(curve_presets::SpatialLength(Point{}, Point{}, Point{}, Point{}) == 0.0,
        "zero spatial path has zero length");

    const double invalid = curve_presets::SpatialLength(
        Point{0.0, 0.0, 0.0}, Point{0.0, 0.0, 0.0},
        Point{0.0, std::numeric_limits<double>::infinity(), 0.0}, Point{});
    Check(std::isnan(invalid), "nonfinite spatial input returns NaN");
}

}  // namespace

int main() {
    TestBuiltins();
    TestCaptureAndReapply();
    TestDegenerateAndInvalidValues();
    TestPersistence();
    TestSpatialLength();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All curve preset model tests passed\n";
    return 0;
}
