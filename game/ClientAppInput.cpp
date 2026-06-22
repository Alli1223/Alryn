// ClientApp - event routing, per-frame input packet and cursor->world aim.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::cast_ability(u8 slot) {
    if (slot >= kAbilitySlots || ability_cd_[slot] > 0.0f) {
        return;
    }
    pending_ability_ = static_cast<u8>(slot + 1);
    ability_cd_[slot] = ability_def(role_, slot).cooldown;
    spawn_ability_vfx(role_, slot, local_feet(), face_yaw_, aim_valid_ ? aim_ : local_feet());
    if (role_ == PlayerRole::Knight && slot == 1) {
        bulwark_fx_ = kBulwarkDuration;
    } else if (role_ == PlayerRole::Hunter && slot == 2) {
        dash_fx_ = kDashDuration;
    }
}

void ClientApp::on_event(Event& event) {
    EventDispatcher dispatcher{event};

    // (Window resizes are handled in on_update once the swapchain has the new
    // size, so the menu relayout reads the correct extent.)

    // Route input to the UI whenever it's showing: the main menu, or the
    // in-game pause menu overlaid on the (still-live) game.
    if (state_ == AppState::Menu || paused_) {
        dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
            return ui_.pointer_down(pointer_pos(), e.button());
        });
        dispatcher.dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
            return ui_.pointer_up(pointer_pos(), e.button());
        });
        dispatcher.dispatch<KeyTypedEvent>([&](KeyTypedEvent& e) {
            return ui_.text(static_cast<char>(e.codepoint()));
        });
        dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
            if (e.key() == key::Escape) {
                escape_pressed();
                return true;
            }
            return ui_.key(e.key());
        });
        return;
    }

    // Full-screen map overlay (M). While it's open, world input is frozen and all
    // other in-game input is swallowed.
    {
        bool consumed = false;
        dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
            if (e.key() == key::M) {
                map_open_ = !map_open_;
                consumed = true;
                return true;
            }
            if (map_open_ && e.key() == key::Escape) {
                map_open_ = false;
                consumed = true;
                return true;
            }
            return false;
        });
        if (map_open_ || consumed) {
            return;
        }
    }

    // In-game input (not paused).
    dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
        if (e.button() == 0) {
            // A click on the contract panel's ACCEPT / CANCEL buttons takes priority over melee.
            const Vec2 p = pointer_pos();
            if (in_rect(p, accept_btn_)) {
                selected_wagon_ = panel_wagon_; // accept -> this becomes our vote
                return true;
            }
            if (in_rect(p, cancel_btn_)) {
                selected_wagon_ = 0; // withdraw the offer
                return true;
            }
            // Primary attack is role-specific: the Knight swings the held sword, the Hunter
            // looses an arrow, the Cleric casts a damage spell (both fire a role projectile
            // the server picks). Only the Knight melees + plays the sword swing.
            if (role_ == PlayerRole::Knight) {
                pending_attack_ = true;      // melee swing (carves terrain if nothing to hit)
                pending_local_swing_ = true; // swing the actual held sword on our own model
            } else {
                pending_fire_ = true;        // Hunter arrow / Cleric arcane bolt
                spawn_primary_vfx();         // muzzle / cast flourish at the hand
            }
        } else if (e.button() == 1) {
            // Right mouse is held: a Knight raises their shield, a Cleric channels a heal
            // aura (charges while held). Everyone else builds terrain.
            if (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric) {
                blocking_ = true;
            } else {
                pending_add_ = true;
            }
        }
        return false;
    });
    dispatcher.dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& e) {
        if (e.button() == 1) {
            blocking_ = false; // lower the shield / stop channelling
        }
        return false;
    });
    dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
        if (e.key() == key::Escape) {
            escape_pressed(); // opens the pause menu
        } else if (e.key() == key::F) {
            pending_fire_ = true; // throw a rock toward the cursor
        } else if (e.key() == key::E) {
            pending_grab_ = true; // hitch / unhitch the nearest wagon (manual haul)
        } else if (e.key() == key::H) {
            vote_mode_ = vote_mode_ == 1 ? 2 : 1; // toggle hire driver / haul manually
        } else if (e.key() == key::Digit1 || e.key() == key::Digit2 ||
                   e.key() == key::Digit3 || e.key() == key::Digit4) {
            cast_ability(static_cast<u8>(e.key() - key::Digit1)); // abilities (wagons start by walking up)
        }
        return false;
    });
}

