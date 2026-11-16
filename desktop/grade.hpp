// Colour grading for Sentinel-2 reflectance data.
//
// The calculator converts reflectance to pixels with one fixed curve. To let
// the user adjust the look without changing the calculator program, the
// adjustments are baked into the saved data: reflectance is graded into the
// display values we want, then written back as reflectance that the
// calculator's fixed curve turns into exactly those values. The preview runs
// the very same conversion code as the calculator, so it matches.
#pragma once

#include <cstdint>
#include <vector>

#include "svd_format.h"
#include "types.hpp"

namespace sv {

struct GradeParams {
    bool useSceneOffset = true;   // true: use the offset from the scene metadata
    double offset = 0.0;          // reflectance added to DN/div (when !useSceneOffset)
    double ev = 0.0;              // exposure in stops, -3..3
    double midtones = 0.0;        // -100..100 (gamma)
    double shadows = 50.0;        // -100..100 (lift)
    double highlights = 60.0;     // 0..100 (highlight recovery)
    double contrast = 0.0;        // -100..100
    double saturation = 100.0;    // 0..200 percent
    double temperature = 0.0;     // -100..100 (cool..warm)
    bool showClouds = false;      // preview only
};

struct GradeStats {
    double clippedBlackPct = 0.0;   // pixels where R,G,B are all <= 0 after the offset
    double clippedWhitePct = 0.0;   // pixels whose graded output is >= 250/255
    double offset = 0.0;            // reflectance offset that was applied
};

struct GradedZoomLevel {
    uint16_t resolution_m = 20;
    svd_info info{};
    std::vector<uint8_t> payload; // temporary graded reflectance payload; packed on export
};

// Base graded payload is retained for the desktop preview. Additional levels
// carry the same render-ready reflectance representation until SVD2 export.
struct Graded {
    svd_info info{};
    std::vector<uint8_t> payload;
    std::vector<GradedZoomLevel> zoomOut;
    GradeStats stats;
};

void ClampParams(GradeParams& p);
double EffectiveOffset(const SceneData& scene, const GradeParams& p);

// Apply the colour settings and bake the result (offset 0, divisor 10000).
Graded GradeScene(const SceneData& scene, const GradeParams& params, bool includeZoomOut = true);
// Pass the raw data through untouched, with the scene's own offset.
Graded RawScene(const SceneData& scene);

// Same conversion the calculator performs, then a 24-bit BMP.
std::vector<uint16_t> RenderRgb565(const Graded& g, bool showClouds);
std::vector<uint8_t> BuildBmp24(const std::vector<uint16_t>& rgb565);

}  // namespace sv
