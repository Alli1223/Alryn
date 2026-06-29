// ClientApp - event routing, per-frame input packet and cursor->world aim.
// (Split out of the single ClientApp class; see ClientApp.h.)

#include "ClientApp.h"

namespace alryn::game {

void ClientApp::cast_ability(u8 ability) {
    if (ability >= kAbilityCount || ability_cd_[ability] > 0.0f) {
        return;
    }
    pending_ability_ = static_cast<u8>(ability + 1); // the wire carries the ability index + 1
    ability_cd_[ability] = ability_def(role_, ability).cooldown;
    spawn_ability_vfx(role_, ability, local_feet(), face_yaw_, aim_valid_ ? aim_ : local_feet());
    if (role_ == PlayerRole::Knight && (ability == 1 || ability == 5)) {
        bulwark_fx_ = kBulwarkDuration; // Bulwark + Rally both raise the golden dome
    } else if (role_ == PlayerRole::Hunter && ability == 2) {
        dash_fx_ = kDashDuration;
    }
}

int ClientApp::key_to_element(KeyCode k) {
    // Two ways to queue: number keys (slot order Fire/Water/Earth/Nature) or W/A/S/D. W = Earth so
    // the classic "CTRL + W,W,W" raises a rock wall, matching the in-game hint.
    switch (k) {
        case key::Digit1: case key::S: return static_cast<int>(Element::Fire);
        case key::Digit2: case key::A: return static_cast<int>(Element::Water);
        case key::Digit3: case key::W: return static_cast<int>(Element::Earth);
        case key::Digit4: case key::D: return static_cast<int>(Element::Nature);
        default: return -1;
    }
}

void ClientApp::cast_mage_spell(SpellId sp) {
    if (sp == SpellId::None || mage_cd_ > 0.0f) {
        return; // nothing queued, or still cooling down
    }
    pending_spell_ = static_cast<u8>(sp);
    mage_cd_ = spell_cooldown(sp); // mirror the server cooldown for the HUD + to gate spam
    spawn_primary_vfx();           // a cast flourish at the staff
    // Projectile spells (fireball/frost/boulder) are visible as the projectile; give the INSTANT
    // ones (meteor / heal bloom / empower) a burst so the cast reads.
    const Vec3 feet = local_feet();
    if (sp == SpellId::Meteor && aim_valid_) {
        emit_ring(aim_, Vec4{1.0f, 0.4f, 0.1f, 0.95f}, 30, kMeteorRadius * 1.4f, 0.6f, 0.2f);
        emit_burst(aim_ + Vec3{0.0f, 0.3f, 0.0f}, Vec4{1.0f, 0.55f, 0.15f, 1.0f}, 32, 7.0f, 0.6f,
                   0.18f, 1, 2.5f);
    } else if (sp == SpellId::HealBloom) {
        emit_ring(feet, Vec4{0.5f, 1.0f, 0.6f, 0.9f}, 24, kHealBloomRadius * 0.5f, 0.7f, 0.16f);
        for (int i = 0; i < 16; ++i) {
            emit(feet + rand_dir() * frand(0.3f, 1.5f), Vec3{0.0f, frand(1.5f, 3.0f), 0.0f},
                 Vec4{0.5f, 1.0f, 0.6f, 0.9f}, 0.8f, 0.12f, 1, -1.0f);
        }
    } else if (sp == SpellId::Empower) {
        emit_ring(feet, Vec4{1.0f, 0.5f, 0.2f, 0.9f}, 24, kHealBloomRadius * 0.5f, 0.7f, 0.16f);
        emit_burst(feet + Vec3{0.0f, 0.5f, 0.0f}, Vec4{1.0f, 0.6f, 0.25f, 0.9f}, 16, 3.0f, 0.7f,
                   0.13f, 1, 1.5f);
    }
}

u8 ClientApp::resolve_combo() const {
    int f = 0, w = 0, e = 0, n = 0;
    for (u8 i = 0; i < combo_n_; ++i) {
        switch (static_cast<Element>(combo_[i])) {
            case Element::Fire: ++f; break;
            case Element::Water: ++w; break;
            case Element::Earth: ++e; break;
            default: ++n; break;
        }
    }
    return static_cast<u8>(spell_for_combo(f, w, e, n));
}

void ClientApp::equip_ability(u8 ability) {
    // Already on the bar? clicking again unequips it (clears that slot).
    for (int& slot : bar_) {
        if (slot == static_cast<int>(ability)) {
            slot = -1;
            return;
        }
    }
    // Otherwise drop it into the first empty slot, replacing the last slot if the bar is full.
    for (int& slot : bar_) {
        if (slot < 0) {
            slot = static_cast<int>(ability);
            return;
        }
    }
    bar_[kAbilitySlots - 1] = static_cast<int>(ability);
}

void ClientApp::skills_click(const Vec2& p) {
    for (u8 a = 0; a < kAbilityCount; ++a) {
        if (in_rect(p, skill_node_rects_[a])) {
            equip_ability(a);
            return;
        }
    }
}

void ClientApp::apply_debug_flags() {
    // The debug gameplay toggles only have teeth on a listen server we host (we own the sim).
    if (host_local_ && local_server_.running()) {
        local_server_.set_debug_god(debug_god_);
        local_server_.set_debug_no_ambush(debug_no_ambush_);
    }
}

bool ClientApp::debug_click(const Vec2& p) {
    if (in_rect(p, god_btn_)) {
        debug_god_ = !debug_god_;
        apply_debug_flags();
        return true;
    }
    if (in_rect(p, noatk_btn_)) {
        debug_no_ambush_ = !debug_no_ambush_;
        apply_debug_flags();
        return true;
    }
    return false;
}

bool ClientApp::abilitybar_press(const Vec2& p) {
    for (u8 i = 0; i < kAbilitySlots; ++i) {
        if (bar_[i] >= 0 && in_rect(p, ability_slot_rects_[i])) {
            drag_slot_ = static_cast<int>(i); // begin reordering this slot
            return true;
        }
    }
    return false;
}

bool ClientApp::abilitybar_release(const Vec2& p) {
    if (drag_slot_ < 0) {
        return false;
    }
    const int from = drag_slot_;
    drag_slot_ = -1;
    for (u8 i = 0; i < kAbilitySlots; ++i) {
        if (in_rect(p, ability_slot_rects_[i])) {
            std::swap(bar_[from], bar_[static_cast<int>(i)]); // drop onto a slot -> swap/reorder
            break;
        }
    }
    return true; // released off the bar just cancels the drag (consumed either way)
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

    // Full-screen overlays: the world map (M) and the skills tree (K). While either is
    // open, world input is frozen and all other in-game input is swallowed. Opening one
    // closes the other; ESC closes whichever is open.
    {
        bool consumed = false;
        dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
            if (e.key() == key::M) {
                map_open_ = !map_open_;
                skills_open_ = wardrobe_open_ = false;
                if (map_open_) { // open centred on the player at the default zoom
                    const Vec3 f = local_feet();
                    map_center_ = Vec2{f.x, f.z};
                    map_zoom_ = 1.0f;
                    map_dragging_ = false;
                }
                consumed = true;
                return true;
            }
            if (e.key() == key::K) {
                skills_open_ = !skills_open_;
                map_open_ = wardrobe_open_ = false;
                consumed = true;
                return true;
            }
            if (e.key() == key::U) {
                wardrobe_open_ = !wardrobe_open_;
                map_open_ = skills_open_ = false;
                consumed = true;
                return true;
            }
            if (e.key() == key::C) {
                // Debug: cut all of the local player's flowing cloth off (it flutters to the ground).
                const auto vit = visuals_.find(my_id_);
                if (vit != visuals_.end()) {
                    for (ClothInstance& c : vit->second.cloth) {
                        detach_cloth(c, rand_dir() * frand(0.04f, 0.07f) + Vec3{0.0f, 0.05f, 0.0f});
                    }
                }
                consumed = true;
                return true;
            }
            if ((map_open_ || skills_open_ || wardrobe_open_) && e.key() == key::Escape) {
                map_open_ = skills_open_ = wardrobe_open_ = false;
                consumed = true;
                return true;
            }
            return false;
        });
        // Clicks inside an open overlay route to its hit-tester (equip a skill / buy gear).
        if (skills_open_ || wardrobe_open_) {
            dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
                if (e.button() == 0) {
                    if (skills_open_) {
                        skills_click(pointer_pos());
                    } else {
                        wardrobe_click(pointer_pos());
                    }
                }
                return true;
            });
        }
        if (map_open_ || skills_open_ || wardrobe_open_ || consumed) {
            return;
        }
    }

    // In-game input (not paused).
    dispatcher.dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& e) {
        if (e.button() == 0) {
            const Vec2 p = pointer_pos();
            // A press on the action bar begins a click-drag to reorder it (not an attack).
            if (abilitybar_press(p)) {
                return true;
            }
            // A click on the contract panel's ACCEPT / CANCEL buttons takes priority over melee.
            if (in_rect(p, accept_btn_)) {
                selected_wagon_ = panel_wagon_; // accept -> this becomes our vote
                return true;
            }
            if (in_rect(p, cancel_btn_)) {
                selected_wagon_ = 0; // withdraw the offer
                return true;
            }
            // A click on the debug overlay's toggles takes priority over melee.
            if (debug_open_ && debug_click(p)) {
                return true;
            }
            primary_action(); // role-specific attack (shared with the controller's right trigger)
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
        if (e.button() == 0 && drag_slot_ >= 0) {
            abilitybar_release(pointer_pos()); // finish reordering the action bar
            return true;
        }
        if (e.button() == 1) {
            blocking_ = false; // lower the shield / stop channelling
        }
        return false;
    });
    dispatcher.dispatch<KeyPressedEvent>([&](KeyPressedEvent& e) {
        // Debug overlay (F1) + its toggles (F2 godmode, F3 stop wagon ambushes). Also clickable in
        // the panel; the keys are the quick path and work whatever the role.
        if (e.key() == key::F1) {
            debug_open_ = !debug_open_;
            return true;
        }
        if (e.key() == key::F2) {
            debug_god_ = !debug_god_;
            apply_debug_flags();
            return true;
        }
        if (e.key() == key::F3) {
            debug_no_ambush_ = !debug_no_ambush_;
            apply_debug_flags();
            return true;
        }
        // Mage casting: tap a number key (1-4) to instantly cast that element's spell; or hold
        // Ctrl to weave a COMBO (queue several elements, release to cast the advanced spell).
        if (role_ == PlayerRole::Mage) {
            if (key::is_ctrl(e.key())) {
                casting_ = true;
                combo_n_ = 0;
                return true;
            }
            if (casting_ && !e.is_repeat()) {
                const int el = key_to_element(e.key());
                if (el >= 0) {
                    if (combo_n_ < kMaxCombo) {
                        combo_[combo_n_++] = static_cast<u8>(el);
                    }
                    return true; // swallow the element key while building a combo
                }
            }
            // Not weaving a combo: a number key fires that single element's spell right away.
            if (!casting_ && !e.is_repeat() &&
                (e.key() == key::Digit1 || e.key() == key::Digit2 || e.key() == key::Digit3 ||
                 e.key() == key::Digit4)) {
                const int el = e.key() - key::Digit1; // 0 Fire / 1 Water / 2 Earth / 3 Nature
                int f = el == 0, w = el == 1, ea = el == 2, n = el == 3;
                cast_mage_spell(spell_for_combo(f, w, ea, n));
                return true;
            }
        }
        if (e.key() == key::Escape) {
            escape_pressed(); // opens the pause menu
        } else if (e.key() == key::F) {
            pending_fire_ = true; // throw a rock toward the cursor
        } else if (key::is_shift(e.key()) && !e.is_repeat()) {
            pending_dodge_ = true; // dodge roll (a quick burst + brief i-frames)
        } else if (e.key() == key::E) {
            pending_grab_ = true; // hitch / unhitch the nearest wagon (manual haul)
        } else if (e.key() == key::H) {
            vote_mode_ = vote_mode_ == 1 ? 2 : 1; // toggle hire driver / haul manually
        } else if (role_ != PlayerRole::Mage &&
                   (e.key() == key::Digit1 || e.key() == key::Digit2 || e.key() == key::Digit3 ||
                    e.key() == key::Digit4)) {
            cast_bar_slot(static_cast<u8>(e.key() - key::Digit1)); // cast the ability equipped there
        }
        return false;
    });
    dispatcher.dispatch<KeyReleasedEvent>([&](KeyReleasedEvent& e) {
        // Releasing Ctrl casts the queued combo (the Mage's elemental spell).
        if (role_ == PlayerRole::Mage && key::is_ctrl(e.key()) && casting_) {
            casting_ = false;
            cast_mage_spell(static_cast<SpellId>(resolve_combo())); // cast the woven combo (0 = none)
            combo_n_ = 0;
            return true;
        }
        return false;
    });
}

