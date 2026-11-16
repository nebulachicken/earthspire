#include "grade.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "svd_render.h"

namespace sv {
namespace {

constexpr double kWhiteRefl = 0.50;      // reflectance that maps to full scale at EV 0
constexpr unsigned kBakedDiv = 10000;    // divisor written into graded files

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double Clamp01(double v) { return Clamp(v, 0.0, 1.0); }

// Soft highlight roll-off. hi = 0 clips hard at 1.0; hi = 100 starts rolling
// off at 0.45 so bright roofs and sand keep some detail instead of blowing out.
double Shoulder(double x, double hiPct) {
    if (x <= 0.0) return 0.0;
    const double knee = 1.0 - 0.55 * hiPct / 100.0;
    if (knee >= 0.999) return x > 1.0 ? 1.0 : x;
    if (x <= knee) return x;
    return knee + (1.0 - knee) * std::tanh((x - knee) / (1.0 - knee));
}

struct Curve {
    double gain[3];     // linear gain per channel (exposure, temperature, white point)
    double expo;        // gamma exponent applied to the shouldered value
    double shadowK;     // shadow lift strength
    double highlights;  // 0..100
    double contrast;    // -1..1
    double sat;         // 0..2
};

Curve MakeCurve(const GradeParams& p) {
    Curve c;
    const double lin = std::pow(2.0, p.ev) / kWhiteRefl;
    const double t = p.temperature / 100.0;
    c.gain[0] = lin * (1.0 + 0.20 * t);
    c.gain[1] = lin;
    c.gain[2] = lin * (1.0 - 0.20 * t);
    c.expo = 0.5 * std::pow(2.0, -p.midtones / 100.0);   // 0.5 = sqrt, same as the calculator
    c.shadowK = p.shadows >= 0.0 ? 1.5 * p.shadows / 100.0 : 0.9 * p.shadows / 100.0;
    c.highlights = p.highlights;
    c.contrast = p.contrast / 100.0;
    c.sat = p.saturation / 100.0;
    return c;
}

// reflectance (>= 0 after clamping) -> display value 0..1
double Tone(double refl, double gain, const Curve& c) {
    const double x = (refl > 0.0 ? refl : 0.0) * gain;
    const double y = Shoulder(x, c.highlights);
    double d = std::pow(y, c.expo);
    d += c.shadowK * d * (1.0 - d) * (1.0 - d);          // lift/deepen the darks, keeps 0 and 1 fixed
    d = Clamp01(d);
    if (c.contrast >= 0.0) {
        const double s = d * d * (3.0 - 2.0 * d);          // S-curve
        d += c.contrast * (s - d);
    } else {
        d += (-c.contrast) * 0.4 * (0.5 - d);              // flatten towards mid grey
    }
    return Clamp01(d);
}

// For each display level 0..255, the baked DN that the calculator's real
// tone function (svd_tone) turns back into exactly that level.
const std::array<uint16_t, 256>& BakeTable() {
    static const std::array<uint16_t, 256> table = [] {
        std::array<int, 257> lo;
        lo.fill(-1);
        const int maxDn = static_cast<int>(kBakedDiv * 3u / 10u);   // white point
        for (int dn = 0; dn <= maxDn; ++dn) {
            const int t = svd_tone(dn, kBakedDiv, 256);
            if (lo[static_cast<size_t>(t)] < 0) lo[static_cast<size_t>(t)] = dn;
        }
        // Dark levels the tone function can never produce snap to the nearest one it can.
        std::array<bool, 256> reachable{};
        for (int t = 0; t < 256; ++t) reachable[static_cast<size_t>(t)] = lo[static_cast<size_t>(t)] >= 0;
        lo[256] = maxDn + 1;
        int nextLo = lo[256];
        for (int t = 255; t >= 0; --t) {
            if (reachable[static_cast<size_t>(t)]) { nextLo = lo[static_cast<size_t>(t)]; continue; }
            int below = t - 1;
            while (below >= 0 && !reachable[static_cast<size_t>(below)]) --below;
            int above = t + 1;
            while (above < 256 && !reachable[static_cast<size_t>(above)]) ++above;
            const bool useBelow = below >= 0 && (above >= 256 || (t - below) <= (above - t));
            lo[static_cast<size_t>(t)] = useBelow ? lo[static_cast<size_t>(below)] : nextLo;
        }
        std::array<uint16_t, 256> out{};
        for (int t = 0; t < 256; ++t) {
            int hi = (t < 255) ? lo[static_cast<size_t>(t + 1)] - 1 : lo[static_cast<size_t>(t)];
            if (hi < lo[static_cast<size_t>(t)]) hi = lo[static_cast<size_t>(t)];
            out[static_cast<size_t>(t)] = static_cast<uint16_t>((lo[static_cast<size_t>(t)] + hi) / 2);
        }
        return out;
    }();
    return table;
}

void CopyText(char* dst, size_t dstSize, const std::string& src) {
    std::memset(dst, 0, dstSize);
    std::memcpy(dst, src.data(), std::min(src.size(), dstSize - 1));
}

void FillInfo(svd_info& info, const SceneInfo& scene, bool hasScl, int dnOffset, unsigned dnDiv) {
    std::memset(&info, 0, sizeof info);
    info.width = kWidth;
    info.height = kHeight;
    info.has_scl = hasScl ? 1 : 0;
    info.dn_offset = dnOffset;
    info.dn_div = dnDiv;
    info.cloud_permille = static_cast<uint16_t>(std::lround(scene.cropCloudPct * 10.0));
    info.nodata_permille = static_cast<uint16_t>(std::lround(scene.nodataPct * 10.0));
    CopyText(info.acquired, sizeof info.acquired,
             scene.datetime.size() >= 19 ? scene.datetime.substr(0, 19) + "Z" : scene.datetime);
    CopyText(info.scene_id, sizeof info.scene_id, scene.id);
}

bool ValidRaster(const SceneInfo& info, const std::vector<uint16_t>& r,
                 const std::vector<uint16_t>& g, const std::vector<uint16_t>& b,
                 const std::vector<uint8_t>& scl) {
    const size_t px = static_cast<size_t>(kWidth) * kHeight;
    return r.size() == px && g.size() == px && b.size() == px &&
           (scl.empty() || scl.size() == px) && info.dnDiv != 0;
}

void Finish(Graded& g, const std::vector<uint16_t>* planes[3],
            const std::vector<uint8_t>& scl) {
    const size_t px = static_cast<size_t>(kWidth) * kHeight;
    g.payload.assign(svd_expected_payload_size(kWidth, kHeight, g.info.has_scl), 0);
    for (size_t p = 0; p < 3; ++p)
        for (size_t i = 0; i < px; ++i)
            svd_put_u16(g.payload.data() + p * px * 2 + i * 2, (*planes[p])[i]);
    if (g.info.has_scl) std::memcpy(g.payload.data() + px * 6, scl.data(), px);
    g.info.payload_size = static_cast<uint32_t>(g.payload.size());
    g.info.payload_crc32 = svd_crc32(0, g.payload.data(), g.payload.size());
}

Graded RawRaster(const SceneInfo& info,
                 const std::vector<uint16_t>& r, const std::vector<uint16_t>& g,
                 const std::vector<uint16_t>& b, const std::vector<uint8_t>& scl) {
    Graded out;
    if (!ValidRaster(info, r, g, b, scl)) return out;
    FillInfo(out.info, info, !scl.empty(), info.dnOffset, info.dnDiv);
    out.stats.offset = info.dnDiv ? static_cast<double>(info.dnOffset) / info.dnDiv : 0.0;
    const std::vector<uint16_t>* planes[3] = {&r, &g, &b};
    Finish(out, planes, scl);
    return out;
}

Graded GradeRaster(const SceneInfo& info,
                   const std::vector<uint16_t>& r, const std::vector<uint16_t>& g,
                   const std::vector<uint16_t>& b, const std::vector<uint8_t>& scl,
                   const GradeParams& in) {
    Graded out;
    if (!ValidRaster(info, r, g, b, scl)) return out;
    GradeParams gp = in;
    ClampParams(gp);

    const size_t px = static_cast<size_t>(kWidth) * kHeight;
    const double offset = gp.useSceneOffset
        ? (info.dnDiv ? static_cast<double>(info.dnOffset) / info.dnDiv : 0.0)
        : gp.offset;
    const double invDiv = 1.0 / static_cast<double>(info.dnDiv);
    const Curve c = MakeCurve(gp);
    const auto& bake = BakeTable();

    std::vector<uint16_t> outPlanes[3];
    for (auto& v : outPlanes) v.assign(px, 0);

    size_t valid = 0, black = 0, white = 0;
    for (size_t i = 0; i < px; ++i) {
        if (!scl.empty() && scl[i] == 0) continue;
        ++valid;
        const double refl[3] = {r[i] * invDiv + offset, g[i] * invDiv + offset,
                                b[i] * invDiv + offset};
        if (refl[0] <= 0.0 && refl[1] <= 0.0 && refl[2] <= 0.0) ++black;

        double d[3];
        for (int k = 0; k < 3; ++k) d[k] = Tone(refl[k], c.gain[k], c);
        if (c.sat != 1.0) {
            const double luma = 0.2126 * d[0] + 0.7152 * d[1] + 0.0722 * d[2];
            for (int k = 0; k < 3; ++k) d[k] = Clamp01(luma + (d[k] - luma) * c.sat);
        }
        bool bright = false;
        for (int k = 0; k < 3; ++k) {
            const int t = static_cast<int>(std::lround(d[k] * 255.0));
            if (t >= 250) bright = true;
            outPlanes[k][i] = bake[static_cast<size_t>(t)];
        }
        if (bright) ++white;
    }

    FillInfo(out.info, info, !scl.empty(), 0, kBakedDiv);
    out.stats.offset = offset;
    if (valid) {
        out.stats.clippedBlackPct = 100.0 * static_cast<double>(black) / static_cast<double>(valid);
        out.stats.clippedWhitePct = 100.0 * static_cast<double>(white) / static_cast<double>(valid);
    }
    const std::vector<uint16_t>* planes[3] = {&outPlanes[0], &outPlanes[1], &outPlanes[2]};
    Finish(out, planes, scl);
    return out;
}

}  // namespace

void ClampParams(GradeParams& p) {
    p.offset = Clamp(p.offset, -0.5, 0.5);
    p.ev = Clamp(p.ev, -3.0, 3.0);
    p.midtones = Clamp(p.midtones, -100.0, 100.0);
    p.shadows = Clamp(p.shadows, -100.0, 100.0);
    p.highlights = Clamp(p.highlights, 0.0, 100.0);
    p.contrast = Clamp(p.contrast, -100.0, 100.0);
    p.saturation = Clamp(p.saturation, 0.0, 200.0);
    p.temperature = Clamp(p.temperature, -100.0, 100.0);
}

double EffectiveOffset(const SceneData& scene, const GradeParams& p) {
    if (p.useSceneOffset)
        return scene.info.dnDiv ? static_cast<double>(scene.info.dnOffset) / scene.info.dnDiv : 0.0;
    return p.offset;
}

Graded RawScene(const SceneData& s) {
    Graded out = RawRaster(s.info, s.r, s.g, s.b, s.scl);
    for (size_t i = 0; i < s.zoomOut.size() && i + 1 < SVD_MAX_LEVELS; ++i) {
        const SceneLevel& z = s.zoomOut[i];
        Graded item = RawRaster(s.info, z.r, z.g, z.b, z.scl);
        GradedZoomLevel level;
        level.resolution_m = static_cast<uint16_t>(20u << i);
        level.info = item.info;
        level.payload = std::move(item.payload);
        if (!level.payload.empty()) out.zoomOut.push_back(std::move(level));
    }
    return out;
}

Graded GradeScene(const SceneData& s, const GradeParams& in, bool includeZoomOut) {
    Graded out = GradeRaster(s.info, s.r, s.g, s.b, s.scl, in);
    if (!includeZoomOut) return out;
    for (size_t i = 0; i < s.zoomOut.size() && i + 1 < SVD_MAX_LEVELS; ++i) {
        const SceneLevel& z = s.zoomOut[i];
        Graded item = GradeRaster(s.info, z.r, z.g, z.b, z.scl, in);
        GradedZoomLevel level;
        level.resolution_m = static_cast<uint16_t>(20u << i);
        level.info = item.info;
        level.payload = std::move(item.payload);
        if (!level.payload.empty()) out.zoomOut.push_back(std::move(level));
    }
    return out;
}

std::vector<uint16_t> RenderRgb565(const Graded& g, bool showClouds) {
    std::vector<uint16_t> img(static_cast<size_t>(kWidth) * kHeight, 0);
    if (g.payload.empty()) return img;
    svd_render_opts ro;
    svd_render_defaults(&ro);
    ro.show_clouds = showClouds ? 1 : 0;
    svd_render_rgb565(&g.info, g.payload.data(), &ro, img.data());
    return img;
}

std::vector<uint8_t> BuildBmp24(const std::vector<uint16_t>& rgb565) {
    const int rowBytes = (kWidth * 3 + 3) & ~3;
    const uint32_t imageSize = static_cast<uint32_t>(rowBytes) * kHeight;
    const uint32_t fileSize = 54 + imageSize;
    std::vector<uint8_t> out(fileSize, 0);
    auto put32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i) out[o + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    };
    out[0] = 'B'; out[1] = 'M';
    put32(2, fileSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, kWidth);
    put32(22, kHeight);          // positive height: rows stored bottom-up
    out[26] = 1;
    out[28] = 24;
    put32(34, imageSize);
    for (int y = 0; y < kHeight; ++y) {
        uint8_t* row = out.data() + 54 + static_cast<size_t>(kHeight - 1 - y) * static_cast<size_t>(rowBytes);
        for (int x = 0; x < kWidth; ++x) {
            const unsigned v = rgb565[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
            const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            row[x * 3 + 0] = static_cast<uint8_t>((b << 3) | (b >> 2));
            row[x * 3 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
            row[x * 3 + 2] = static_cast<uint8_t>((r << 3) | (r >> 2));
        }
    }
    return out;
}

}  // namespace sv
