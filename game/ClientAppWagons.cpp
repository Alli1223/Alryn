// ClientApp - wagon / goods / horse / harness-rope rendering and helpers.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::wagon_orient(const net::WagonState& wg, f32 moved, f32& pitch, f32& roll, f32& bob) const {
    const f32 cy = std::cos(wg.yaw);
    const f32 sy = std::sin(wg.yaw);
    const Vec2 fwd{cy, sy};  // world xz the cart faces (local +X under rotate(-yaw))
    const Vec2 lat{-sy, cy}; // world xz across the cart (local +Z)
    constexpr f32 d = 0.75f;
    auto height = [&](const Vec2& o) {
        return worldgen::height(wg.position.x + o.x, wg.position.z + o.y, world_seed_);
    };
    const f32 slope_f = (height(fwd * d) - height(fwd * -d)) / (2.0f * d);
    const f32 slope_l = (height(lat * d) - height(lat * -d)) / (2.0f * d);
    pitch = glm::clamp(std::atan(slope_f), -0.6f, 0.6f); // nose up going uphill
    roll = glm::clamp(-std::atan(slope_l), -0.6f, 0.6f); // lean across the slope
    bob = std::sin(elapsed_ * 11.0f + wg.position.x * 0.7f) * 0.04f *
          glm::clamp(moved * 12.0f, 0.0f, 1.0f);
}

Mat4 ClientApp::wagon_model(const net::WagonState& wg, f32 moved) const {
    // The wagon mesh faces local +X. The server yaw has forward = (cos, sin) in world
    // xz, and glm::rotate(-yaw) maps local +X exactly there - so the tongue points the
    // way the cart travels (toward its puller).
    f32 pitch, roll, bob;
    wagon_orient(wg, moved, pitch, roll, bob);
    return glm::translate(Mat4{1.0f}, Vec3{wg.position.x, wg.position.y + bob, wg.position.z}) *
           glm::rotate(Mat4{1.0f}, -wg.yaw, Vec3{0.0f, 1.0f, 0.0f}) *
           glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
           glm::rotate(Mat4{1.0f}, roll, Vec3{1.0f, 0.0f, 0.0f});
}

