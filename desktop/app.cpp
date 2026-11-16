#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <iostream>

#include "svd_format.h"
#include "ui_html.hpp"

namespace sv {
namespace {

// ------------------------------------------------------------------ JSON out

std::string JStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
                else o += static_cast<char>(ch);
        }
    }
    return o + "\"";
}

std::string JNum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[48];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

HttpResponse Json(int status, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json; charset=utf-8";
    r.body = body;
    return r;
}

HttpResponse JsonError(int status, const std::string& message) {
    return Json(status, "{\"error\":" + JStr(message) + "}");
}

// ------------------------------------------------------------------- params

bool GetNum(const std::map<std::string, std::string>& m, const char* key, double& out) {
    const auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

GradeParams ParseGrade(const std::map<std::string, std::string>& m) {
    GradeParams p;
    double v;
    if (GetNum(m, "off", v)) { p.useSceneOffset = false; p.offset = v; }
    if (GetNum(m, "ev", v)) p.ev = v;
    if (GetNum(m, "mid", v)) p.midtones = v;
    if (GetNum(m, "sh", v)) p.shadows = v;
    if (GetNum(m, "hi", v)) p.highlights = v;
    if (GetNum(m, "con", v)) p.contrast = v;
    if (GetNum(m, "sat", v)) p.saturation = v;
    if (GetNum(m, "tmp", v)) p.temperature = v;
    const auto c = m.find("clouds");
    p.showClouds = c != m.end() && c->second == "1";
    ClampParams(p);
    return p;
}

void Percentiles(const std::vector<uint16_t>& plane, unsigned& p1, unsigned& p50, unsigned& p99) {
    if (plane.empty()) { p1 = p50 = p99 = 0; return; }
    std::vector<uint16_t> v(plane);
    auto at = [&](double frac) {
        const size_t k = std::min(v.size() - 1, static_cast<size_t>(frac * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return static_cast<unsigned>(v[k]);
    };
    p1 = at(0.01);
    p50 = at(0.50);
    p99 = at(0.99);
}

std::string ArrJson(const unsigned a[3]) {
    return "[" + std::to_string(a[0]) + "," + std::to_string(a[1]) + "," + std::to_string(a[2]) + "]";
}

}  // namespace

// ------------------------------------------------------------ public helpers

int ChooseScene(const std::vector<SceneInfo>& scenes, double maxCloudPct, bool allowCloudy) {
    constexpr double kMaxNodata = 2.0;
    int best = -1;
    for (size_t i = 0; i < scenes.size(); ++i) {
        const SceneInfo& s = scenes[i];
        if (s.nodataPct > kMaxNodata) continue;
        if (s.cropCloudPct <= maxCloudPct) return static_cast<int>(i);
        if (best < 0 || s.cropCloudPct < scenes[static_cast<size_t>(best)].cropCloudPct)
            best = static_cast<int>(i);
    }
    return allowCloudy ? best : -1;
}

namespace {

std::vector<uint8_t> PackPyramidLevel(const Graded& g) {
    const size_t px = static_cast<size_t>(g.info.width) * g.info.height;
    if (g.payload.empty() || g.info.width != kWidth || g.info.height != kHeight)
        throw std::runtime_error("A zoom level has invalid image data.");
    const std::vector<uint16_t> image = RenderRgb565(g, false);
    std::vector<uint8_t> raw(svd_pyramid_raw_size(g.info.width, g.info.height), 0);
    for (size_t i = 0; i < px; ++i) svd_put_u16(raw.data() + i * 2u, image[i]);

    if (g.info.has_scl) {
        const uint8_t* scl = g.payload.data() + px * 6u;
        uint8_t* mask = raw.data() + px * 2u;
        for (size_t i = 0; i < px; ++i) {
            uint8_t cls = 0;
            if (scl[i] == 0) cls = 1;
            else if (scl[i] == 3) cls = 2;
            else if (scl[i] == 8 || scl[i] == 9 || scl[i] == 10) cls = 3;
            mask[i >> 2] |= static_cast<uint8_t>(cls << ((i & 3u) * 2u));
        }
    }
    return raw;
}

struct EncodedLevel {
    std::vector<uint8_t> bytes;
    svd_pyramid_level meta{};
};

EncodedLevel EncodeLevel(const Graded& graded, uint16_t resolutionM) {
    EncodedLevel out;
    std::vector<uint8_t> raw = PackPyramidLevel(graded);
    out.meta.resolution_m = resolutionM;
    out.meta.raw_size = static_cast<uint32_t>(raw.size());
    out.meta.raw_crc32 = svd_crc32(0, raw.data(), raw.size());

    const size_t bound = svd_lzss_compress_bound(raw.size());
    std::vector<uint8_t> best(bound);
    size_t bestSize = 0;
    uint8_t bestCodec = SVD_CODEC_RAW;
    if (raw.size() <= best.size()) {
        /* Plain LZSS preserves the original byte stream. */
        if (svd_lzss_compress(raw.data(), raw.size(), best.data(), best.size(), &bestSize) == SVD_OK &&
            bestSize < raw.size()) {
            bestCodec = SVD_CODEC_LZSS;
        }
    }

    /* A lossless horizontal XOR predictor makes neighbouring RGB565 pixels
     * more repetitive for LZSS. Work right-to-left so source neighbours are
     * still the original values while the filter is applied. */
    std::vector<uint8_t> filtered = raw;
    for (uint32_t y = 0; y < graded.info.height; ++y) {
        const size_t row = static_cast<size_t>(y) * graded.info.width;
        for (uint32_t x = graded.info.width; x-- > 1;) {
            const size_t at = row + x;
            const uint16_t current = svd_get_u16(raw.data() + at * 2u);
            const uint16_t left = svd_get_u16(raw.data() + (at - 1u) * 2u);
            svd_put_u16(filtered.data() + at * 2u, static_cast<uint16_t>(current ^ left));
        }
    }
    std::vector<uint8_t> filteredCompressed(bound);
    size_t filteredSize = 0;
    if (svd_lzss_compress(filtered.data(), filtered.size(), filteredCompressed.data(),
                         filteredCompressed.size(), &filteredSize) == SVD_OK &&
        filteredSize < raw.size() &&
        (bestCodec == SVD_CODEC_RAW || filteredSize < bestSize)) {
        best.swap(filteredCompressed);
        bestSize = filteredSize;
        bestCodec = SVD_CODEC_LZSS_XOR;
    }

    if (bestCodec == SVD_CODEC_RAW) {
        out.bytes = std::move(raw);
    } else {
        best.resize(bestSize);
        out.bytes = std::move(best);
    }
    out.meta.codec = bestCodec;
    out.meta.stored_size = static_cast<uint32_t>(out.bytes.size());
    return out;
}

}  // namespace

void WriteSvdFile(const std::string& path, const Graded& g) {
    if (g.payload.empty()) throw std::runtime_error("Nothing to save: no scene is loaded.");
    if (g.zoomOut.size() + 1u > SVD_MAX_LEVELS)
        throw std::runtime_error("Too many zoom levels to save.");

    svd_pyramid_info info{};
    info.width = g.info.width;
    info.height = g.info.height;
    info.level_count = static_cast<uint8_t>(g.zoomOut.size() + 1u);
    info.cloud_permille = g.info.cloud_permille;
    info.nodata_permille = g.info.nodata_permille;
    std::memcpy(info.acquired, g.info.acquired, sizeof info.acquired);
    std::memcpy(info.scene_id, g.info.scene_id, sizeof info.scene_id);

    std::vector<EncodedLevel> encoded;
    encoded.reserve(info.level_count);
    encoded.push_back(EncodeLevel(g, 10));
    for (const GradedZoomLevel& z : g.zoomOut) {
        Graded temp;
        temp.info = z.info;
        temp.payload = z.payload;
        encoded.push_back(EncodeLevel(temp, z.resolution_m));
    }
    for (size_t i = 0; i < encoded.size(); ++i) info.levels[i] = encoded[i].meta;

    uint8_t header[SVD2_HEADER_SIZE];
    svd_pyramid_header_write(&info, header);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not create " + path);
    f.write(reinterpret_cast<const char*>(header), sizeof header);
    for (const EncodedLevel& z : encoded)
        f.write(reinterpret_cast<const char*>(z.bytes.data()), static_cast<std::streamsize>(z.bytes.size()));
    f.close();
    if (!f) throw std::runtime_error("Failed writing " + path);
}

void VerifySvdFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Could not re-open " + path);
    uint8_t magic[4];
    f.read(reinterpret_cast<char*>(magic), sizeof magic);
    if (!f) throw std::runtime_error("Read-back: file too short.");
    f.seekg(0);

    if (std::memcmp(magic, "SVD1", 4) == 0) {
        uint8_t header[SVD_HEADER_SIZE];
        f.read(reinterpret_cast<char*>(header), sizeof header);
        if (!f) throw std::runtime_error("Read-back: legacy header truncated.");
        svd_info oldInfo;
        int rc = svd_header_parse(header, &oldInfo);
        if (rc != SVD_OK) throw std::runtime_error(std::string("Read-back header: ") + svd_strerror(rc));
        std::vector<uint8_t> payload(oldInfo.payload_size);
        f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!f) throw std::runtime_error("Read-back: payload truncated.");
        rc = svd_payload_verify(&oldInfo, payload.data(), payload.size());
        if (rc != SVD_OK) throw std::runtime_error(std::string("Read-back payload: ") + svd_strerror(rc));
        return;
    }

    uint8_t header[SVD2_HEADER_SIZE];
    f.read(reinterpret_cast<char*>(header), sizeof header);
    if (!f) throw std::runtime_error("Read-back: SVD2 header truncated.");
    svd_pyramid_info info;
    int rc = svd_pyramid_header_parse(header, &info);
    if (rc != SVD_OK) throw std::runtime_error(std::string("Read-back header: ") + svd_strerror(rc));

    for (unsigned i = 0; i < info.level_count; ++i) {
        const svd_pyramid_level& z = info.levels[i];
        std::vector<uint8_t> stored(z.stored_size);
        std::vector<uint8_t> raw(z.raw_size);
        f.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(stored.size()));
        if (!f) throw std::runtime_error("Read-back: a zoom level is truncated.");
        rc = svd_pyramid_level_decode(&info, i, stored.data(), stored.size(), raw.data(), raw.size());
        if (rc != SVD_OK)
            throw std::runtime_error("Read-back level " + std::to_string(i) + ": " + svd_strerror(rc));
    }
    char extra;
    if (f.read(&extra, 1)) throw std::runtime_error("Read-back: unexpected trailing data after the zoom pyramid.");
}