void ClientApp::primary_action() {
    // The left-click / right-trigger primary attack is role-specific: the Knight swings the held
    // sword, the Hunter looses an arrow, the Cleric casts a damage spell (both fire a role projectile
    // the server picks), the Mage throws a basic fireball. Only the Knight melees + plays the swing.
    if (role_ == PlayerRole::Knight) {
        pending_attack_ = true;      // melee swing (carves terrain if nothing to hit)
        pending_local_swing_ = true; // swing the actual held sword on our own model
    } else if (role_ == PlayerRole::Mage) {
        cast_mage_spell(SpellId::Fireball); // basic bolt (1-4 = elements, CTRL = combos)
    } else {
        pending_fire_ = true; // Hunter arrow / Cleric arcane bolt
        spawn_primary_vfx();  // muzzle / cast flourish at the hand
    }
}

// Fold the connected gamepad into the same input path as mouse + keyboard. The left stick is added
// to the move vector in send_input (where the camera-relative basis lives); here we handle the
// buttons, triggers, bumper-zoom and the menu/overlay toggles, plus the device auto-switch that
// decides whether the aim follows the right stick or the mouse cursor.
void ClientApp::apply_gamepad(Timestep dt) {
    Input* in = input();
    if (in == nullptr) {
        return;
    }
    const bool present = in->gamepad_present();
    // Auto-switch the active device: any controller activity selects the pad; any real mouse motion
    // selects KBM. This only chooses the aim source + reticle - both devices always drive movement.
    if (present) {
        bool active = glm::length(in->left_stick()) > 0.0f || glm::length(in->right_stick()) > 0.0f ||
                      in->left_trigger() > 0.2f || in->right_trigger() > 0.2f;
        for (int b = 0; b < 15 && !active; ++b) {
            active = in->pad_down(b);
        }
        if (active) {
            using_gamepad_ = true;
        }
    }
    if (glm::length(in->mouse_delta()) > 0.5f) {
        using_gamepad_ = false;
    }
    if (!present) {
        using_gamepad_ = false;
        return;
    }

    // Analog triggers are axes, not buttons, so we edge-detect them ourselves.
    const bool rt = in->right_trigger() > pad::trigger_threshold;
    const bool lt = in->left_trigger() > pad::trigger_threshold;
    const bool rt_edge = rt && !pad_rt_prev_;
    const bool lt_edge = lt && !pad_lt_prev_;
    pad_rt_prev_ = rt;
    pad_lt_prev_ = lt;
    auto pressed = [&](int b) { return in->pad_pressed(b); };

    // The full-screen overlays (map / skills / wardrobe) aren't focus-navigable lists - the pad just
    // closes them (the same B/Back that opened them).
    if (state_ == AppState::Playing && (map_open_ || skills_open_ || wardrobe_open_)) {
        if (pressed(pad::B) || pressed(pad::Back)) {
            map_open_ = skills_open_ = wardrobe_open_ = false;
        }
        return;
    }
    // The main menu (Menu state) and the in-game pause menu (paused_) are widget trees we navigate by
    // focus: D-pad up/down moves between controls, left/right adjusts a slider/stepper/swatch, A
    // activates, B goes back. The mouse and pad share the menu - mouse motion hides the focus ring.
    if (state_ != AppState::Playing || paused_) {
        if (!using_gamepad_) {
            ui_.clear_focus();
            return;
        }
        bool navigated = false;
        if (pressed(pad::DDown)) {
            ui_.focus_move(+1);
            navigated = true;
        } else if (pressed(pad::DUp)) {
            ui_.focus_move(-1);
            navigated = true;
        }
        if (pressed(pad::DRight)) {
            ui_.focus_nav(+1);
            navigated = true;
        } else if (pressed(pad::DLeft)) {
            ui_.focus_nav(-1);
            navigated = true;
        }
        if (pressed(pad::A)) {
            ui_.focus_activate();
            navigated = true;
        }
        // B = back (resume when paused), but never on the root menu screen (escape_pressed -> close()
        // there would quit the game out from under the player); Start resumes from the pause menu.
        const bool on_root_menu = state_ == AppState::Menu && current_screen_ == Screen::Main;
        if ((pressed(pad::B) && !on_root_menu) || (paused_ && pressed(pad::Start))) {
            escape_pressed();
            navigated = true;
        }
        if (!navigated && !ui_.has_focus()) {
            ui_.focus_move(+1); // entering a menu with the pad: show the ring on the first control
        }
        return;
    }
    // In-game: Back opens the map; Start opens the pause menu.
    if (pressed(pad::Back)) {
        map_open_ = true;
        skills_open_ = wardrobe_open_ = false;
        const Vec3 f = local_feet();
        map_center_ = Vec2{f.x, f.z};
        map_zoom_ = 1.0f;
        map_dragging_ = false;
        return;
    }
    if (pressed(pad::Start)) {
        escape_pressed();
        return;
    }

    // In-game actions (mirrors the mouse/keyboard bindings).
    if (rt_edge) {
        primary_action(); // right trigger = primary attack
    }
    if (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric) {
        blocking_ = lt; // hold left trigger to guard (Knight) / channel a heal (Cleric)
    } else if (lt_edge) {
        pending_add_ = true; // build terrain
    }
    if (pressed(pad::X)) {
        pending_grab_ = true; // hitch / unhitch the nearest wagon, interact
    }
    if (pressed(pad::B)) {
        pending_dodge_ = true; // dodge roll
    }
    if (pressed(pad::Y)) {
        pending_fire_ = true; // throw a rock / loose toward the aim
    }
    // D-pad -> abilities 1-4 (for the Mage, cast that single element's spell, like the number keys).
    const int dpad[4] = {pad::DUp, pad::DRight, pad::DDown, pad::DLeft};
    for (u8 i = 0; i < 4 && i < kAbilitySlots; ++i) {
        if (!pressed(dpad[i])) {
            continue;
        }
        if (role_ == PlayerRole::Mage) {
            const int f = i == 0, w = i == 1, e = i == 2, n = i == 3;
            cast_mage_spell(spell_for_combo(f, w, e, n));
        } else {
            cast_bar_slot(i);
        }
    }
    // Bumpers zoom the camera continuously while held (mirrors the scroll wheel).
    constexpr f32 zoom_rate = 6.0f;
    if (in->pad_down(pad::RB)) { // zoom in (closer)
        cam_distance_ = glm::clamp(cam_distance_ * std::pow(cam::zoom_step, zoom_rate * dt.seconds),
                                   cam::min_distance, cam::max_distance);
    }
    if (in->pad_down(pad::LB)) { // zoom out (farther)
        cam_distance_ = glm::clamp(cam_distance_ * std::pow(cam::zoom_step, -zoom_rate * dt.seconds),
                                   cam::min_distance, cam::max_distance);
    }
}

