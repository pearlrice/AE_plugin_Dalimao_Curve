#pragma once

#include "CurvePresetModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace curve_thumbnail {

constexpr std::size_t kMaxSamples = 65;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Samples {
    std::array<Point, kMaxSamples> points{};
    std::size_t count = 0;
    double minY = 0.0;
    double maxY = 1.0;
};

struct ControlPoints {
    Point p0{ 0.0, 0.0 };
    Point p1{ 1.0 / 3.0, 1.0 / 3.0 };
    Point p2{ 2.0 / 3.0, 2.0 / 3.0 };
    Point p3{ 1.0, 1.0 };
};

inline double FiniteOr(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

inline Point CubicPoint(Point p0, Point p1, Point p2, Point p3, double t) {
    const double u = 1.0 - t;
    const double uu = u * u;
    const double tt = t * t;
    return {
        uu * u * p0.x + 3.0 * uu * t * p1.x + 3.0 * u * tt * p2.x + tt * t * p3.x,
        uu * u * p0.y + 3.0 * uu * t * p1.y + 3.0 * u * tt * p2.y + tt * t * p3.y,
    };
}

// These are normalized value-over-time thumbnails. They visualize easing shape,
// including overshoot, rather than reproducing After Effects' speed graph.
inline ControlPoints MakeControlPoints(const curve_presets::Preset& preset) {
    const double outInfluence = std::clamp(FiniteOr(preset.outInfluence, 33.333) / 100.0, 0.001, 1.0);
    const double inInfluence = std::clamp(FiniteOr(preset.inInfluence, 33.333) / 100.0, 0.001, 1.0);
    const double outSlope = FiniteOr(preset.outSlope, 1.0);
    const double inSlope = FiniteOr(preset.inSlope, 1.0);
    ControlPoints result;
    result.p1 = preset.outType == curve_presets::Interpolation::Linear
        ? Point{ 1.0 / 3.0, 1.0 / 3.0 }
        : Point{ outInfluence, outSlope * outInfluence };
    result.p2 = preset.inType == curve_presets::Interpolation::Linear
        ? Point{ 2.0 / 3.0, 2.0 / 3.0 }
        : Point{ 1.0 - inInfluence, 1.0 - inSlope * inInfluence };
    return result;
}

inline Samples Sample(const curve_presets::Preset& preset, std::size_t requestedSamples = 49) {
    Samples result;
    if (preset.outType == curve_presets::Interpolation::Hold ||
        preset.inType == curve_presets::Interpolation::Hold) {
        result.points[0] = { 0.0, 0.0 };
        result.points[1] = { 1.0, 0.0 };
        result.points[2] = { 1.0, 1.0 };
        result.count = 3;
        return result;
    }

    const ControlPoints controls = MakeControlPoints(preset);

    result.count = std::clamp(requestedSamples, std::size_t{ 2 }, kMaxSamples);
    for (std::size_t index = 0; index < result.count; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(result.count - 1);
        result.points[index] = CubicPoint(
            controls.p0, controls.p1, controls.p2, controls.p3, t);
        result.minY = std::min(result.minY, result.points[index].y);
        result.maxY = std::max(result.maxY, result.points[index].y);
    }
    return result;
}

} // namespace curve_thumbnail
