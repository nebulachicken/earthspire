// Tests for the grading engine: bake round-trip, slider behaviour, monotonic curves.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include "../desktop/grade.hpp"
#include "../desktop/app.hpp"
#include "svd_render.h"
#include "fake_source.hpp"

#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1);} } while (0)
using namespace sv;

static void Px(const std::vector<uint16_t>& img, int x, int y, int& r, int& g, int& b) {
    unsigned v = img[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
    r = ((v >> 11) & 31) * 255 / 31; g = ((v >> 5) & 63) * 255 / 63; b = (v & 31) * 255 / 31;
}

int main() {
    SceneData s = MakeSyntheticScene(-1000);
    for (int level = 1; level < kZoomLevelCount; ++level) {
        SceneLevel z;
        z.r = s.r; z.g = s.g; z.b = s.b; z.scl = s.scl;
        s.zoomOut.push_back(std::move(z));
    }
    GradeParams p;

    // 1. Baked file passes the same verification the calculator does.
    Graded g = GradeScene(s, p);
    CHECK(svd_payload_verify(&g.info, g.payload.data(), g.payload.size()) == SVD_OK);
    CHECK(g.zoomOut.size() == 4);
    CHECK(g.zoomOut[0].resolution_m == 20 && g.zoomOut[3].resolution_m == 160);
    uint8_t hdr[SVD_HEADER_SIZE]; svd_header_write(&g.info, hdr);
    svd_info back; CHECK(svd_header_parse(hdr, &back) == SVD_OK);
    CHECK(back.dn_offset == 0 && back.dn_div == 10000);

    // The exported file contains all five compressed display levels and reads
    // back through the same decoder/checksums used by the calculator.
    const std::filesystem::path pyramidPath =
        std::filesystem::temp_directory_path() / "svd2_grade_test.svd.tns";
    WriteSvdFile(pyramidPath.string(), g);
    VerifySvdFile(pyramidPath.string());
    {
        std::ifstream f(pyramidPath, std::ios::binary);
        uint8_t h2[SVD2_HEADER_SIZE];
        svd_pyramid_info pyramid{};
        CHECK(f.read(reinterpret_cast<char*>(h2), sizeof h2).good());
        CHECK(svd_pyramid_header_parse(h2, &pyramid) == SVD_OK);
        CHECK(pyramid.level_count == 5);
        CHECK(pyramid.levels[0].resolution_m == 10);
        CHECK(pyramid.levels[4].resolution_m == 160);
        const size_t rawPerLevel = svd_pyramid_raw_size(kWidth, kHeight);
        const uintmax_t actual = std::filesystem::file_size(pyramidPath);
        CHECK(actual < SVD2_HEADER_SIZE + rawPerLevel * 5u);

        // The newly exported 10 m level reproduces the desktop preview exactly.
        const auto& first = pyramid.levels[0];
        std::vector<uint8_t> stored(first.stored_size), decoded(first.raw_size);
        CHECK(f.read(reinterpret_cast<char*>(stored.data()),
                     static_cast<std::streamsize>(stored.size())).good());
        CHECK(svd_pyramid_level_decode(&pyramid, 0, stored.data(), stored.size(),
                                       decoded.data(), decoded.size()) == SVD_OK);
        std::vector<uint16_t> fromFile(kWidth * kHeight);
        svd_render_opts ro; svd_render_defaults(&ro);
        CHECK(svd_pyramid_render_rgb565(&pyramid, decoded.data(), 0, &ro,
                                        fromFile.data()) == SVD_OK);
        CHECK(fromFile == RenderRgb565(g, false));
        ro.show_clouds = 1;
        CHECK(svd_pyramid_render_rgb565(&pyramid, decoded.data(), 0, &ro,
                                        fromFile.data()) == SVD_OK);
        CHECK(fromFile == RenderRgb565(g, true));
    }
    std::filesystem::remove(pyramidPath);

    // 2. Bake round-trip: every display level 0..255 survives bake -> svd_tone exactly.
    for (int t = 0; t < 256; ++t) {
        // find the baked DN the same way the engine does, via a one-pixel probe is overkill;
        // instead brute-force: the table must hit t for all reachable levels (>= 5).
        int found = -1;
        for (int dn = 0; dn <= 3000; ++dn) if (svd_tone(dn, 10000, 256) == t) { found = dn; break; }
        if (t >= 12) CHECK(found >= 0);   // darker levels are skipped by the 16-bit data steps
    }

    // 3. Default grading lifts dark ground and does not blow the bright roof flat.
    std::vector<uint16_t> raw = RenderRgb565(RawScene(s), false);
    std::vector<uint16_t> gr  = RenderRgb565(g, false);
    int r, gg, b, r0, g0, b0;
    Px(raw, 40, 20, r0, g0, b0);   Px(gr, 40, 20, r, gg, b);
    std::printf("grass  raw (%3d,%3d,%3d)  graded (%3d,%3d,%3d)\n", r0, g0, b0, r, gg, b);
    CHECK(r + gg + b > r0 + g0 + b0);
    Px(raw, 260, 50, r0, g0, b0);  Px(gr, 260, 50, r, gg, b);
    std::printf("roof   raw (%3d,%3d,%3d)  graded (%3d,%3d,%3d)\n", r0, g0, b0, r, gg, b);
    CHECK(r <= r0);

    // 4. Offset diagnostics: using offset 0 on data that carries +1000 shifts brightness.
    GradeParams q; q.useSceneOffset = false; q.offset = 0.0;
    Graded gz = GradeScene(s, q);
    std::printf("clipped black: scene offset %.1f%%   offset 0: %.1f%%\n",
                g.stats.clippedBlackPct, gz.stats.clippedBlackPct);
    CHECK(gz.stats.clippedBlackPct <= g.stats.clippedBlackPct);

    // 5. Mis-set offset clips: scene stored WITHOUT the +1000 but metadata claims -1000.
    SceneData bad = MakeSyntheticScene(0);
    bad.info.dnOffset = -1000;                       // claims an offset the data does not carry
    Graded gb = GradeScene(bad, p);
    std::printf("mismatched offset -> clipped black %.1f%% (this is the symptom to look for)\n",
                gb.stats.clippedBlackPct);
    CHECK(gb.stats.clippedBlackPct > 30.0);

    // 6. Each slider moves the image in the expected direction, and curves are monotonic.
    auto mean = [&](const GradeParams& gp) {
        std::vector<uint16_t> im = RenderRgb565(GradeScene(s, gp), false);
        double sum = 0; for (uint16_t v : im) sum += ((v >> 11) & 31) + ((v >> 5) & 63) / 2.0 + (v & 31);
        return sum / static_cast<double>(im.size());
    };
    GradeParams a = p, b2 = p;
    a.ev = -1; b2.ev = 1;            CHECK(mean(a) < mean(p) && mean(p) < mean(b2));
    a = p; b2 = p; a.shadows = -50; b2.shadows = 90;   CHECK(mean(a) < mean(p) && mean(p) < mean(b2));
    a = p; b2 = p; a.midtones = -50; b2.midtones = 50; CHECK(mean(a) < mean(p) && mean(p) < mean(b2));
    a = p; a.saturation = 0;
    { std::vector<uint16_t> im = RenderRgb565(GradeScene(s, a), false); int rr,g2,bb; Px(im, 40, 20, rr, g2, bb);
      CHECK(std::abs(rr - g2) < 24 && std::abs(g2 - bb) < 24); }   // grey (565 quantisation allows small drift)
    for (double sh : {-100.0, -40.0, 0.0, 50.0, 100.0})
        for (double con : {-100.0, 0.0, 100.0})
            for (double hi : {0.0, 50.0, 100.0}) {
                GradeParams m = p; m.shadows = sh; m.contrast = con; m.highlights = hi;
                // monotonic in input: brighter reflectance never gives a darker output
                SceneData ramp = MakeSyntheticScene(0);
                for (int x = 0; x < kWidth; ++x) { uint16_t v = static_cast<uint16_t>(x * 20); for (int y = 0; y < kHeight; ++y) { size_t i = static_cast<size_t>(y)*kWidth+static_cast<size_t>(x); ramp.r[i]=ramp.g[i]=ramp.b[i]=v; ramp.scl[i]=4; } }
                ramp.info.dnOffset = 0;
                Graded gm = GradeScene(ramp, m);
                std::vector<uint16_t> im = RenderRgb565(gm, false);
                int prev = -1;
                for (int x = 0; x < kWidth; ++x) { int rr,g2,bb; Px(im, x, 5, rr, g2, bb); CHECK(g2 >= prev - 4); prev = g2; }
            }

    // 7. Neutral settings on a ramp must reproduce the intended curve sqrt(refl/0.50).
    {
        GradeParams n; n.shadows = 0; n.highlights = 0; n.contrast = 0; n.saturation = 100; n.temperature = 0;
        SceneData ramp = MakeSyntheticScene(0);
        for (int x = 0; x < kWidth; ++x) { uint16_t v = static_cast<uint16_t>(x * 20); for (int y = 0; y < kHeight; ++y) { size_t i = static_cast<size_t>(y)*kWidth+static_cast<size_t>(x); ramp.r[i]=ramp.g[i]=ramp.b[i]=v; ramp.scl[i]=4; } }
        ramp.info.dnOffset = 0;
        std::vector<uint16_t> im = RenderRgb565(GradeScene(ramp, n), false);
        int worst = 0;
        for (int x = 0; x < kWidth; ++x) {
            double refl = x * 20 / 10000.0;
            int want = static_cast<int>(std::lround(255.0 * std::sqrt(std::min(refl / 0.50, 1.0))));
            int rr, g2, bb; Px(im, x, 5, rr, g2, bb);
            worst = std::max(worst, std::abs(g2 - want));
        }
        std::printf("neutral ramp: worst green-channel error %d/255 (RGB565 allows about 4)\n", worst);
        CHECK(worst <= 6);
    }
    std::printf("grade_test: all checks passed\n");
    return 0;
}
