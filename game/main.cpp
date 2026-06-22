// Entry point for the Alryn sample game. Parses the command line and launches one
// of three modes: the windowed client (default), a dedicated headless server
// (--server), or a headless wandering bot (--bot). The modes themselves live in
// ClientApp / ServerApp / Bot; this file is just the dispatcher.

#include "Bot.h"
#include "ClientApp.h"
#include "ServerApp.h"

#include <cstdlib>
#include <string>

using namespace alryn;       // u64, f32
using namespace alryn::game; // ServerApp, ClientApp, run_bot

int main(int argc, char** argv) {
    std::string mode = "client";
    std::string host = "127.0.0.1";
    bool joining = false;
    u64 frames = 0;
    f32 bot_seconds = 60.0f;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server") {
            mode = "server";
        } else if (arg == "--bot") {
            mode = "bot";
        } else if (arg.rfind("--host=", 0) == 0) {
            host = arg.substr(7);
            joining = true;
        } else {
            frames = std::strtoull(arg.c_str(), nullptr, 10);
            bot_seconds = static_cast<f32>(frames > 0 ? frames : 60);
        }
    }

    if (mode == "server") {
        ServerApp app;
        app.run();
    } else if (mode == "bot") {
        run_bot(host, bot_seconds);
    } else {
        const bool auto_start = joining || frames > 0;
        ClientApp app{host, !joining, frames, auto_start};
        app.run();
    }
    return 0;
}