// --------------------------------------------------------------- Controller

Controller::Controller(SceneSource& source, std::string outPath, double startLat, double startLon)
    : source_(source), outPath_(std::move(outPath)), startLat_(startLat), startLon_(startLon) {}

Controller::~Controller() {
    quit_ = true;
    JoinWorker();
}

bool Controller::Busy() const { return state_ == State::Searching || state_ == State::Loading; }

void Controller::JoinWorker() {
    if (worker_.joinable()) worker_.join();
}

void Controller::SetMessage(const std::string& m) {
    std::lock_guard<std::mutex> lock(mu_);
    message_ = m;
}

HttpResponse Controller::Handle(const HttpRequest& rq) {
    // Only answer requests addressed to this machine's loopback name, and only
    // accept POSTs from our own page (defends against other web pages talking
    // to the local server).
    const std::string portText = std::to_string(port_);
    auto hostIt = rq.headers.find("host");
    if (hostIt == rq.headers.end() ||
        (hostIt->second != "127.0.0.1:" + portText && hostIt->second != "localhost:" + portText))
        return JsonError(403, "unexpected Host header");
    if (rq.method == "POST") {
        auto originIt = rq.headers.find("origin");
        if (originIt != rq.headers.end() &&
            originIt->second != "http://127.0.0.1:" + portText &&
            originIt->second != "http://localhost:" + portText)
            return JsonError(403, "unexpected Origin header");
    }

    try {
        if (rq.method == "GET" && (rq.path == "/" || rq.path == "/index.html")) {
            HttpResponse r;
            r.contentType = "text/html; charset=utf-8";
            r.body = kIndexHtml;
            return r;
        }
        if (rq.method == "GET" && rq.path == "/api/status") return Status();
        if (rq.method == "GET" && rq.path == "/api/preview.bmp") return Preview(ParseForm(rq.query));
        if (rq.method == "POST" && rq.path == "/api/search") return StartSearch(ParseForm(rq.body));
        if (rq.method == "POST" && rq.path == "/api/load") return StartLoad(ParseForm(rq.body));
        if (rq.method == "POST" && rq.path == "/api/save") return Save(ParseForm(rq.body));
        if (rq.method == "POST" && rq.path == "/api/quit") {
            quit_ = true;
            return Json(200, "{\"ok\":true}");
        }
    } catch (const std::exception& e) {
        return JsonError(500, e.what());
    }
    return JsonError(404, "not found");
}

