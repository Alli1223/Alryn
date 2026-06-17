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
    add_box(m, {-0.7f, 0.95f, -0.26f}, {0.55f, 1.45f, 0.26f}, hide);        // barrel
    add_box(m, {-0.85f, 1.0f, -0.2f}, {-0.7f, 1.35f, 0.2f}, hide);         // rump
    // Neck (angled up-forward) + head.
    add_box(m, {0.45f, 1.3f, -0.16f}, {0.8f, 1.85f, 0.16f}, hide);         // neck
    add_box(m, {0.7f, 1.7f, -0.14f}, {1.05f, 2.0f, 0.14f}, hide);          // head
    add_box(m, {1.0f, 1.78f, -0.12f}, {1.25f, 1.95f, 0.12f}, hide * 0.95f); // muzzle
    add_box(m, {0.62f, 1.85f, -0.12f}, {0.72f, 2.05f, -0.02f}, mane);      // ears
    add_box(m, {0.62f, 1.85f, 0.02f}, {0.72f, 2.05f, 0.12f}, mane);
    add_box(m, {0.45f, 1.45f, -0.05f}, {0.78f, 1.95f, 0.05f}, mane);       // mane
    add_box(m, {-0.9f, 1.0f, -0.04f}, {-0.78f, 1.4f, 0.04f}, mane);        // tail
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

} // namespace alryn
