// Cape Canaveral Sentinel data fetcher (desktop side), prototype 0.2.
//
// Downloads RAW Sentinel-2 L2A data for a 3.2 km x 2.4 km window and writes it
// to an SVD1 file. It does not build an image: the calculator client converts
// the reflectance data into pixels itself.
//
// Data fetched (Element 84 Earth Search, collection sentinel-2-l2a):
//   red / green / blue  B04, B03, B02 surface reflectance, 16-bit, 10 m
//   scl                 Scene Classification Layer, 8-bit, 20 m
// The SCL layer is used here to measure cloud cover over the crop itself
// (not just the whole-scene figure), and is also sent to the calculator.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <nlohmann/json.hpp>

#include "svd_format.h"
#include "svd_render.h"

using json = nlohmann::json;

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

// 3.2 km x 2.4 km around Cape Canaveral. Longitude span is widened by
// 1/cos(28.48 deg) so the box is 4:3 on the ground.
constexpr double kWest = -80.58645;
constexpr double kSouth = 28.4692;
constexpr double kEast = -80.55355;
constexpr double kNorth = 28.4908;

constexpr const char* kStacSearch = "https://earth-search.aws.element84.com/v1/search";

constexpr double kMaxNodataPct = 2.0;   // reject partial-footprint scenes
constexpr int kMaxCandidates = 40;

struct Options {
    std::string outPath = "capecanaveral.svd.tns";
    std::string previewPath;            // optional BMP
    double maxCloudPct = 10.0;          // cloud + shadow share of the crop
    int days = 120;                     // look-back window
    bool allowCloudy = false;           // fall back to least-cloudy candidate
};