HttpResponse Controller::Status() {
    std::lock_guard<std::mutex> lock(mu_);
    const char* st = "idle";
    if (state_ == State::Searching) st = "searching";
    else if (state_ == State::Loading) st = "loading";
    else if (state_ == State::Error) st = "error";

    std::ostringstream o;
    o << "{\"state\":" << JStr(st) << ",\"message\":" << JStr(message_)
      << ",\"error\":" << JStr(error_) << ",\"out\":" << JStr(outPath_)
      << ",\"defaultLat\":" << JNum(startLat_) << ",\"defaultLon\":" << JNum(startLon_)
      << ",\"scenes\":[";
    for (size_t i = 0; i < scenes_.size(); ++i) {
        const SceneInfo& s = scenes_[i];
        if (i) o << ",";
        o << "{\"i\":" << i << ",\"id\":" << JStr(s.id) << ",\"date\":" << JStr(s.datetime.substr(0, 10))
          << ",\"cloud\":" << JNum(s.cropCloudPct) << ",\"nodata\":" << JNum(s.nodataPct)
          << ",\"sceneCloud\":" << JNum(s.sceneCloud) << "}";
    }
    o << "],\"loaded\":";
    if (scene_) {
        o << "{\"index\":" << loadedIndex_ << ",\"id\":" << JStr(scene_->info.id)
          << ",\"date\":" << JStr(scene_->info.datetime.substr(0, 10))
          << ",\"cloud\":" << JNum(scene_->info.cropCloudPct)
          << ",\"offset\":" << JNum(scene_->info.dnDiv ? static_cast<double>(scene_->info.dnOffset) / scene_->info.dnDiv : 0.0)
          << ",\"dnOffset\":" << scene_->info.dnOffset << ",\"dnDiv\":" << scene_->info.dnDiv
          << ",\"rawP1\":" << ArrJson(rawP1_) << ",\"rawP50\":" << ArrJson(rawP50_)
          << ",\"rawP99\":" << ArrJson(rawP99_) << "}";
    } else {
        o << "null";
    }
    o << "}";
    return Json(200, o.str());
}