void ClientApp::send_input() {
    if (!client_.connected()) {
        return;
    }
    if (paused_ || map_open_) {
        blocking_ = false; // drop the guard if a release got swallowed by the menu/map
    }
    // Movement is relative to the fixed camera: W goes "into" the screen.
    const f32 cam_yaw = radians(iso::yaw_deg);
    const Vec3 cam_fwd{-std::cos(cam_yaw), 0.0f, -std::sin(cam_yaw)};
    const Vec3 cam_right{-cam_fwd.z, 0.0f, cam_fwd.x};
    Vec3 move{0.0f};
    bool jump = false;
    f32 throttle = 0.0f; // raw W/S - drives a carriage when piloting (else ignored)
    f32 steer = 0.0f;    // raw A/D - reins the horses (RDR-style) when piloting
    // While the pause menu is up the player holds still (and ignores WASD/SPACE
    // that would otherwise leak through to movement).
    if (Input* in = input(); in != nullptr && !paused_ && !map_open_) {
        if (in->key_down(key::W)) { move += cam_fwd; throttle += 1.0f; }
        if (in->key_down(key::S)) { move -= cam_fwd; throttle -= 1.0f; }
        if (in->key_down(key::D)) { move += cam_right; steer += 1.0f; } // rein right
        if (in->key_down(key::A)) { move -= cam_right; steer -= 1.0f; } // rein left
        jump = in->key_down(key::Space);
    }
    if (glm::length(move) > 0.01f) {
        face_yaw_ = std::atan2(move.z, move.x); // face the way we walk
    }

    net::PlayerInput packet;
    packet.sequence = ++sequence_;
    packet.move = move;
    packet.yaw = face_yaw_;
    packet.jump = jump;
    packet.throttle = throttle;
    packet.steer = steer;
    packet.add = pending_add_;
    packet.fire = pending_fire_;
    packet.attack = pending_attack_;
    packet.build = pending_build_;
    packet.rally = pending_rally_;
    packet.grab = pending_grab_;
    packet.aim = aim_;
    // A haul starts by walking up to a parked wagon (which pops its info panel) and pressing
    // ACCEPT. `near_wagon_` is the offer you're standing by; `selected_wagon_` is the one you
    // accepted (your vote). Drop a stale vote if its offer is gone (town changed).
    const bool offering = snapshot_.contract_phase == static_cast<u8>(ContractPhase::Offer);
    near_wagon_ = offering ? nearest_offer_in_range() : 0;
    if (selected_wagon_ != 0 && (!offering || !wagon_offered(selected_wagon_))) {
        selected_wagon_ = 0;
    }
    packet.vote_wagon = offering ? selected_wagon_ : 0;
    packet.vote_mode = vote_mode_;
    packet.role = static_cast<u8>(role_);
    packet.ability = pending_ability_;
    // Right-mouse hold: Knight shield guard / Cleric heal channel.
    packet.block = blocking_ && (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric);
    packet.appearance = appearance_;
    client_.send_input(packet);
    pending_ability_ = 0;
    pending_add_ = false;
    pending_fire_ = false;
    pending_attack_ = false;
    pending_build_ = false;
    pending_rally_ = false;
    pending_grab_ = false;
}

void ClientApp::update_aim() {
    aim_valid_ = false;
    if (terrain_ == nullptr || renderer_ == nullptr) {
        return;
    }
    const VkExtent2D extent = renderer_->extent();
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    Vec2 cursor{static_cast<f32>(extent.width) * 0.5f, static_cast<f32>(extent.height) * 0.5f};
    if (Input* in = input()) {
        cursor = in->mouse_position();
    }
    const Vec2 ndc{2.0f * cursor.x / static_cast<f32>(extent.width) - 1.0f,
                   2.0f * cursor.y / static_cast<f32>(extent.height) - 1.0f};
    const Mat4 inv = glm::inverse(camera_.view_projection());
    const Vec4 near_h = inv * Vec4{ndc.x, ndc.y, 0.0f, 1.0f};
    const Vec4 far_h = inv * Vec4{ndc.x, ndc.y, 1.0f, 1.0f};
    const Vec3 origin = Vec3{near_h} / near_h.w;
    const Vec3 dir = glm::normalize(Vec3{far_h} / far_h.w - origin);
    if (const auto hit = terrain_->raycast(origin, dir, 300.0f)) {
        aim_ = *hit;
        aim_valid_ = true;
    }
}

} // namespace alryn::game
