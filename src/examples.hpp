#pragma once
#include "scene.hpp"

namespace fd {

struct Example { const char* id; const char* name; SceneFile (*make)(); };
SceneFile playgroundScene();
SceneFile planetScene();
SceneFile materialsScene();
inline const Example EXAMPLES[] = {
    {"playground", "4D playground", playgroundScene},
    {"planet", "Hypersphere planet (geodesic)", planetScene},
    {"materials", "Materials (every channel with a 0-1 checker)", materialsScene},
};

}  // namespace fd
