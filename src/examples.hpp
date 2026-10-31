#pragma once
#include "scene.hpp"

namespace fd {

struct Example { const char* id; const char* name; SceneFile (*make)(); };
SceneFile playgroundScene();
SceneFile planetScene();
inline const Example EXAMPLES[] = {
    {"playground", "4D playground", playgroundScene},
    {"planet", "Hypersphere planet (geodesic)", planetScene},
};

}  // namespace fd