Vec3 ClientApp::attach_to_wagon(const net::WagonState& wg, const Vec3& flat_world) const {
    f32 pitch, roll, bob;
    wagon_orient(wg, wagon_frame_move(wg), pitch, roll, bob);
    const Mat4 tilt = glm::rotate(Mat4{1.0f}, pitch, Vec3{0.0f, 0.0f, 1.0f}) *
                      glm::rotate(Mat4{1.0f}, roll, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 off = Vec3{tilt * Vec4{flat_world - wg.position, 0.0f}};
    return wg.position + off + Vec3{0.0f, bob, 0.0f};
}

void ClientApp::draw_wagons() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    for (const net::WagonState& wg : snapshot_.wagons) {
        const f32 hp = static_cast<f32>(wg.health) / 255.0f;
        Vec3 tint{1.0f};
        if (wg.mode == static_cast<u8>(WagonMode::Parked) && wg.id == selected_wagon_) {
            tint = Vec3{1.4f, 1.25f, 0.7f}; // your pick glows
        }
        tint = glm::mix(Vec3{0.35f, 0.28f, 0.22f}, tint, glm::clamp(hp, 0.25f, 1.0f));

        const VehicleType& vt = vehicle_type(wg.type);

        // Spin the wheels at their true rolling speed: with no slip the wheel turns
        // (ground distance / wheel radius) radians. Sign it by the travel direction so a
        // reversing carriage rolls its wheels backward.
        f32& roll = wagon_roll_[wg.id];
        f32 moved = 0.0f;
        const auto prev = wagon_prev_.find(wg.id);
        if (prev != wagon_prev_.end()) {
            const Vec2 delta{wg.position.x - prev->second.x, wg.position.z - prev->second.z};
            moved = glm::length(delta);
            const Vec2 fwd{std::cos(wg.yaw), std::sin(wg.yaw)};
            roll -= glm::dot(delta, fwd) / vt.wheel_radius(); // signed distance / radius
        }
        wagon_prev_[wg.id] = wg.position;

        Mat4 m = wagon_model(wg, moved);
        // A wheel has come off: the cart lists onto its missing corner (front-left, index 0).
        if (wg.wheel_off) {
            m = m * glm::rotate(Mat4{1.0f}, 0.17f, Vec3{1.0f, 0.0f, 0.0f}) *
                glm::rotate(Mat4{1.0f}, -0.05f, Vec3{0.0f, 0.0f, 1.0f});
        }
        renderer_->draw(vehicle_meshes_[wg.type % vehicle_meshes_.size()], m, Vec4{tint, 1.0f});

        // Wheels (scaled to the type's radius), spun by the accumulated roll. When a wheel is off,
        // its mount (index 0) is left bare.
        const f32 wscale = vt.wheel_radius() / kWagonWheelRadius;
        const std::vector<Vec3> wheels = vt.wheels();
        for (usize wi = 0; wi < wheels.size(); ++wi) {
            if (wg.wheel_off && wi == 0) {
                continue; // this wheel is the one that fell off
            }
            const Mat4 wm = m * glm::translate(Mat4{1.0f}, wheels[wi]) *
                            glm::rotate(Mat4{1.0f}, roll, Vec3{0.0f, 0.0f, 1.0f}) *
                            glm::scale(Mat4{1.0f}, Vec3{wscale});
            renderer_->draw(wagon_wheel_mesh_, wm, Vec4{tint, 1.0f});
        }
        // The fallen / carried wheel, lying tilted where it dropped (or held at a player's waist).
        if (wg.wheel_off) {
            const Mat4 wm = glm::translate(Mat4{1.0f}, wg.wheel_pos) *
                            glm::rotate(Mat4{1.0f}, elapsed_ * 0.4f, Vec3{0.0f, 1.0f, 0.0f}) *
                            glm::rotate(Mat4{1.0f}, 0.55f, Vec3{1.0f, 0.0f, 0.25f}) *
                            glm::scale(Mat4{1.0f}, Vec3{wscale});
            renderer_->draw(wagon_wheel_mesh_, wm, Vec4{0.46f, 0.34f, 0.22f, 1.0f});
        }

        // The lamp: an emissive glow always, plus a warm light at night when near.
        const Vec3 lampw = Vec3{m * Vec4{vt.lamp(), 1.0f}};
        const f32 night = 1.0f - sun_intensity_;
        const f32 glow = glm::mix(1.0f, 0.5f, sun_intensity_);
        renderer_->draw_emissive(shape_sphere_,
                                 glm::translate(Mat4{1.0f}, lampw) *
                                     glm::scale(Mat4{1.0f}, Vec3{0.16f}),
                                 Vec4{glow, glow * 0.82f, glow * 0.5f, 1.0f});
        const bool is_active_cargo =
            snapshot_.contract_phase == static_cast<u8>(ContractPhase::Active);
        if (night > 0.1f && glm::length(lampw - local_feet()) < light::wagon_lamp_cull_dist) {
            Renderer::SpotLight sl;
            sl.position = lampw + Vec3{0.0f, 0.6f, 0.0f}; // lift it so shadows rake outward
            sl.direction = Vec3{0.0f, -1.0f, 0.0f};
            sl.color = Vec3{1.0f, 0.82f, 0.5f} * (1.7f * night);
            sl.range = 18.0f;
            sl.cone_outer_cos = std::cos(glm::radians(74.0f));
            sl.cone_inner_cos = std::cos(glm::radians(40.0f));
            // The ACTIVE escorted cargo is the scene's KEY light: a top-priority real
            // shadow-caster (the most expensive light), throwing crisp shadows of the player
            // + nearby props/enemies as it rolls. Parked offers are cheap unshadowed lamps.
            sl.cast_shadow = is_active_cargo;
            sl.priority = is_active_cargo;
            renderer_->add_light(sl);
        }

        // The draft oxen (a yoked pair) pulling the wagon, with a walking gait + harness ropes.
        if (wg.has_horse) {
            draw_oxen(wg.horse_pos, wg.horse_yaw);
            draw_ropes(wg.id);
        }
    }
}

void ClientApp::draw_goods() {
    if (renderer_ == nullptr || !have_snapshot_) {
        return;
    }
    const net::WagonState* aw = active_wagon();
    const Mat4 cart = aw != nullptr ? wagon_model(*aw, wagon_frame_move(*aw)) : Mat4{1.0f};
    for (const net::GoodState& g : snapshot_.goods) {
        Mat4 m;
        if (g.loose == 0 && aw != nullptr) {
            // Bed-local position -> world via the cart transform (centred crate on the floor).
            m = cart * glm::translate(Mat4{1.0f}, g.position + Vec3{0.0f, kCargoHalf, 0.0f});
        } else {
            // A slight per-crate lean so fallen goods look tumbled rather than neatly placed.
            m = glm::translate(Mat4{1.0f}, g.position) *
                glm::rotate(Mat4{1.0f}, static_cast<f32>(g.id) * 1.3f, Vec3{0.0f, 1.0f, 0.0f}) *
                glm::rotate(Mat4{1.0f}, 0.18f, Vec3{1.0f, 0.0f, 0.0f});
        }
        renderer_->draw(goods_mesh_, m);
    }
}

