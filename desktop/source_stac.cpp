// Sentinel-2 L2A scenes from Element 84 Earth Search, read with GDAL.
//
// Adapted from the v0.2 fetcher: the search/crop/cloud-check logic is the
// same, but the crop window is now a parameter (chosen in the UI) and the
// work is split into Search() (list + screen scenes by cloud cover over the
// crop) and Load() (download the colour bands of one chosen scene).
#include "source_stac.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>

#include <curl/curl.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <nlohmann/json.hpp>

namespace sv {
namespace {

using json = nlohmann::json;

constexpr const char* kStacSearch = "https://earth-search.aws.element84.com/v1/search";
constexpr int kMaxCandidates = 40;

struct Candidate {
    std::string id, datetime;
    std::string redHref, greenHref, blueHref, sclHref;
    double sceneCloud = -1.0;
    int dnOffset = 0;
    unsigned dnDiv = 10000;
};

struct SclStats {
    double cloudPct = 0.0;
    double nodataPct = 0.0;
};

// ---------------------------------------------------------------- utilities

std::string IsoUtc(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

struct DatasetCloser {
    void operator()(void* h) const {
        if (h) GDALClose(static_cast<GDALDatasetH>(h));
    }
};
using Dataset = std::unique_ptr<void, DatasetCloser>;

size_t CurlWrite(char* data, size_t size, size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

std::string HttpPostJson(const std::string& url, const json& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Could not initialize libcurl.");

    std::string response;
    const std::string request = body.dump();
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/geo+json, application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CapeCanaveralSentinelFetch/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK)
        throw std::runtime_error(std::string("STAC request failed: ") + curl_easy_strerror(code));
    if (status < 200 || status >= 300)
        throw std::runtime_error("STAC returned HTTP " + std::to_string(status) + ": " + response);
    return response;
}

// --------------------------------------------------------------- STAC search

bool AssetHref(const json& assets, const char* key, std::string& href) {
    if (!assets.contains(key) || !assets[key].is_object()) return false;
    const auto& a = assets[key];
    if (!a.contains("href") || !a["href"].is_string()) return false;
    href = a["href"].get<std::string>();
    return !href.empty();
}

// Surface reflectance = DN * scale + offset. Scenes processed with baseline
// 04.00 or later (since 2022) carry an offset of -0.1; older ones carry none.
void ReadScaling(const json& feature, Candidate& c) {
    double scale = 0.0001;
    double offset = 0.0;
    bool found = false;

    const auto& red = feature["assets"]["red"];
    if (red.contains("raster:bands") && red["raster:bands"].is_array() &&
        !red["raster:bands"].empty()) {
        const auto& rb = red["raster:bands"].front();
        if (rb.contains("scale") && rb["scale"].is_number()) scale = rb["scale"].get<double>();
        if (rb.contains("offset") && rb["offset"].is_number()) offset = rb["offset"].get<double>();
        found = rb.contains("scale") || rb.contains("offset");
    }
    if (!found && feature.contains("properties") &&
        feature["properties"].contains("s2:processing_baseline") &&
        feature["properties"]["s2:processing_baseline"].is_string()) {
        const double baseline =
            std::atof(feature["properties"]["s2:processing_baseline"].get<std::string>().c_str());
        offset = baseline >= 4.0 ? -0.1 : 0.0;
        found = true;
    }
    if (!found)
        std::cerr << "Warning: no reflectance offset in metadata for " << c.id
                  << "; assuming none.\n";
    if (scale <= 0.0) scale = 0.0001;
    c.dnDiv = static_cast<unsigned>(std::lround(1.0 / scale));
    c.dnOffset = static_cast<int>(std::lround(offset / scale));
}

std::vector<Candidate> SearchCatalogue(const BBox& box, int days) {
    const std::time_t now = std::time(nullptr);
    const std::time_t start = now - static_cast<std::time_t>(days) * 86400;
    const std::string range = IsoUtc(start) + "/" + IsoUtc(now);

    json query = {
        {"collections", json::array({"sentinel-2-l2a"})},
        {"bbox", json::array({box.west, box.south, box.east, box.north})},
        {"datetime", range},
        {"limit", kMaxCandidates},
        // Loose scene-level pre-filter; the real test is the crop-level SCL check.
        {"query", {{"eo:cloud_cover", {{"lte", 80}}}}},
        {"sortby", json::array({{{"field", "properties.datetime"}, {"direction", "desc"}}})}
    };

    const json result = json::parse(HttpPostJson(kStacSearch, query));
    if (!result.contains("features") || !result["features"].is_array())
        throw std::runtime_error("STAC response did not contain a feature list.");

    std::vector<Candidate> out;
    for (const auto& feature : result["features"]) {
        try {
            if (!feature.contains("assets") || !feature["assets"].is_object()) continue;
            const auto& assets = feature["assets"];
            Candidate c;
            if (!AssetHref(assets, "red", c.redHref) || !AssetHref(assets, "green", c.greenHref) ||
                !AssetHref(assets, "blue", c.blueHref) || !AssetHref(assets, "scl", c.sclHref))
                continue;
            c.id = feature.value("id", std::string("unknown-scene"));
            if (feature.contains("properties") && feature["properties"].is_object()) {
                const auto& p = feature["properties"];
                c.datetime = p.value("datetime", std::string());
                c.sceneCloud = p.value("eo:cloud_cover", -1.0);
            }
            ReadScaling(feature, c);
            out.push_back(std::move(c));
        } catch (const std::exception& ex) {
            std::cerr << "Skipping malformed STAC item: " << ex.what() << "\n";
        }
    }
    return out;
}

// Keep the same 4:3 centre while doubling the ground footprint at each zoom-out level.
BBox ScaleBBox(const BBox& box, double factor) {
    const double cx = (box.west + box.east) * 0.5;
    const double cy = (box.south + box.north) * 0.5;
    const double halfLon = (box.east - box.west) * 0.5 * factor;
    const double halfLat = (box.north - box.south) * 0.5 * factor;
    return {cx - halfLon, cy - halfLat, cx + halfLon, cy + halfLat};
}

// -------------------------------------------------------------- GDAL reading

// Crop the geographic window out of a remote COG and resample it to 320x240.
// /vsicurl/ makes GDAL use HTTP range requests rather than downloading the
// whole granule.
Dataset CropToMem(const std::string& href, const BBox& box, const char* resample) {
    const std::string vsiPath = "/vsicurl/" + href;
    Dataset source(GDALOpenEx(vsiPath.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                              nullptr, nullptr, nullptr));
    if (!source)
        throw std::runtime_error("GDAL could not open " + href + ": " + CPLGetLastErrorMsg());

    const std::vector<std::string> argv = {
        "-of", "MEM",
        "-outsize", std::to_string(kWidth), std::to_string(kHeight),
        "-projwin", std::to_string(box.west), std::to_string(box.north),
                    std::to_string(box.east), std::to_string(box.south),
        "-projwin_srs", "EPSG:4326",
        "-r", resample,
        "-b", "1"
    };
    char** args = nullptr;
    for (const auto& a : argv) args = CSLAddString(args, a.c_str());
    GDALTranslateOptions* options = GDALTranslateOptionsNew(args, nullptr);
    CSLDestroy(args);
    if (!options) throw std::runtime_error("Could not create GDAL crop options.");

    int usageError = FALSE;
    Dataset cropped(GDALTranslate("", static_cast<GDALDatasetH>(source.get()), options, &usageError));
    GDALTranslateOptionsFree(options);
    if (!cropped || usageError)
        throw std::runtime_error(std::string("GDAL crop failed: ") + CPLGetLastErrorMsg());
    return cropped;
}

std::vector<uint8_t> ReadByteWindow(const std::string& href, const BBox& box, const char* resample) {
    Dataset ds = CropToMem(href, box, resample);
    std::vector<uint8_t> px(static_cast<size_t>(kWidth) * kHeight);
    if (GDALRasterIO(GDALGetRasterBand(static_cast<GDALDatasetH>(ds.get()), 1), GF_Read,
                     0, 0, kWidth, kHeight, px.data(), kWidth, kHeight, GDT_Byte, 0, 0) != CE_None)
        throw std::runtime_error("Could not read 8-bit window from " + href);
    return px;
}

std::vector<uint16_t> ReadU16Window(const std::string& href, const BBox& box) {
    Dataset ds = CropToMem(href, box, "bilinear");
    std::vector<uint16_t> px(static_cast<size_t>(kWidth) * kHeight);
    if (GDALRasterIO(GDALGetRasterBand(static_cast<GDALDatasetH>(ds.get()), 1), GF_Read,
                     0, 0, kWidth, kHeight, px.data(), kWidth, kHeight, GDT_UInt16, 0, 0) != CE_None)
        throw std::runtime_error("Could not read 16-bit window from " + href);
    return px;
}

// SCL classes: 0 no data, 3 cloud shadow, 8/9 cloud (medium/high), 10 thin cirrus.
SclStats ComputeSclStats(const std::vector<uint8_t>& scl) {
    size_t cloud = 0, nodata = 0;
    for (uint8_t v : scl) {
        if (v == 0) ++nodata;
        else if (v == 3 || v == 8 || v == 9 || v == 10) ++cloud;
    }
    const double n = static_cast<double>(scl.size());
    return {100.0 * static_cast<double>(cloud) / n, 100.0 * static_cast<double>(nodata) / n};
}

// ------------------------------------------------------------------- source

class StacSource : public SceneSource {
public:
    StacSource() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Could not initialize libcurl.");
        GDALAllRegister();
        CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
        CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "3");
        CPLSetConfigOption("GDAL_HTTP_RETRY_DELAY", "2");
        CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "60");
    }
    ~StacSource() override { curl_global_cleanup(); }