HttpResponse Controller::StartSearch(const std::map<std::string, std::string>& form) {
    double lat = 0, lon = 0, days = 120, maxScreen = 12;
    if (!GetNum(form, "lat", lat) || !GetNum(form, "lon", lon))
        return JsonError(400, "lat and lon are required numbers");
    if (lat < -80.0 || lat > 80.0) return JsonError(400, "latitude must be between -80 and 80");
    if (lon < -180.0 || lon > 180.0) return JsonError(400, "longitude must be between -180 and 180");
    GetNum(form, "days", days);
    GetNum(form, "maxscreen", maxScreen);
    const int daysI = static_cast<int>(std::max(1.0, std::min(3650.0, days)));
    const int screenI = static_cast<int>(std::max(1.0, std::min(40.0, maxScreen)));

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (Busy()) return JsonError(409, "Another job is still running.");
        state_ = State::Searching;
        error_.clear();
        message_ = "Searching for scenes...";
        scenes_.clear();
        scene_.reset();
        loadedIndex_ = -1;
    }
    JoinWorker();

    const BBox box = MakeBBox(lat, lon);
    worker_ = std::thread([this, box, daysI, screenI] {
        Progress pr;
        pr.log = [this](const std::string& m) { SetMessage(m); };
        pr.scene = [this](const SceneInfo& s) {
            std::lock_guard<std::mutex> lock(mu_);
            scenes_.push_back(s);
        };
        pr.cancelled = [this] { return quit_.load(); };
        try {
            std::vector<SceneInfo> found = source_.Search(box, daysI, screenI, pr);
            std::lock_guard<std::mutex> lock(mu_);
            scenes_ = std::move(found);
            state_ = State::Idle;
            message_ = scenes_.empty() ? "No scenes found. Try a longer look-back period."
                                       : "Found " + std::to_string(scenes_.size()) + " scenes.";
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mu_);
            state_ = State::Error;
            error_ = e.what();
            message_ = "Search failed.";
        }
    });
    return Json(200, "{\"ok\":true}");
}

