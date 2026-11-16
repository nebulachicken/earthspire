// Real scene source: Element 84 Earth Search (STAC) + GDAL /vsicurl/ reads.
#pragma once

#include <memory>

#include "types.hpp"

namespace sv {
std::unique_ptr<SceneSource> MakeStacSource();
}
