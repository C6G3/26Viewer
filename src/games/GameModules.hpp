#pragma once

#include "viewer/core/GameRegistry.hpp"

namespace viewer {

/// Registers every game module compiled into this build (chosen with the
/// VIEWER_GAMES CMake option). Implemented by generated code.
void registerBuiltinGames(GameRegistry& registry);

} // namespace viewer
