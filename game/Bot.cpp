#include "Bot.h"

#include <Alryn/Core/Log.h>
#include <Alryn/Core/Time.h>
#include <Alryn/Net/NetClient.h>

#include "GameConfig.h"

#include <chrono>
#include <cmath>
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
    while (elapsed < seconds) {
        client.poll(2);
        const f32 dt = static_cast<f32>(clock.restart());
        elapsed += dt;
        if (client.connected()) {
            net::PlayerInput packet;
            packet.sequence = ++sequence;
            packet.move = Vec3{std::sin(elapsed * 1.3f) * 0.8f, 0.0f, std::cos(elapsed * 0.9f) * 0.8f};
            packet.yaw = elapsed;
            client.send_input(packet);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    client.disconnect();
}

} // namespace alryn::game
