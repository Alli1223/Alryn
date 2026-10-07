#include "Bot.h"

#include <Alryn/Core/Log.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Game/Roles.h>
#include <Alryn/Net/NetClient.h>

#include "GameConfig.h"

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace alryn::game {

void run_bot(const std::string& host, f32 seconds) {
    Log::init(LogLevel::Info);
    net::NetClient client;
    if (!client.connect(host, kPort)) {
        ALRYN_ERROR("Bot could not connect to {}:{}", host, kPort);
        return;
    }
    Clock clock;
    f32 elapsed = 0.0f;
    u32 sequence = 0;
    // Each bot rides as a random class under a name, so a load test reads like a real party
    // (name plates, party frames, identity colours).
    static const char* names[] = {"BRAM", "ODA", "TOMAS", "IVY", "GRIM", "LISBET", "HEW", "NELL"};
    const u32 pick = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count() / 1000);
    const u8 role = static_cast<u8>(pick % kRoleCount);
    const std::string name = std::string{"BOT "} + names[(pick / 7u) % 8u];
    while (elapsed < seconds) {
        client.poll(2);
        const f32 dt = static_cast<f32>(clock.restart());
        elapsed += dt;
        if (client.connected()) {
            net::PlayerInput packet;
            packet.sequence = ++sequence;
            packet.move = Vec3{std::sin(elapsed * 1.3f) * 0.8f, 0.0f, std::cos(elapsed * 0.9f) * 0.8f};
            packet.yaw = elapsed;
            packet.role = role;
            packet.name = name;
            client.send_input(packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    client.disconnect();
}

} // namespace alryn::game
