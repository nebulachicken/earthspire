// Controller for the local control panel: owns the session state, runs the
// slow search/download work on a background thread, and answers the page's
// HTTP requests. Free of curl/GDAL so it can be tested with a fake source.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "grade.hpp"
#include "httpd.hpp"
#include "types.hpp"

namespace sv {

// Index of the scene to use: newest one within maxCloud (and with little
// no-data), else the least cloudy usable one if allowCloudy, else -1.
int ChooseScene(const std::vector<SceneInfo>& scenes, double maxCloudPct, bool allowCloudy);

// Write header + payload to `path` and read it back through the same checks
// the calculator applies. Throws std::runtime_error on any problem.
void WriteSvdFile(const std::string& path, const Graded& g);
void VerifySvdFile(const std::string& path);

class Controller {
public:
    Controller(SceneSource& source, std::string outPath, double startLat, double startLon);
    ~Controller();

    void SetPort(int port) { port_ = port; }
    HttpResponse Handle(const HttpRequest& request);
    bool QuitRequested() const { return quit_; }

private:
    enum class State { Idle, Searching, Loading, Error };

    HttpResponse Status();
    HttpResponse StartSearch(const std::map<std::string, std::string>& form);
    HttpResponse StartLoad(const std::map<std::string, std::string>& form);
    HttpResponse Preview(const std::map<std::string, std::string>& q);
    HttpResponse Save(const std::map<std::string, std::string>& form);
    bool Busy() const;                     // caller holds mu_
    void JoinWorker();                     // caller must not hold mu_
    void SetMessage(const std::string& m);

    SceneSource& source_;
    std::string outPath_;
    double startLat_, startLon_;
    int port_ = 0;

    mutable std::mutex mu_;
    State state_ = State::Idle;
    std::string message_ = "Ready.";
    std::string error_;
    std::vector<SceneInfo> scenes_;
    std::shared_ptr<SceneData> scene_;
    int loadedIndex_ = -1;
    unsigned rawP1_[3] = {0, 0, 0}, rawP50_[3] = {0, 0, 0}, rawP99_[3] = {0, 0, 0};

    std::thread worker_;
    std::atomic<bool> quit_{false};
};

struct UiOptions {
    int port = 8765;
    bool openBrowser = true;
    std::string outPath = "capecanaveral.svd.tns";
    double lat = 28.4800;     // initial map centre (Cape Canaveral)
    double lon = -80.5700;
};

// Start the local control panel and block until the user presses Quit.
// Returns a process exit code.
int RunControlPanel(SceneSource& source, const UiOptions& opt);

}  // namespace sv