    std::vector<SceneInfo> Search(const BBox& box, int days, int maxScreen,
                                  const Progress& pr) override {
        box_ = box;
        cands_.clear();
        scls_.clear();
        infos_.clear();
        if (pr.log) pr.log("Searching the Sentinel-2 catalogue...");
        const std::vector<Candidate> found = SearchCatalogue(box, days);

        int screened = 0;
        for (const Candidate& c : found) {
            if (screened >= maxScreen) break;
            if (pr.cancelled && pr.cancelled()) break;
            if (pr.log)
                pr.log("Checking clouds over the area: " + c.datetime.substr(0, 10) + " (" +
                       std::to_string(screened + 1) + "/" +
                       std::to_string(std::min<size_t>(found.size(), static_cast<size_t>(maxScreen))) + ")");
            try {
                std::vector<uint8_t> scl = ReadByteWindow(c.sclHref, box, "nearest");
                const SclStats st = ComputeSclStats(scl);
                SceneInfo si;
                si.id = c.id;
                si.datetime = c.datetime;
                si.sceneCloud = c.sceneCloud;
                si.cropCloudPct = st.cloudPct;
                si.nodataPct = st.nodataPct;
                si.dnOffset = c.dnOffset;
                si.dnDiv = c.dnDiv;
                cands_.push_back(c);
                scls_.push_back(std::move(scl));
                infos_.push_back(si);
                ++screened;
                if (pr.scene) pr.scene(si);
            } catch (const std::exception& ex) {
                std::cerr << "  skipping " << c.id << ": " << ex.what() << "\n";
                ++screened;
            }
        }
        return infos_;
    }

