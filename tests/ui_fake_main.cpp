// Runs the real control panel (server + controller + grading) against fake
// scenes, so the UI can be exercised without curl, GDAL or the network.
//   ui_fake [port]
#include <cstdlib>
#include <iostream>
#include "../desktop/app.hpp"
#include "fake_source.hpp"

int main(int argc, char** argv) {
    sv::FakeSource source;
    sv::UiOptions opt;
    opt.port = argc > 1 ? std::atoi(argv[1]) : 8765;
    opt.openBrowser = false;
    opt.outPath = argc > 2 ? argv[2] : "fake_out.svd.tns";
    return sv::RunControlPanel(source, opt);
}