void ClientApp::draw_carried_good(const Vec3& feet, f32 yaw) {
    const Vec3 fwd{std::cos(yaw), 0.0f, std::sin(yaw)};
    const Vec3 pos = feet + Vec3{0.0f, 0.85f, 0.0f} + fwd * 0.35f;
    const Mat4 m = glm::translate(Mat4{1.0f}, pos) *
                   glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f});
    renderer_->draw(goods_mesh_, m);
}

void ClientApp::draw_ropes(u32 id) {
    const auto it = wagon_ropes_.find(id);
    if (it == wagon_ropes_.end()) {
        return;
    }
    constexpr f32 thick = 0.04f;
    const Vec4 col{0.16f, 0.11f, 0.06f, 1.0f};
    for (const RopeTrace& r : it->second) {
        if (!r.init) {
            continue;
        }
        for (int i = 0; i < kRopeNodes - 1; ++i) {
            const Vec3 a = r.pos[i];
            const Vec3 d = r.pos[i + 1] - a;
            const f32 L = glm::length(d);
            if (L < 1e-4f) {
                continue;
            }
            // Build a basis whose +Y maps along the link (the rope mesh runs y = 0..1).
            const Vec3 up = d / L;
            const Vec3 ref = std::abs(up.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 bx = glm::normalize(glm::cross(ref, up));
            const Vec3 bz = glm::cross(up, bx);
            Mat4 m{1.0f};
            m[0] = Vec4{bx * thick, 0.0f};
            m[1] = Vec4{up * L, 0.0f};
            m[2] = Vec4{bz * thick, 0.0f};
            m[3] = Vec4{a, 1.0f};
            renderer_->draw(rope_mesh_, m, col);
        }
    }
}

void ClientApp::update_ropes(Timestep dt) {
    if (!have_snapshot_) {
        wagon_ropes_.clear();
        return;
    }
    const f32 h = std::min(dt.seconds, 1.0f / 30.0f); // clamp the step for stability
    auto to_world = [](const Vec3& center, f32 yaw, const Vec3& local) {
        return center + Vec3{glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                             Vec4{local, 0.0f}};
    };
    for (const net::WagonState& wg : snapshot_.wagons) {
        if (!wg.has_horse) {
            continue;
        }
        std::array<RopeTrace, 2>& ropes = wagon_ropes_[wg.id];
        for (int side = 0; side < 2; ++side) {
            const f32 s = side == 0 ? 1.0f : -1.0f;
            const Vec3 A = to_world(wg.position, wg.yaw, Vec3{2.4f, 0.75f, 0.12f * s}); // shaft tip
            const Vec3 B = to_world(wg.horse_pos, wg.horse_yaw, Vec3{0.45f, 1.05f, 0.18f * s}); // collar
            RopeTrace& r = ropes[side];
            if (!r.init) {
                for (int i = 0; i < kRopeNodes; ++i) {
                    const f32 t = static_cast<f32>(i) / static_cast<f32>(kRopeNodes - 1);
                    r.pos[i] = glm::mix(A, B, t);
                    r.prev[i] = r.pos[i];
                }
                r.init = true;
            }
            r.pos[0] = A; // pin the ends to the live attachment points
            r.prev[0] = A;
            r.pos[kRopeNodes - 1] = B;
            r.prev[kRopeNodes - 1] = B;
            for (int i = 1; i < kRopeNodes - 1; ++i) { // verlet: gravity + damping
                const Vec3 cur = r.pos[i];
                const Vec3 vel = (r.pos[i] - r.prev[i]) * 0.94f;
                r.pos[i] = cur + vel + Vec3{0.0f, -12.0f, 0.0f} * (h * h);
                r.prev[i] = cur;
            }
            const f32 seg = glm::length(B - A) * 1.08f / static_cast<f32>(kRopeNodes - 1);
            for (int it = 0; it < 10; ++it) { // satisfy link lengths (a touch slack = sag)
                for (int i = 0; i < kRopeNodes - 1; ++i) {
                    const Vec3 d = r.pos[i + 1] - r.pos[i];
                    const f32 l = glm::length(d);
                    if (l < 1e-5f) {
                        continue;
                    }
                    const Vec3 corr = d * ((l - seg) / l);
                    const bool af = i != 0;
                    const bool bf = i + 1 != kRopeNodes - 1;
                    if (af && bf) {
                        r.pos[i] += corr * 0.5f;
                        r.pos[i + 1] -= corr * 0.5f;
                    } else if (af) {
                        r.pos[i] += corr;
                    } else if (bf) {
                        r.pos[i + 1] -= corr;
                    }
                }
            }
        }
    }
    // Cull ropes whose wagon is gone / no longer horse-drawn.
    for (auto it = wagon_ropes_.begin(); it != wagon_ropes_.end();) {
        const bool live = std::any_of(snapshot_.wagons.begin(), snapshot_.wagons.end(),
                                      [&](const net::WagonState& w) {
                                          return w.id == it->first && w.has_horse != 0;
                                      });
        it = live ? std::next(it) : wagon_ropes_.erase(it);
    }
}

void ClientApp::draw_horse(const Vec3& pos, f32 yaw) {
    const f32 moved = glm::length(Vec2{pos.x - horse_prev_.x, pos.z - horse_prev_.z});
    horse_gait_ += moved * 2.2f;
    horse_prev_ = pos;
    const Mat4 base = glm::translate(Mat4{1.0f}, pos) *
                      glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f});
    renderer_->draw(horse_body_mesh_, base);
    for (int k = 0; k < 4; ++k) {
        const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f; // diagonal pairs
        const f32 swing = std::sin(horse_gait_) * 0.5f * sign;
        const Mat4 lm = base * glm::translate(Mat4{1.0f}, kHorseLegs[k]) *
                        glm::rotate(Mat4{1.0f}, swing, Vec3{0.0f, 0.0f, 1.0f});
        renderer_->draw(horse_leg_mesh_, lm);
    }
}

