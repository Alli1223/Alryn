#pragma once

// A headless "bot" client: connects and walks a looping path so we can demo other
// players moving (and exercise the server) without a window. Launched by
// `alryn_game --bot`.

#include <Alryn/Core/Types.h>

#include <string>

namespace alryn::game {

// Connects to `host` and sends wandering input for `seconds`, then disconnects.
void run_bot(const std::string& host, f32 seconds);

} // namespace alryn::game