    SceneData Load(size_t index, const Progress& pr) override {
        if (index >= cands_.size()) throw std::runtime_error("No such scene.");
        const Candidate& c = cands_[index];
        SceneData d;
        d.info = infos_[index];
        for (int level = 0; level < kZoomLevelCount; ++level) {
            const double factor = static_cast<double>(1u << level);
            const BBox levelBox = ScaleBBox(box_, factor);
            const unsigned metresPerPixel = 10u << level;
            const std::string label = std::to_string(metresPerPixel) + " m/px";
            if (pr.log) pr.log("Downloading " + label + " red band...");
            std::vector<uint16_t> r = ReadU16Window(c.redHref, levelBox);
            if (pr.log) pr.log("Downloading " + label + " green band...");
            std::vector<uint16_t> g = ReadU16Window(c.greenHref, levelBox);
            if (pr.log) pr.log("Downloading " + label + " blue band...");
            std::vector<uint16_t> b = ReadU16Window(c.blueHref, levelBox);

            std::vector<uint8_t> scl;
            if (level == 0) {
                scl = scls_[index]; // reuse the mask already fetched during scene screening
            } else {
                if (pr.log) pr.log("Downloading " + label + " classification mask...");
                scl = ReadByteWindow(c.sclHref, levelBox, "nearest");
            }

            if (level == 0) {
                d.r = std::move(r);
                d.g = std::move(g);
                d.b = std::move(b);
                d.scl = std::move(scl);
            } else {
                SceneLevel extra;
                extra.r = std::move(r);
                extra.g = std::move(g);
                extra.b = std::move(b);
                extra.scl = std::move(scl);
                d.zoomOut.push_back(std::move(extra));
            }
        }
        return d;
    }

private:
    BBox box_{0, 0, 0, 0};
    std::vector<Candidate> cands_;
    std::vector<std::vector<uint8_t>> scls_;
    std::vector<SceneInfo> infos_;
};

}  // namespace

std::unique_ptr<SceneSource> MakeStacSource() { return std::make_unique<StacSource>(); }

}  // namespace sv