// A yoked PAIR of oxen pulling the wagon: two beasts side by side at the puller point, each with a
// walking gait, joined by a wooden yoke beam across their shoulders.
void ClientApp::draw_oxen(const Vec3& pos, f32 yaw) {
    const f32 moved = glm::length(Vec2{pos.x - horse_prev_.x, pos.z - horse_prev_.z});
    horse_gait_ += moved * 2.0f;
    horse_prev_ = pos;
    const Vec3 fwd{std::cos(yaw), 0.0f, std::sin(yaw)};
    const Vec3 right = glm::normalize(glm::cross(Vec3{0.0f, 1.0f, 0.0f}, fwd));
    for (f32 side : {-1.0f, 1.0f}) {
        const Vec3 op = pos + right * (side * 0.52f);
        const Mat4 base = glm::translate(Mat4{1.0f}, op) * glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f});
        renderer_->draw(ox_body_mesh_, base);
        for (int k = 0; k < 4; ++k) {
            const f32 sign = (k == 0 || k == 3) ? 1.0f : -1.0f;
            const f32 swing = std::sin(horse_gait_ + side) * 0.42f * sign;
            const Mat4 lm = base * glm::translate(Mat4{1.0f}, kOxLegs[k]) *
                            glm::rotate(Mat4{1.0f}, swing, Vec3{0.0f, 0.0f, 1.0f});
            renderer_->draw(ox_leg_mesh_, lm);
        }
    }
    // a wooden yoke beam across both oxen's shoulders
    const Mat4 ym = glm::translate(Mat4{1.0f}, pos + Vec3{0.0f, 1.5f, 0.0f}) *
                    glm::rotate(Mat4{1.0f}, -yaw, Vec3{0.0f, 1.0f, 0.0f}) *
                    glm::scale(Mat4{1.0f}, Vec3{0.12f, 0.1f, 1.5f});
    renderer_->draw(shape_box_, ym, Vec4{0.4f, 0.28f, 0.16f, 1.0f});
}

u32 ClientApp::nearest_offer_in_range() const {
    if (!have_snapshot_ || snapshot_.contract_phase != static_cast<u8>(ContractPhase::Offer)) {
        return 0;
    }
    const Vec3 feet = local_feet();
    u32 best_id = 0;
    f32 best = kWagonStartRange;
    for (const net::WagonState& wg : snapshot_.wagons) {
        const f32 d = glm::length(Vec2{wg.position.x - feet.x, wg.position.z - feet.z});
        if (d < best) {
            best = d;
            best_id = wg.id;
        }
    }
    return best_id;
}

} // namespace alryn::game