struct Candidate {
    std::string id;
    std::string datetime;
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CapeCanaveralSentinelFetch/0.2");
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

std::vector<Candidate> SearchScenes(const Options& opt) {
    std::cout << "Searching Sentinel-2 L2A scenes over Cape Canaveral...\n";

    const std::time_t now = std::time(nullptr);
    const std::time_t start = now - static_cast<std::time_t>(opt.days) * 86400;
    const std::string range = IsoUtc(start) + "/" + IsoUtc(now);

    json query = {
        {"collections", json::array({"sentinel-2-l2a"})},
        {"bbox", json::array({kWest, kSouth, kEast, kNorth})},
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
    std::cout << "Candidates with red/green/blue/scl assets: " << out.size() << "\n";
    return out;
}

// -------------------------------------------------------------- GDAL reading

// Crop the geographic window out of a remote COG and resample it to 320x240.
// /vsicurl/ makes GDAL use HTTP range requests rather than downloading the
// whole granule.
Dataset CropToMem(const std::string& href, const char* resample) {
    const std::string vsiPath = "/vsicurl/" + href;
    Dataset source(GDALOpenEx(vsiPath.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                              nullptr, nullptr, nullptr));
    if (!source)
        throw std::runtime_error("GDAL could not open " + href + ": " + CPLGetLastErrorMsg());

    const std::vector<std::string> argv = {
        "-of", "MEM",
        "-outsize", std::to_string(kWidth), std::to_string(kHeight),
        "-projwin", std::to_string(kWest), std::to_string(kNorth),
                    std::to_string(kEast), std::to_string(kSouth),
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

std::vector<uint8_t> ReadByteWindow(const std::string& href, const char* resample) {
    Dataset ds = CropToMem(href, resample);
    std::vector<uint8_t> px(static_cast<size_t>(kWidth) * kHeight);
    if (GDALRasterIO(GDALGetRasterBand(static_cast<GDALDatasetH>(ds.get()), 1), GF_Read,
                     0, 0, kWidth, kHeight, px.data(), kWidth, kHeight, GDT_Byte, 0, 0) != CE_None)
        throw std::runtime_error("Could not read 8-bit window from " + href);
    return px;
}

std::vector<uint16_t> ReadU16Window(const std::string& href) {
    Dataset ds = CropToMem(href, "bilinear");
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

// ------------------------------------------------------------------- output

void WriteBmp24(const std::string& path, const std::vector<uint16_t>& rgb565) {
    const int rowBytes = (kWidth * 3 + 3) & ~3;
    const uint32_t imageSize = static_cast<uint32_t>(rowBytes) * kHeight;
    const uint32_t fileSize = 54 + imageSize;
    std::vector<uint8_t> out(fileSize, 0);
    auto put32 = [&](size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i) out[o + i] = static_cast<uint8_t>(v >> (8 * i));
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
        uint8_t* row = out.data() + 54 + static_cast<size_t>(kHeight - 1 - y) * rowBytes;
        for (int x = 0; x < kWidth; ++x) {
            const unsigned v = rgb565[static_cast<size_t>(y) * kWidth + x];
            const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            row[x * 3 + 0] = static_cast<uint8_t>((b << 3) | (b >> 2));
            row[x * 3 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
            row[x * 3 + 2] = static_cast<uint8_t>((r << 3) | (r >> 2));
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not create " + path);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) throw std::runtime_error("Failed writing " + path);
}

void CopyText(char* dst, size_t dstSize, const std::string& src) {
    std::memset(dst, 0, dstSize);
    std::memcpy(dst, src.data(), std::min(src.size(), dstSize - 1));
}

void WriteSvd(const std::string& path, const Candidate& c, const SclStats& stats,
              const std::vector<uint16_t>& r, const std::vector<uint16_t>& g,
              const std::vector<uint16_t>& b, const std::vector<uint8_t>& scl) {
    const size_t px = static_cast<size_t>(kWidth) * kHeight;
    std::vector<uint8_t> payload(svd_expected_payload_size(kWidth, kHeight, 1));
    if (payload.size() != px * 7) throw std::logic_error("Unexpected payload size.");

    const std::vector<uint16_t>* planes[3] = {&r, &g, &b};
    for (int p = 0; p < 3; ++p)
        for (size_t i = 0; i < px; ++i)
            svd_put_u16(payload.data() + static_cast<size_t>(p) * px * 2 + i * 2, (*planes[p])[i]);
    std::memcpy(payload.data() + px * 6, scl.data(), px);

    svd_info info;
    std::memset(&info, 0, sizeof info);
    info.width = kWidth;
    info.height = kHeight;
    info.has_scl = 1;
    info.dn_offset = c.dnOffset;
    info.dn_div = c.dnDiv;
    info.cloud_permille = static_cast<uint16_t>(std::lround(stats.cloudPct * 10.0));
    info.nodata_permille = static_cast<uint16_t>(std::lround(stats.nodataPct * 10.0));
    CopyText(info.acquired, sizeof info.acquired,
             c.datetime.size() >= 19 ? c.datetime.substr(0, 19) + "Z" : c.datetime);
    CopyText(info.scene_id, sizeof info.scene_id, c.id);
    info.payload_size = static_cast<uint32_t>(payload.size());
    info.payload_crc32 = svd_crc32(0, payload.data(), payload.size());

    uint8_t header[SVD_HEADER_SIZE];
    svd_header_write(&info, header);

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not create " + path);
    f.write(reinterpret_cast<const char*>(header), sizeof header);
    f.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    f.close();
    if (!f) throw std::runtime_error("Failed writing " + path);
}

// Re-read the file we just wrote and verify it the same way the calculator will.
svd_info VerifyFile(const std::string& path, std::vector<uint8_t>& payload) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not re-open " + path);
    uint8_t header[SVD_HEADER_SIZE];
    f.read(reinterpret_cast<char*>(header), sizeof header);
    if (!f) throw std::runtime_error("Read-back: file too short.");
    svd_info info;
    int rc = svd_header_parse(header, &info);
    if (rc != SVD_OK) throw std::runtime_error(std::string("Read-back header: ") + svd_strerror(rc));
    payload.resize(info.payload_size);
    f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!f) throw std::runtime_error("Read-back: payload truncated.");
    rc = svd_payload_verify(&info, payload.data(), payload.size());
    if (rc != SVD_OK) throw std::runtime_error(std::string("Read-back payload: ") + svd_strerror(rc));
    return info;
}

// ---------------------------------------------------------------------- main

void PrintUsage() {
    std::cout <<
        "SentinelFetch - download raw Sentinel-2 data for the calculator viewer\n\n"
        "  --out FILE        output file (default capecanaveral.svd.tns)\n"
        "  --max-cloud PCT   max cloud+shadow share of the crop, default 10\n"
        "  --days N          look back N days, default 120\n"
        "  --allow-cloudy    if nothing meets --max-cloud, use the least cloudy scene\n"
        "  --preview FILE    also write a BMP preview (same conversion as the calculator)\n"
        "  --help\n";
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " needs a value.");
            return argv[++i];
        };
        if (a == "--out") opt.outPath = need("--out");
        else if (a == "--preview") opt.previewPath = need("--preview");
        else if (a == "--max-cloud") opt.maxCloudPct = std::atof(need("--max-cloud"));
        else if (a == "--days") opt.days = std::max(1, std::atoi(need("--days")));
        else if (a == "--allow-cloudy") opt.allowCloudy = true;
        else if (a == "--help" || a == "-h") return false;
        else throw std::runtime_error("Unknown argument: " + a);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    int exitCode = 0;
    try {
        Options opt;
        if (!ParseArgs(argc, argv, opt)) {
            PrintUsage();
            return 0;
        }
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("Could not initialize libcurl.");

        GDALAllRegister();
        CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
        CPLSetConfigOption("CPL_VSIL_CURL_ALLOWED_EXTENSIONS", ".tif,.tiff");
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "3");
        CPLSetConfigOption("GDAL_HTTP_RETRY_DELAY", "2");
        CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "60");

        const std::vector<Candidate> candidates = SearchScenes(opt);
        if (candidates.empty())
            throw std::runtime_error("No scenes with the required assets in the last " +
                                     std::to_string(opt.days) + " days. Try a larger --days.");

        // Screen newest-first using the small SCL layer over the crop itself.
        int chosen = -1;
        int bestIdx = -1;
        SclStats chosenStats{}, bestStats{};
        std::vector<uint8_t> chosenScl, bestScl;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& c = candidates[i];
            try {
                std::vector<uint8_t> scl = ReadByteWindow(c.sclHref, "nearest");
                const SclStats s = ComputeSclStats(scl);
                std::cout << "  " << c.datetime.substr(0, 10) << "  " << c.id
                          << "  crop cloud " << s.cloudPct << "%  nodata " << s.nodataPct << "%\n";
                if (s.nodataPct > kMaxNodataPct) continue;
                if (bestIdx < 0 || s.cloudPct < bestStats.cloudPct) {
                    bestIdx = static_cast<int>(i);
                    bestStats = s;
                    bestScl = scl;
                }
                if (s.cloudPct <= opt.maxCloudPct) {
                    chosen = static_cast<int>(i);
                    chosenStats = s;
                    chosenScl = std::move(scl);
                    break;
                }
            } catch (const std::exception& ex) {
                std::cerr << "  skipping " << c.id << ": " << ex.what() << "\n";
            }
        }
        if (chosen < 0) {
            if (opt.allowCloudy && bestIdx >= 0) {
                chosen = bestIdx;
                chosenStats = bestStats;
                chosenScl = bestScl;
                std::cout << "No scene met --max-cloud; using the least cloudy one.\n";
            } else {
                throw std::runtime_error(
                    "No scene had <= " + std::to_string(opt.maxCloudPct) +
                    "% cloud over the crop. Raise --max-cloud, use --allow-cloudy, or --days.");
            }
        }

        const Candidate& scene = candidates[static_cast<size_t>(chosen)];
        std::cout << "Selected " << scene.id << " (" << scene.datetime << ")\n"
                  << "Downloading red/green/blue windows...\n";
        const auto r = ReadU16Window(scene.redHref);
        const auto g = ReadU16Window(scene.greenHref);
        const auto b = ReadU16Window(scene.blueHref);

        WriteSvd(opt.outPath, scene, chosenStats, r, g, b, chosenScl);
        std::vector<uint8_t> payload;
        const svd_info info = VerifyFile(opt.outPath, payload);
        std::cout << "Wrote " << opt.outPath << " (" << SVD_HEADER_SIZE + info.payload_size
                  << " bytes, checksums verified).\n";

        if (!opt.previewPath.empty()) {
            std::vector<uint16_t> img(static_cast<size_t>(kWidth) * kHeight);
            svd_render_opts ro;
            svd_render_defaults(&ro);
            if (svd_render_rgb565(&info, payload.data(), &ro, img.data()) != SVD_OK)
                throw std::runtime_error("Preview conversion failed.");
            WriteBmp24(opt.previewPath, img);
            std::cout << "Wrote preview " << opt.previewPath << "\n";
        }
        std::cout << "Next: send " << opt.outPath << " to the calculator (see README).\n";
    } catch (const std::exception& ex) {
        std::cerr << "\nERROR: " << ex.what() << "\nNo placeholder data was written.\n";
        exitCode = 1;
    }
    curl_global_cleanup();
    return exitCode;
}