HttpResponse Controller::StartLoad(const std::map<std::string, std::string>& form) {
    double idx = -1;
    if (!GetNum(form, "index", idx)) return JsonError(400, "index is required");
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (Busy()) return JsonError(409, "Another job is still running.");
        if (idx < 0 || idx >= static_cast<double>(scenes_.size())) return JsonError(400, "no such scene");
        state_ = State::Loading;
        error_.clear();
        message_ = "Downloading " + scenes_[static_cast<size_t>(idx)].datetime.substr(0, 10) + "...";
    }
    JoinWorker();

    const size_t index = static_cast<size_t>(idx);
    worker_ = std::thread([this, index] {
        Progress pr;
        pr.log = [this](const std::string& m) { SetMessage(m); };
        pr.cancelled = [this] { return quit_.load(); };
        try {
            auto data = std::make_shared<SceneData>(source_.Load(index, pr));
            unsigned p1[3], p50[3], p99[3];
            Percentiles(data->r, p1[0], p50[0], p99[0]);
            Percentiles(data->g, p1[1], p50[1], p99[1]);
            Percentiles(data->b, p1[2], p50[2], p99[2]);
            std::lock_guard<std::mutex> lock(mu_);
            scene_ = std::move(data);
            loadedIndex_ = static_cast<int>(index);
            std::memcpy(rawP1_, p1, sizeof p1);
            std::memcpy(rawP50_, p50, sizeof p50);
            std::memcpy(rawP99_, p99, sizeof p99);
            state_ = State::Idle;
            message_ = "Loaded " + scene_->info.datetime.substr(0, 10) + ".";
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mu_);
            state_ = State::Error;
            error_ = e.what();
            message_ = "Download failed.";
        }
    });
    return Json(200, "{\"ok\":true}");
}

HttpResponse Controller::Preview(const std::map<std::string, std::string>& q) {
    std::shared_ptr<SceneData> scene;
    {
        std::lock_guard<std::mutex> lock(mu_);
        scene = scene_;
    }
    if (!scene) return JsonError(404, "No scene is loaded yet.");

    const GradeParams p = ParseGrade(q);
    const Graded g = GradeScene(*scene, p, false);
    HttpResponse r;
    r.contentType = "image/bmp";
    const std::vector<uint8_t> bmp = BuildBmp24(RenderRgb565(g, p.showClouds));
    r.body.assign(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    r.headers.emplace_back("X-Stats",
        "{\"clippedBlack\":" + JNum(g.stats.clippedBlackPct) + ",\"clippedWhite\":" +
        JNum(g.stats.clippedWhitePct) + ",\"offset\":" + JNum(g.stats.offset) + "}");
    return r;
}

HttpResponse Controller::Save(const std::map<std::string, std::string>& form) {
    std::shared_ptr<SceneData> scene;
    {
        std::lock_guard<std::mutex> lock(mu_);
        scene = scene_;
    }
    if (!scene) return JsonError(404, "No scene is loaded yet.");

    const auto raw = form.find("raw");
    const bool rawMode = raw != form.end() && raw->second == "1";
    const Graded g = rawMode ? RawScene(*scene) : GradeScene(*scene, ParseGrade(form));
    WriteSvdFile(outPath_, g);
    VerifySvdFile(outPath_);

    std::error_code ec;
    const std::string full = std::filesystem::absolute(outPath_, ec).string();
    const size_t bytes = static_cast<size_t>(std::filesystem::file_size(outPath_));
    return Json(200, "{\"ok\":true,\"path\":" + JStr(ec ? outPath_ : full) +
                     ",\"bytes\":" + std::to_string(bytes) +
                     ",\"raw\":" + (rawMode ? "true" : "false") + "}");
}

// ------------------------------------------------------------ control panel

namespace {
void OpenBrowser(const std::string& url) {
#ifdef _WIN32
    const std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    const std::string cmd = "open \"" + url + "\" >/dev/null 2>&1 &";
#else
    const std::string cmd = "xdg-open \"" + url + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(cmd.c_str()) != 0)
        std::cerr << "(Could not open a browser automatically; open the address above yourself.)\n";
}
}  // namespace

int RunControlPanel(SceneSource& source, const UiOptions& opt) {
    HttpServer server;
    std::string err;
    int port = opt.port;
    bool listening = false;
    for (int i = 0; i < 10 && !listening; ++i) {
        listening = server.Listen(opt.port + i, err);
        if (listening) port = opt.port + i;
    }
    if (!listening) {
        std::cerr << "ERROR: could not start the control panel: " << err << "\n";
        return 1;
    }

    Controller controller(source, opt.outPath, opt.lat, opt.lon);
    controller.SetPort(port);
    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::cout << "Control panel running at " << url << "\n"
              << "Leave this window open while you use it; press Quit in the page to stop.\n";
    if (opt.openBrowser) OpenBrowser(url);

    server.Serve([&](const HttpRequest& rq) {
        HttpResponse rs = controller.Handle(rq);
        if (controller.QuitRequested()) server.Stop();
        return rs;
    });
    std::cout << "Stopped.\n";
    return 0;
}

}  // namespace sv
