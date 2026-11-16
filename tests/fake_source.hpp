// Fake scene source for tests: synthetic 320x240 scenes, no network.
#pragma once
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include "../desktop/types.hpp"

namespace sv {

inline SceneData MakeSyntheticScene(int dnOffset) {
    SceneData s;
    s.info.id = "FAKE_SCENE_TEST";
    s.info.datetime = "2026-11-02T16:00:00Z";
    s.info.dnOffset = dnOffset;
    s.info.dnDiv = 10000;
    const size_t px = static_cast<size_t>(kWidth) * kHeight;
    s.r.assign(px, 0); s.g.assign(px, 0); s.b.assign(px, 0); s.scl.assign(px, 4);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.004);
    auto put = [&](size_t i, double r, double g, double b) {
        auto dn = [&](double refl) {
            double v = (refl + noise(rng)) * 10000.0 - dnOffset;   // DN = refl*div - offset
            return static_cast<uint16_t>(std::lround(std::max(0.0, std::min(65535.0, v))));
        };
        s.r[i] = dn(r); s.g[i] = dn(g); s.b[i] = dn(b);
    };
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = static_cast<size_t>(y) * kWidth + static_cast<size_t>(x);
            if (x < 120)                       put(i, 0.030, 0.055, 0.022);   // grass / scrub
            else if (x < 190)                  put(i, 0.018, 0.028, 0.030);   // dark water
            else                               put(i, 0.095, 0.105, 0.090);   // dry ground
            if (y > 100 && y < 112)            put(i, 0.14, 0.14, 0.14);      // road
            if (x > 230 && x < 290 && y > 30 && y < 80)   put(i, 0.55, 0.55, 0.52);  // bright roof
            if (x > 40 && x < 80 && y > 150 && y < 200)   put(i, 0.30, 0.29, 0.26);  // concrete pad
            if (x > 300 && y < 40)             s.scl[i] = 9;                  // a cloud corner
        }
    return s;
}

class FakeSource : public SceneSource {
public:
    std::vector<SceneInfo> Search(const BBox&, int, int, const Progress& pr) override {
        std::vector<SceneInfo> out;
        const char* dates[] = {"2026-11-09T16:00:00Z", "2026-11-04T16:00:00Z",
                               "2026-10-30T16:00:00Z", "2026-10-25T16:00:00Z"};
        const double cloud[] = {42.0, 7.5, 1.2, 0.0};
        for (int i = 0; i < 4; ++i) {
            if (pr.log) pr.log("Checking scene " + std::to_string(i + 1) + "/4");
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            SceneInfo si;
            si.id = std::string("FAKE_") + std::to_string(i);
            si.datetime = dates[i];
            si.sceneCloud = cloud[i] + 5;
            si.cropCloudPct = cloud[i];
            si.nodataPct = 0.0;
            si.dnOffset = -1000;
            out.push_back(si);
            if (pr.scene) pr.scene(si);
        }
        infos_ = out;
        return out;
    }
    SceneData Load(size_t index, const Progress& pr) override {
        if (pr.log) pr.log("Downloading bands...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SceneData s = MakeSyntheticScene(infos_.at(index).dnOffset);
        s.info = infos_.at(index);
        for (int level = 1; level < kZoomLevelCount; ++level) {
            SceneLevel z;
            /* A deterministic wider-area fixture; the real source uses a
             * proportionally larger geographic crop for each level. */
            z.r = s.r; z.g = s.g; z.b = s.b; z.scl = s.scl;
            s.zoomOut.push_back(std::move(z));
        }
        return s;
    }
private:
    std::vector<SceneInfo> infos_;
};

}  // namespace sv
