// Shared types for the desktop app. No third-party dependencies.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sv {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr int kZoomLevelCount = 5; // 10, 20, 40, 80 and 160 metres per pixel

struct BBox {
    double west, south, east, north;
};

// 3.2 km x 2.4 km window centred on (lat, lon). The longitude span is widened
// by 1/cos(lat) so the box is 4:3 on the ground, like the original Cape
// Canaveral window.
inline BBox MakeBBox(double lat, double lon) {
    const double kPi = 3.14159265358979323846;
    double c = std::cos(lat * kPi / 180.0);
    if (c < 0.05) c = 0.05;
    const double halfLat = 1.2 / 111.2;
    const double halfLon = 1.6 / (111.32 * c);
    return {lon - halfLon, lat - halfLat, lon + halfLon, lat + halfLat};
}

struct SceneInfo {
    std::string id;
    std::string datetime;
    double sceneCloud = -1.0;     // whole-scene cloud % from the catalogue
    double cropCloudPct = 0.0;    // cloud + shadow share of the crop (from SCL)
    double nodataPct = 0.0;       // SCL no-data share of the crop
    int dnOffset = 0;             // reflectance = (DN + dnOffset) / dnDiv
    unsigned dnDiv = 10000;
};

struct SceneLevel {
    std::vector<uint16_t> r, g, b;   // raw DN, kWidth * kHeight each
    std::vector<uint8_t> scl;        // Scene Classification Layer
};

struct SceneData {
    SceneInfo info;
    std::vector<uint16_t> r, g, b;   // base 10 m/px level
    std::vector<uint8_t> scl;
    // Additional wider-area levels: 20, 40, 80 and 160 m/pixel.
    std::vector<SceneLevel> zoomOut;
};

struct Progress {
    std::function<void(const std::string&)> log;      // status text
    std::function<void(const SceneInfo&)> scene;      // a scene was screened
    std::function<bool()> cancelled;                  // true = stop early
};

// Where scenes come from. The real implementation talks to the Earth Search
// STAC API and reads cloud-optimised GeoTIFFs with GDAL; tests use a fake.
class SceneSource {
public:
    virtual ~SceneSource() = default;
    // Find scenes over the box, newest first, and read the cloud mask of up to
    // maxScreen of them. Returned order defines the index used by Load().
    virtual std::vector<SceneInfo> Search(const BBox& box, int days, int maxScreen,
                                          const Progress& progress) = 0;
    // Download the colour bands of scene `index` from the last Search().
    virtual SceneData Load(size_t index, const Progress& progress) = 0;
};

}  // namespace sv
