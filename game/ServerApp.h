#pragma once

// Dedicated (headless) server: owns the authoritative GameServer and ticks it at
// ~60 Hz. Launched by `alryn_game --server`.

#include <Alryn/Alryn.h>
#include <Alryn/Net/GameServer.h>

#include "GameConfig.h"

#include <chrono>
#include <thread>

namespace alryn::game {

class ServerApp : public Application {
public:
    ServerApp() : Application(make_config()) {}

protected:
    void on_init() override {
        const u32 seed = world_seed();
        if (!server_.start(kPort, seed)) {
            ALRYN_FATAL("Failed to start server on port {}", kPort);
            close();
            return;
        }
        ALRYN_INFO("Dedicated server listening on port {} (world seed {}) - Ctrl+C to stop.", kPort,
                   seed);
    }
    void on_update(Timestep dt) override {
        server_.tick(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    void on_shutdown() override { server_.stop(); }

private:
    static ApplicationConfig make_config() {
        ApplicationConfig config;
        config.name = "Alryn Server";
        config.headless = true;
        config.max_frames = 0;
        return config;
    }
    GameServer server_;
};

} // namespace alryn::game
