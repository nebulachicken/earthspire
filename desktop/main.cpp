// SentinelFetch 0.4.2 - Sentinel-2 data for the calculator viewer.
//
//   SentinelFetch                 start the control panel (pick a place on a
//                                 map, adjust colours, preview, save)
//   SentinelFetch --lat .. --lon ..  [--out FILE] [--preview FILE] ...
//                                 command-line mode, no browser needed
//
// The saved SVD2 file contains five 320x240 image levels (10..160 m/pixel).
// Colour adjustments are baked into RGB565 pixels; the calculator can change
// brightness, toggle cloud overlays and switch levels without re-downloading.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "app.hpp"
#include "grade.hpp"
#include "source_stac.hpp"

namespace {

struct Options {
    bool ui = false;
    bool uiForced = false;
    bool noBrowser = false;
    int port = 8765;
    double lat = 28.4800;
    double lon = -80.5700;
    std::string outPath = "sentinelviewer.svd.tns";
    std::string previewPath;
    double maxCloudPct = 10.0;
    int days = 120;
    bool allowCloudy = false;
    bool raw = false;
};

void PrintUsage() {
    std::cout <<
        "SentinelFetch - Sentinel-2 data for the calculator viewer\n\n"
        "With no arguments (or --ui) it starts a control panel in your browser:\n"
        "pick a place on a map, adjust colours, preview, and save.\n\n"
        "  --ui              start the control panel\n"
        "  --port N          control panel port, default 8765\n"
        "  --no-browser      do not open the browser automatically\n\n"
        "Command-line mode (used when any of these is given):\n"
        "  --lat DEG --lon DEG   centre of the 3.2 x 2.4 km area (default starting position)\n"
        "  --out FILE        output file (default sentinelviewer.svd.tns)\n"
        "  --max-cloud PCT   max cloud+shadow share of the area, default 10\n"
        "  --days N          look back N days, default 120\n"
        "  --allow-cloudy    if nothing meets --max-cloud, use the least cloudy scene\n"
        "  --preview FILE    also write a BMP preview\n"
        "  --raw             bypass desktop colour grading (use the default display curve)\n"
        "  --help\n";
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    bool cli = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " needs a value.");
            return argv[++i];
        };
        if (a == "--ui") { opt.uiForced = true; }
        else if (a == "--no-browser") { opt.noBrowser = true; }
        else if (a == "--port") { opt.port = std::max(1, std::atoi(need("--port"))); }
        else if (a == "--lat") { opt.lat = std::atof(need("--lat")); cli = true; }
        else if (a == "--lon") { opt.lon = std::atof(need("--lon")); cli = true; }
        else if (a == "--out") { opt.outPath = need("--out"); cli = true; }
        else if (a == "--preview") { opt.previewPath = need("--preview"); cli = true; }
        else if (a == "--max-cloud") { opt.maxCloudPct = std::atof(need("--max-cloud")); cli = true; }
        else if (a == "--days") { opt.days = std::max(1, std::atoi(need("--days"))); cli = true; }
        else if (a == "--allow-cloudy") { opt.allowCloudy = true; cli = true; }
        else if (a == "--raw") { opt.raw = true; cli = true; }
        else if (a == "--help" || a == "-h") return false;
        else throw std::runtime_error("Unknown argument: " + a);
    }
    opt.ui = opt.uiForced || !cli;
    if (opt.lat < -80.0 || opt.lat > 80.0) throw std::runtime_error("--lat must be between -80 and 80.");
    if (opt.lon < -180.0 || opt.lon > 180.0) throw std::runtime_error("--lon must be between -180 and 180.");
    return true;
}

int RunCommandLine(const Options& opt) {
    auto source = sv::MakeStacSource();
    const sv::BBox box = sv::MakeBBox(opt.lat, opt.lon);

    bool acceptable = false;
    sv::Progress pr;
    pr.log = [](const std::string& m) { std::cout << m << "\n"; };
    pr.scene = [&](const sv::SceneInfo& s) {
        std::cout << "  " << s.datetime.substr(0, 10) << "  " << s.id << "  crop cloud "
                  << s.cropCloudPct << "%  nodata " << s.nodataPct << "%\n";
        if (s.nodataPct <= 2.0 && s.cropCloudPct <= opt.maxCloudPct) acceptable = true;
    };
    pr.cancelled = [&] { return acceptable; };       // stop at the first good scene

    const std::vector<sv::SceneInfo> found = source->Search(box, opt.days, 40, pr);
    if (found.empty())
        throw std::runtime_error("No scenes with the required assets in the last " +
                                 std::to_string(opt.days) + " days. Try a larger --days.");
    const int pick = sv::ChooseScene(found, opt.maxCloudPct, opt.allowCloudy);
    if (pick < 0)
        throw std::runtime_error("No scene had <= " + std::to_string(opt.maxCloudPct) +
                                 "% cloud over the area. Raise --max-cloud, use --allow-cloudy, or --days.");

    const sv::SceneData data = source->Load(static_cast<size_t>(pick), pr);
    std::cout << "Selected " << data.info.id << " (" << data.info.datetime << ")\n";

    const sv::Graded g = opt.raw ? sv::RawScene(data) : sv::GradeScene(data, sv::GradeParams());
    sv::WriteSvdFile(opt.outPath, g);
    sv::VerifySvdFile(opt.outPath);
    const auto fileBytes = std::filesystem::file_size(opt.outPath);
    std::cout << "Wrote " << opt.outPath << " (" << fileBytes
              << " bytes, " << (g.zoomOut.size() + 1u)
              << " zoom levels, checksums verified).\n";

    if (!opt.previewPath.empty()) {
        const std::vector<uint8_t> bmp = sv::BuildBmp24(sv::RenderRgb565(g, false));
        std::ofstream f(opt.previewPath, std::ios::binary);
        if (!f) throw std::runtime_error("Could not create " + opt.previewPath);
        f.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        std::cout << "Wrote preview " << opt.previewPath << "\n";
    }
    std::cout << "Next: send " << opt.outPath << " to the calculator (see README).\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options opt;
        if (!ParseArgs(argc, argv, opt)) {
            PrintUsage();
            return 0;
        }
        if (opt.ui) {
            auto source = sv::MakeStacSource();
            sv::UiOptions ui;
            ui.port = opt.port;
            ui.openBrowser = !opt.noBrowser;
            ui.outPath = opt.outPath;
            ui.lat = opt.lat;
            ui.lon = opt.lon;
            return sv::RunControlPanel(*source, ui);
        }
        return RunCommandLine(opt);
    } catch (const std::exception& ex) {
        std::cerr << "\nERROR: " << ex.what() << "\nNo placeholder data was written.\n";
        return 1;
    }
}
