#include <Alryn/World/VehicleTypes.h>

#include <Alryn/Renderer/MeshPrimitives.h>

namespace alryn {
namespace {

void add_box(MeshData& m, const Vec3& lo, const Vec3& hi, const Vec3& color) {
    const MeshData b = primitives::box(lo, hi, color);
    const u32 base = static_cast<u32>(m.vertices.size());
    m.vertices.insert(m.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (u32 i : b.indices) {
        m.indices.push_back(base + i);
    }
}

const Vec3 kWood{0.45f, 0.32f, 0.18f};
const Vec3 kDark{0.27f, 0.19f, 0.11f};
const Vec3 kMetal{0.30f, 0.30f, 0.33f};
const Vec3 kFrame{0.13f, 0.13f, 0.15f};
const Vec3 kGlow{1.0f, 0.85f, 0.5f};

// A lamp post + housing at a local position (the client adds the glow + light at lamp()).
void add_lamp(MeshData& m, const Vec3& at) {
    add_box(m, {at.x - 0.04f, 0.5f, at.z - 0.04f}, {at.x + 0.04f, at.y, at.z + 0.04f}, kDark);
    add_box(m, {at.x - 0.1f, at.y - 0.04f, at.z - 0.1f}, {at.x + 0.1f, at.y + 0.04f, at.z + 0.1f},
            kFrame);
    add_box(m, {at.x - 0.07f, at.y - 0.22f, at.z - 0.07f}, {at.x + 0.07f, at.y - 0.04f, at.z + 0.07f},
            kGlow); // glass (a little emissive look via bright colour; real glow is client-side)
}

} // namespace

// ---- Cart: small 2-wheel handcart ---------------------------------------
MeshData CartType::body() const {
    MeshData m;
    add_box(m, {-0.08f, 0.35f, -0.58f}, {0.08f, 0.45f, 0.58f}, kMetal);     // axle
    add_box(m, {-0.9f, 0.45f, -0.55f}, {0.6f, 0.55f, 0.55f}, kWood);        // bed
    add_box(m, {-0.9f, 0.55f, 0.48f}, {0.6f, 0.85f, 0.55f}, kWood);         // right rail
    add_box(m, {-0.9f, 0.55f, -0.55f}, {0.6f, 0.85f, -0.48f}, kWood);       // left rail
    add_box(m, {-0.9f, 0.55f, -0.55f}, {-0.82f, 0.95f, 0.55f}, kWood);      // back board
    add_box(m, {0.6f, 0.38f, -0.06f}, {1.5f, 0.48f, 0.06f}, kDark);         // draw handles
                                                                            // (cargo is physical, loaded at runtime)
    add_box(m, {1.4f, 0.38f, -0.3f}, {1.5f, 0.48f, 0.3f}, kDark);
    add_lamp(m, lamp());
    return m;
}
std::vector<Vec3> CartType::wheels() const {
    return {{-0.1f, 0.4f, 0.58f}, {-0.1f, 0.4f, -0.58f}};
}
std::vector<Seat> CartType::seats() const {
    return {Seat{{-0.85f, 0.62f, 0.25f}}, Seat{{-0.85f, 0.62f, -0.25f}}};
}

// ---- Wagon: 4-wheel open wagon ------------------------------------------
MeshData WagonType::body() const {
    MeshData m;
    for (f32 wx : {-0.8f, 0.8f}) {
        add_box(m, {wx - 0.06f, 0.37f, -0.62f}, {wx + 0.06f, 0.47f, 0.62f}, kMetal); // axles
    }
    add_box(m, {-1.0f, 0.6f, -0.58f}, {1.0f, 0.72f, 0.58f}, kWood);        // bed
    add_box(m, {-1.0f, 0.72f, 0.50f}, {1.0f, 1.05f, 0.58f}, kWood);        // right rail
    add_box(m, {-1.0f, 0.72f, -0.58f}, {1.0f, 1.05f, -0.50f}, kWood);      // left rail
    add_box(m, {0.92f, 0.72f, -0.58f}, {1.0f, 1.05f, 0.58f}, kWood);       // front board
    add_box(m, {-1.0f, 0.72f, -0.58f}, {-0.92f, 1.15f, 0.58f}, kWood);     // back board
    add_box(m, {1.0f, 0.5f, -0.07f}, {1.9f, 0.62f, 0.07f}, kDark);         // tongue
                                                                           // (cargo is physical, loaded at runtime)
    add_box(m, {1.8f, 0.5f, -0.35f}, {1.9f, 0.62f, 0.35f}, kDark);
    add_lamp(m, lamp());
    return m;
}
std::vector<Vec3> WagonType::wheels() const {
    return {{0.8f, 0.42f, 0.62f}, {0.8f, 0.42f, -0.62f}, {-0.8f, 0.42f, 0.62f}, {-0.8f, 0.42f, -0.62f}};
}
std::vector<Seat> WagonType::seats() const {
    return {Seat{{0.1f, 0.85f, 0.72f}}, Seat{{0.1f, 0.85f, -0.72f}}, Seat{{-0.7f, 0.95f, 0.0f}}};
}

// ---- Carriage: large enclosed horse-drawn coach -------------------------
MeshData CarriageType::body() const {
    MeshData m;
    const Vec3 coach{0.36f, 0.17f, 0.14f}; // dark red lacquer
    const Vec3 trim{0.55f, 0.44f, 0.2f};   // gilt trim
    const Vec3 window{0.78f, 0.82f, 0.86f};
    for (f32 wx : {-1.0f, 1.0f}) {
        add_box(m, {wx - 0.07f, 0.42f, -0.75f}, {wx + 0.07f, 0.58f, 0.75f}, kMetal); // axles
    }
    add_box(m, {-1.2f, 0.6f, -0.7f}, {1.2f, 0.8f, 0.7f}, kDark);            // chassis
    add_box(m, {-1.0f, 0.8f, -0.66f}, {0.7f, 1.85f, 0.66f}, coach);        // cabin
    add_box(m, {-1.02f, 1.85f, -0.72f}, {0.74f, 2.0f, 0.72f}, coach * 0.8f); // roof
    add_box(m, {-1.0f, 1.8f, -0.7f}, {0.7f, 1.86f, 0.7f}, trim);           // roof trim
    // Windows on each side + back.
    add_box(m, {-0.7f, 1.15f, 0.66f}, {0.3f, 1.6f, 0.69f}, window);
    add_box(m, {-0.7f, 1.15f, -0.69f}, {0.3f, 1.6f, -0.66f}, window);
    add_box(m, {-1.02f, 1.15f, -0.4f}, {-0.99f, 1.6f, 0.4f}, window);
    // Driver bench up top front + footboard.
    add_box(m, {0.7f, 1.5f, -0.6f}, {1.15f, 1.62f, 0.6f}, kWood);          // footboard
    add_box(m, {0.78f, 1.62f, -0.55f}, {1.05f, 2.05f, 0.55f}, kWood);      // bench seat back
    add_box(m, {0.78f, 1.62f, -0.55f}, {1.12f, 1.75f, 0.55f}, kWood);      // bench cushion
    // Harness shaft forward to the horse + lamps on the front corners.
    add_box(m, {1.2f, 0.7f, -0.18f}, {2.4f, 0.8f, -0.06f}, kDark);
    add_box(m, {1.2f, 0.7f, 0.06f}, {2.4f, 0.8f, 0.18f}, kDark);
    add_lamp(m, Vec3{1.45f, 1.9f, 0.55f});
    add_lamp(m, Vec3{1.45f, 1.9f, -0.55f});
    return m;
}
std::vector<Vec3> CarriageType::wheels() const {
    return {{1.0f, 0.5f, 0.75f}, {1.0f, 0.5f, -0.75f}, {-1.0f, 0.5f, 0.75f}, {-1.0f, 0.5f, -0.75f}};
}
std::vector<Seat> CarriageType::seats() const {
    // Inside the cabin (passengers).
    return {Seat{{-0.4f, 0.9f, 0.35f}}, Seat{{-0.4f, 0.9f, -0.35f}}, Seat{{0.2f, 0.9f, 0.0f}}};
}

// ---- Registry ------------------------------------------------------------
const std::vector<std::unique_ptr<VehicleType>>& vehicle_types() {
    static const std::vector<std::unique_ptr<VehicleType>> types = [] {
        std::vector<std::unique_ptr<VehicleType>> v;
        v.push_back(std::make_unique<CartType>());
        v.push_back(std::make_unique<WagonType>());
        v.push_back(std::make_unique<CarriageType>());
        return v;
    }();
    return types;
}
const VehicleType& vehicle_type(u8 i) {
    const auto& v = vehicle_types();
    return *v[i % v.size()];
}

// ---- Horse ---------------------------------------------------------------
MeshData build_horse_body() {
    MeshData m;
    const Vec3 hide{0.34f, 0.22f, 0.13f};
    const Vec3 mane{0.16f, 0.11f, 0.07f};
    // Body: a deep chest up front + a rounded haunch behind (two masses, not one flat barrel).
    add_box(m, {-0.2f, 0.95f, -0.27f}, {0.58f, 1.48f, 0.27f}, hide);   // chest/shoulder (deeper)
    add_box(m, {-0.86f, 0.95f, -0.24f}, {-0.16f, 1.42f, 0.24f}, hide); // barrel + haunch
    // Neck: an ARCHED crest stepping up-forward from the shoulder to the head (not a straight box).
    add_box(m, {0.44f, 1.34f, -0.16f}, {0.74f, 1.74f, 0.16f}, hide);        // lower neck (off the chest)
    add_box(m, {0.6f, 1.62f, -0.14f}, {0.86f, 1.96f, 0.14f}, hide);         // upper neck (arching up)
    add_box(m, {0.78f, 1.74f, -0.13f}, {1.08f, 2.02f, 0.13f}, hide);        // head
    add_box(m, {1.04f, 1.78f, -0.11f}, {1.28f, 1.96f, 0.11f}, hide * 0.95f); // muzzle
    add_box(m, {0.84f, 2.0f, -0.11f}, {0.94f, 2.18f, -0.02f}, mane);   // ear L
    add_box(m, {0.84f, 2.0f, 0.02f}, {0.94f, 2.18f, 0.11f}, mane);     // ear R
    add_box(m, {0.92f, 1.92f, -0.04f}, {1.0f, 2.06f, 0.04f}, mane);    // forelock
    // Mane: a crest running down the back of the neck (stepped + fuller).
    add_box(m, {0.58f, 1.66f, -0.05f}, {0.86f, 2.02f, 0.05f}, mane);   // upper mane (along the arch)
    add_box(m, {0.44f, 1.42f, -0.05f}, {0.68f, 1.8f, 0.05f}, mane);    // lower mane
    // Tail: a docked top + a long flowing switch hanging down behind the rump.
    add_box(m, {-0.92f, 1.1f, -0.06f}, {-0.82f, 1.36f, 0.06f}, mane);  // dock (off the rump)
    add_box(m, {-0.98f, 0.5f, -0.07f}, {-0.86f, 1.14f, 0.07f}, mane);  // long switch hanging down
    return m;
}
MeshData build_horse_leg() {
    MeshData m;
    const Vec3 hide{0.30f, 0.19f, 0.11f};
    const Vec3 hoof{0.12f, 0.10f, 0.09f};
    add_box(m, {-0.08f, -0.85f, -0.08f}, {0.08f, 0.0f, 0.08f}, hide); // hangs below the hip pivot
    add_box(m, {-0.09f, -0.95f, -0.09f}, {0.09f, -0.85f, 0.09f}, hoof);
    return m;
}

// A chunky horned ox, faces local +X: a broad low barrel, a shoulder hump, a blocky head with two
// out-swept horns + ears + muzzle, and a tufted tail.
MeshData build_ox_body() {
    MeshData m;
    const Vec3 hide{0.37f, 0.27f, 0.18f};
    const Vec3 dark{0.22f, 0.16f, 0.11f};
    const Vec3 horn{0.84f, 0.8f, 0.68f};
    add_box(m, {-0.62f, 0.85f, -0.34f}, {0.48f, 1.35f, 0.34f}, hide);  // barrel (broad, low)
    add_box(m, {-0.78f, 0.9f, -0.3f}, {-0.6f, 1.3f, 0.3f}, hide);      // rump
    add_box(m, {0.2f, 1.3f, -0.22f}, {0.46f, 1.6f, 0.22f}, hide);      // shoulder hump
    add_box(m, {0.42f, 1.02f, -0.24f}, {0.72f, 1.42f, 0.24f}, hide);   // thick neck
    add_box(m, {0.68f, 0.98f, -0.22f}, {1.02f, 1.4f, 0.22f}, hide);    // head
    add_box(m, {0.98f, 1.0f, -0.17f}, {1.2f, 1.28f, 0.17f}, hide * 0.94f); // muzzle
    add_box(m, {1.18f, 1.02f, -0.1f}, {1.22f, 1.18f, 0.1f}, dark);         // dark nose pad
    add_box(m, {0.5f, 0.8f, -0.13f}, {0.82f, 1.08f, 0.13f}, hide);         // dewlap (hanging throat fold)
    // horns: a tapering arc that sweeps OUT to the side, then curves UP and FORWARD to a point
    for (f32 sz : {-1.0f, 1.0f}) {
        auto seg = [&](f32 cx, f32 cy, f32 cz, f32 h) {
            add_box(m, {cx - h, cy - h, sz * cz - h}, {cx + h, cy + h, sz * cz + h}, horn);
        };
        seg(0.80f, 1.44f, 0.22f, 0.075f); // root on the head crown
        seg(0.83f, 1.50f, 0.33f, 0.064f); // sweeping outward
        seg(0.86f, 1.59f, 0.41f, 0.055f); // out + rising
        seg(0.90f, 1.69f, 0.44f, 0.046f); // curving up
        seg(0.96f, 1.78f, 0.42f, 0.034f); // forward to the tip
        add_box(m, {0.64f, 1.30f, sz * 0.27f - 0.05f}, {0.76f, 1.42f, sz * 0.27f + 0.05f}, dark); // ear
    }
    add_box(m, {-0.82f, 0.85f, -0.04f}, {-0.72f, 1.32f, 0.04f}, dark); // tail
    add_box(m, {-0.84f, 0.68f, -0.06f}, {-0.7f, 0.86f, 0.06f}, dark);  // tail tuft
    return m;
}
MeshData build_ox_leg() {
    MeshData m;
    const Vec3 hide{0.33f, 0.24f, 0.16f};
    const Vec3 hoof{0.12f, 0.1f, 0.09f};
    add_box(m, {-0.11f, -0.85f, -0.11f}, {0.11f, 0.0f, 0.11f}, hide); // sturdy leg
    add_box(m, {-0.12f, -0.95f, -0.12f}, {0.12f, -0.85f, 0.12f}, hoof);
    return m;
}

// A slender tan DEER (a stag) facing local +X: a lithe body with a pale belly, a long neck + small
// head with ears + forked antlers, and a white scut tail. Slim legs are a separate mesh.
MeshData build_deer_body() {
    MeshData m;
    const Vec3 hide{0.56f, 0.39f, 0.23f};
    const Vec3 belly{0.8f, 0.68f, 0.52f};
    const Vec3 dark{0.3f, 0.2f, 0.12f};
    const Vec3 antler{0.72f, 0.62f, 0.46f};
    // Body: a deeper chest/shoulder mass up front (higher withers) and a lower, tapering haunch
    // behind - a sloped topline rather than one flat box, for a lither deer silhouette.
    add_box(m, {0.02f, 0.98f, -0.17f}, {0.4f, 1.36f, 0.17f}, hide);    // chest/shoulder (withers)
    add_box(m, {-0.5f, 0.98f, -0.15f}, {0.06f, 1.28f, 0.15f}, hide);   // barrel + haunch (lower, tapering)
    add_box(m, {-0.46f, 0.92f, -0.13f}, {0.34f, 1.02f, 0.13f}, belly); // pale belly
    // Neck: a forward-leaning wedge stepping up from the shoulder to the head (not a vertical post).
    add_box(m, {0.32f, 1.18f, -0.11f}, {0.5f, 1.46f, 0.11f}, hide); // lower neck (off the chest)
    add_box(m, {0.42f, 1.4f, -0.1f}, {0.6f, 1.62f, 0.1f}, hide);    // upper neck (up-forward)
    add_box(m, {0.52f, 1.56f, -0.09f}, {0.74f, 1.82f, 0.09f}, hide);        // head
    add_box(m, {0.72f, 1.58f, -0.07f}, {0.94f, 1.74f, 0.07f}, hide * 0.95f); // muzzle
    for (f32 sz : {-1.0f, 1.0f}) {
        add_box(m, {0.48f, 1.78f, sz * 0.07f - 0.03f}, {0.56f, 1.96f, sz * 0.07f + 0.03f}, dark); // ear
        // a small forked antler
        add_box(m, {0.54f, 1.8f, sz * 0.06f - 0.03f}, {0.6f, 2.18f, sz * 0.06f + 0.03f}, antler);
        add_box(m, {0.52f, 2.04f, sz * 0.16f - 0.03f}, {0.62f, 2.12f, sz * 0.16f + 0.03f}, antler);
        add_box(m, {0.54f, 1.98f, sz * 0.22f - 0.03f}, {0.6f, 2.14f, sz * 0.22f + 0.03f}, antler);
    }
    add_box(m, {-0.52f, 1.04f, -0.05f}, {-0.46f, 1.26f, 0.05f},
            Vec3{0.93f, 0.9f, 0.85f}); // white scut at the rump (hangs at the rear, not on the spine)
    return m;
}
MeshData build_deer_leg() {
    MeshData m;
    const Vec3 hide{0.5f, 0.35f, 0.21f};
    const Vec3 hoof{0.12f, 0.1f, 0.09f};
    add_box(m, {-0.05f, -0.92f, -0.05f}, {0.05f, 0.0f, 0.05f}, hide); // slim leg
    add_box(m, {-0.06f, -0.98f, -0.06f}, {0.06f, -0.92f, 0.06f}, hoof);
    return m;
}

} // namespace alryn