void ClientApp::send_input() {
    if (!client_.connected()) {
        return;
    }
    if (paused_ || map_open_ || skills_open_) {
        blocking_ = false; // drop the guard if a release got swallowed by a menu/overlay
        drag_slot_ = -1;   // and cancel any in-progress action-bar drag
        casting_ = false;  // and cancel a half-woven Mage spell
        combo_n_ = 0;
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
    // A Mage holding Ctrl stands still to weave a spell (W/A/S/D are queuing elements, not moving).
    if (Input* in = input();
        in != nullptr && !paused_ && !map_open_ && !skills_open_ && !casting_) {
        if (in->key_down(key::W)) { move += cam_fwd; throttle += 1.0f; }
        if (in->key_down(key::S)) { move -= cam_fwd; throttle -= 1.0f; }
        if (in->key_down(key::D)) { move += cam_right; steer += 1.0f; } // rein right
        if (in->key_down(key::A)) { move -= cam_right; steer -= 1.0f; } // rein left
        jump = in->key_down(key::Space);
        // Controller left stick: same camera-relative mapping as WASD, but analog - partial
        // deflection walks slower (the controller clamps the move length to 1.0, so the magnitude
        // carries through as speed). Stick up (-y) is "into the screen" / forward, like W.
        const Vec2 ls = in->left_stick();
        if (ls.x != 0.0f || ls.y != 0.0f) {
            move += cam_fwd * (-ls.y) + cam_right * ls.x;
            throttle += -ls.y; // forward/back for piloting a carriage
            steer += ls.x;     // rein left/right
        }
        jump = jump || in->pad_down(pad::A); // A jumps
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
    packet.dodge = pending_dodge_;
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
    packet.spell = pending_spell_; // Mage combo spell (0 = none)
    // Right-mouse hold: Knight shield guard / Cleric heal channel.
    packet.block = blocking_ && (role_ == PlayerRole::Knight || role_ == PlayerRole::Cleric);
    packet.appearance = appearance_;
    packet.equipment = equip_loadout_; // desired loadout; the server clamps tiers to what's owned
    // A pending shop purchase: keep requesting until the server grants it (owned_tier reaches it).
    if (const net::PlayerState* me = local_player(); me != nullptr && me->owned_tier >= pending_buy_) {
        pending_buy_ = 0;
    }
    packet.buy = pending_buy_;
    // Wagon-rig purchase: keep requesting until the server's rig_level reaches the target.
    if (snapshot_.rig_level >= pending_buy_rig_) {
        pending_buy_rig_ = 0;
    }
    packet.buy_rig = pending_buy_rig_;
    client_.send_input(packet);
    pending_ability_ = 0;
    pending_spell_ = 0;
    pending_add_ = false;
    pending_fire_ = false;
    pending_attack_ = false;
    pending_dodge_ = false;
    pending_build_ = false;
    pending_rally_ = false;
    pending_grab_ = false;
}

void ClientApp::update_aim() {
    aim_valid_ = false;
    if (terrain_ == nullptr || renderer_ == nullptr) {
        return;
    }
    // Controller: there's no cursor, so aim along the right stick relative to the fixed camera (a
    // centred stick aims where you're facing). Project to a point a few metres ahead and drop it
    // onto the ground so digging / throwing / spells land where the stick points.
    if (using_gamepad_) {
        constexpr f32 reach = 6.0f;
        const f32 cam_yaw = radians(iso::yaw_deg);
        const Vec3 cam_fwd{-std::cos(cam_yaw), 0.0f, -std::sin(cam_yaw)};
        const Vec3 cam_right{-cam_fwd.z, 0.0f, cam_fwd.x};
        Vec2 rs{0.0f};
        if (Input* in = input()) {
            rs = in->right_stick();
        }
        const Vec3 dir = (glm::length(rs) > 0.15f)
                             ? glm::normalize(cam_fwd * (-rs.y) + cam_right * rs.x)
                             : Vec3{std::cos(face_yaw_), 0.0f, std::sin(face_yaw_)};
        const Vec3 target = local_feet() + dir * reach;
        if (const auto hit = terrain_->raycast(target + Vec3{0.0f, 30.0f, 0.0f},
                                               Vec3{0.0f, -1.0f, 0.0f}, 60.0f)) {
            aim_ = *hit;
        } else {
            aim_ = target; // no ground under the point - aim at the flat target anyway
        }
        aim_valid_ = true;
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
