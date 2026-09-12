#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace curve_presets {

enum class Interpolation {
    Linear = 0,
    Bezier = 1,
    Hold = 2,
};

struct Preset {
    std::string name;
    Interpolation outType = Interpolation::Bezier;
    Interpolation inType = Interpolation::Bezier;
    double outInfluence = 33.333333;
    double inInfluence = 33.333333;
    double outSlope = 0.0;
    double inSlope = 0.0;
};

namespace detail {

constexpr std::size_t kMaxPresets = 100;
constexpr std::size_t kMaxNameLength = 80;
constexpr double kMinInfluence = 0.1;
constexpr double kMaxInfluence = 100.0;

inline bool IsInterpolationValid(Interpolation value) {
    return value == Interpolation::Linear ||
        value == Interpolation::Bezier ||
        value == Interpolation::Hold;
}

inline bool IsNameValid(const std::string& name) {
    if (name.size() > kMaxNameLength) {
        return false;
    }
    for (unsigned char character : name) {
        if (character < 0x20 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

inline bool IsNearlyZero(double value) {
    return std::abs(value) <= 1.0e-12;
}

inline double Distance(const std::array<double, 3>& first, const std::array<double, 3>& second) {
    return std::hypot(first[0] - second[0], first[1] - second[1], first[2] - second[2]);
}

inline std::array<double, 3> Midpoint(
    const std::array<double, 3>& first,
    const std::array<double, 3>& second) {
    return {
        (first[0] + second[0]) * 0.5,
        (first[1] + second[1]) * 0.5,
        (first[2] + second[2]) * 0.5,
    };
}

inline double SpatialLengthRecursive(
    const std::array<double, 3>& p0,
    const std::array<double, 3>& p1,
    const std::array<double, 3>& p2,
    const std::array<double, 3>& p3,
    int depth) {
    const double chord = Distance(p0, p3);
    const double controlPolygon = Distance(p0, p1) + Distance(p1, p2) + Distance(p2, p3);
    if (depth >= 18 || controlPolygon - chord <= 1.0e-6 * std::max(1.0, controlPolygon)) {
        return (controlPolygon + chord) * 0.5;
    }

    const auto p01 = Midpoint(p0, p1);
    const auto p12 = Midpoint(p1, p2);
    const auto p23 = Midpoint(p2, p3);
    const auto p012 = Midpoint(p01, p12);
    const auto p123 = Midpoint(p12, p23);
    const auto p0123 = Midpoint(p012, p123);
    return SpatialLengthRecursive(p0, p01, p012, p0123, depth + 1) +
        SpatialLengthRecursive(p0123, p123, p23, p3, depth + 1);
}

class LocaleGuard {
public:
    explicit LocaleGuard(std::ios_base& stream)
        : stream_(stream), locale_(stream.getloc()) {
        stream_.imbue(std::locale::classic());
    }

    ~LocaleGuard() {
        try {
            stream_.imbue(locale_);
        } catch (...) {
        }
    }

private:
    std::ios_base& stream_;
    std::locale locale_;
};

}  // namespace detail

inline double SpatialLength(
    const std::array<double, 3>& p0,
    const std::array<double, 3>& p1,
    const std::array<double, 3>& p2,
    const std::array<double, 3>& p3) {
    for (const auto* point : {&p0, &p1, &p2, &p3}) {
        for (double coordinate : *point) {
            if (!std::isfinite(coordinate)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
        }
    }
    return detail::SpatialLengthRecursive(p0, p1, p2, p3, 0);
}

inline bool IsValid(const Preset& preset) {
    return detail::IsNameValid(preset.name) &&
        detail::IsInterpolationValid(preset.outType) &&
        detail::IsInterpolationValid(preset.inType) &&
        std::isfinite(preset.outInfluence) &&
        std::isfinite(preset.inInfluence) &&
        preset.outInfluence >= detail::kMinInfluence &&
        preset.outInfluence <= detail::kMaxInfluence &&
        preset.inInfluence >= detail::kMinInfluence &&
        preset.inInfluence <= detail::kMaxInfluence &&
        std::isfinite(preset.outSlope) &&
        std::isfinite(preset.inSlope);
}

inline std::vector<Preset> Builtins() {
    return {
        {"Linear", Interpolation::Linear, Interpolation::Linear, 33.333, 33.333, 1.0, 1.0},
        {"Easy Ease", Interpolation::Bezier, Interpolation::Bezier, 33.333, 33.333, 0.0, 0.0},
        {"Slow Start", Interpolation::Bezier, Interpolation::Bezier, 100.0, 0.1, 0.0, 0.0},
        {"Slow End", Interpolation::Bezier, Interpolation::Bezier, 0.1, 100.0, 0.0, 0.0},
        {"Strong Ease", Interpolation::Bezier, Interpolation::Bezier, 66.666, 66.666, 0.0, 0.0},
        {"Extreme Ease", Interpolation::Bezier, Interpolation::Bezier, 100.0, 100.0, 0.0, 0.0},
    };
}

inline bool Capture(
    Preset& out,
    double outSpeed,
    double inSpeed,
    double delta,
    double duration) {
    if (!IsValid(out) || !std::isfinite(outSpeed) || !std::isfinite(inSpeed) ||
        !std::isfinite(delta) || !std::isfinite(duration) || duration <= 0.0) {
        return false;
    }

    if (detail::IsNearlyZero(delta)) {
        if (outSpeed != 0.0 || inSpeed != 0.0) {
            return false;
        }
        out.outSlope = 0.0;
        out.inSlope = 0.0;
        return true;
    }

    const double baseSpeed = delta / duration;
    const double outSlope = outSpeed / baseSpeed;
    const double inSlope = inSpeed / baseSpeed;
    if (!std::isfinite(outSlope) || !std::isfinite(inSlope)) {
        return false;
    }

    out.outSlope = outSlope;
    out.inSlope = inSlope;
    return true;
}

inline bool Speeds(
    const Preset& preset,
    double delta,
    double duration,
    double& outSpeed,
    double& inSpeed) {
    if (!IsValid(preset) || !std::isfinite(delta) || !std::isfinite(duration) || duration <= 0.0) {
        return false;
    }

    if (detail::IsNearlyZero(delta)) {
        if (preset.outSlope != 0.0 || preset.inSlope != 0.0) {
            return false;
        }
        outSpeed = 0.0;
        inSpeed = 0.0;
        return true;
    }

    const double baseSpeed = delta / duration;
    const double convertedOutSpeed = preset.outSlope * baseSpeed;
    const double convertedInSpeed = preset.inSlope * baseSpeed;
    if (!std::isfinite(convertedOutSpeed) || !std::isfinite(convertedInSpeed)) {
        return false;
    }

    outSpeed = convertedOutSpeed;
    inSpeed = convertedInSpeed;
    return true;
}

inline bool Write(std::ostream& stream, const std::vector<Preset>& presets) {
    if (presets.size() > detail::kMaxPresets) {
        return false;
    }
    for (const Preset& preset : presets) {
        if (!IsValid(preset)) {
            return false;
        }
    }

    try {
        detail::LocaleGuard localeGuard(stream);
        const std::streamsize oldPrecision = stream.precision();
        stream << "DALIMAO_CURVES_PRESETS 1\n" << presets.size() << '\n';
        stream << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (const Preset& preset : presets) {
            stream << std::quoted(preset.name) << ' '
                << static_cast<int>(preset.outType) << ' '
                << static_cast<int>(preset.inType) << ' '
                << preset.outInfluence << ' ' << preset.inInfluence << ' '
                << preset.outSlope << ' ' << preset.inSlope << '\n';
        }
        stream.precision(oldPrecision);
        return static_cast<bool>(stream);
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

inline bool Read(std::istream& stream, std::vector<Preset>& presets) {
    try {
        detail::LocaleGuard localeGuard(stream);
        std::string magic;
        int version = 0;
        long long count = 0;
        if (!(stream >> magic >> version) || magic != "DALIMAO_CURVES_PRESETS" || version != 1 ||
            !(stream >> count) || count < 0 || count > static_cast<long long>(detail::kMaxPresets)) {
            return false;
        }

        std::vector<Preset> parsed;
        parsed.reserve(static_cast<std::size_t>(count));
        for (long long index = 0; index < count; ++index) {
            Preset preset;
            int outType = 0;
            int inType = 0;
            if (!(stream >> std::quoted(preset.name) >> outType >> inType >>
                preset.outInfluence >> preset.inInfluence >> preset.outSlope >> preset.inSlope)) {
                return false;
            }
            preset.outType = static_cast<Interpolation>(outType);
            preset.inType = static_cast<Interpolation>(inType);
            if (!IsValid(preset)) {
                return false;
            }
            parsed.push_back(std::move(preset));
        }

        stream >> std::ws;
        if (!stream.eof()) {
            return false;
        }
        presets = std::move(parsed);
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

}  // namespace curve_presets
