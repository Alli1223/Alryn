#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <memory>
#include <vector>

// The transport vehicles, as an OOP type hierarchy (the Type-Object pattern): one shared
// VehicleType object per kind (cart / wagon / carriage), and each Wagon instance carries a
// `type` index into the registry. The base declares the shape + layout an instance needs;
// concrete subclasses fill it in. The server uses the layout accessors (seats, reach,
// horse, driver seat, capacity); the client also builds the meshes once per type.
namespace alryn {

// One mounting point on a vehicle, in the vehicle's local frame (local +X = forward).
struct Seat {
    Vec3 pos{0.0f};
};

// The cargo bed, in the vehicle's local frame: a box whose floor is lo.y and whose xz extent
// is the area crates slide around in. `wall` is how high the side rails stand above the floor:
// a crate can only escape by being launched UP over a rail (a bump tossing it above `wall`),
// never by sliding through one. An enclosed body (the carriage) uses a very tall `wall`, so its
// cargo can never get out - the benefit of a closed cabin.
struct CargoBed {
    Vec3 lo{0.0f};
    Vec3 hi{0.0f};
    f32 wall = 0.35f;
};

class VehicleType {
public:
    virtual ~VehicleType() = default;

    virtual const char* name() const = 0;
    virtual MeshData body() const = 0;            // body mesh (no wheels); includes an emissive lamp
    virtual std::vector<Vec3> wheels() const = 0; // local axle centres (2 = cart, 4 = wagon/carriage)
    virtual f32 wheel_radius() const { return 0.42f; }
    virtual std::vector<Seat> seats() const = 0;  // passenger seats (edges / inside)
    virtual bool horse_drawn() const { return false; }
    // Local seat for the driver up top (carriages). {} (default) means the puller walks in
    // front instead of riding.
    virtual Vec3 driver_seat() const { return Vec3{0.0f}; }
    virtual bool has_driver_seat() const { return false; }
    virtual Vec3 lamp() const = 0;            // local lamp position (warm light + emissive glow)
    virtual f32 reach() const { return 1.1f; } // clearance half-size for the cart's own routing
    virtual u32 capacity() const { return 1; } // cargo units -> reward multiplier
    virtual CargoBed bed() const = 0;          // where the physical cargo crates ride + slide
    virtual Vec2 footprint() const { return Vec2{1.0f, 0.6f}; } // body half-extents (x,z) for
                                                                // blocking players from walking through
    virtual f32 deck_height() const { return 1.1f; } // local top-surface y a player stands on (ride-along)
};

// --- Concrete vehicles ----------------------------------------------------
class CartType : public VehicleType {
public:
    const char* name() const override { return "cart"; }
    MeshData body() const override;
    std::vector<Vec3> wheels() const override;
    f32 wheel_radius() const override { return 0.4f; }
    std::vector<Seat> seats() const override;
    Vec3 lamp() const override { return Vec3{0.8f, 1.15f, -0.5f}; }
    f32 reach() const override { return 0.9f; }
    u32 capacity() const override { return 1; }
    CargoBed bed() const override { return {{-0.85f, 0.55f, -0.5f}, {0.55f, 0.9f, 0.5f}, 0.3f}; }
    Vec2 footprint() const override { return Vec2{1.0f, 0.55f}; }
};

class WagonType : public VehicleType {
public:
    const char* name() const override { return "wagon"; }
    MeshData body() const override;
    std::vector<Vec3> wheels() const override;
    std::vector<Seat> seats() const override;
    Vec3 lamp() const override { return Vec3{1.0f, 1.3f, 0.0f}; }
    f32 reach() const override { return 1.1f; }
    u32 capacity() const override { return 2; }
    CargoBed bed() const override { return {{-0.95f, 0.72f, -0.52f}, {0.95f, 1.05f, 0.52f}, 0.33f}; }
    Vec2 footprint() const override { return Vec2{1.15f, 0.62f}; }
};

class CarriageType : public VehicleType {
public:
    const char* name() const override { return "carriage"; }
    MeshData body() const override;
    std::vector<Vec3> wheels() const override;
    f32 wheel_radius() const override { return 0.5f; }
    std::vector<Seat> seats() const override;
    bool horse_drawn() const override { return true; }
    Vec3 driver_seat() const override { return Vec3{1.0f, 1.75f, 0.0f}; }
    bool has_driver_seat() const override { return true; }
    Vec3 lamp() const override { return Vec3{1.45f, 1.9f, 0.0f}; }
    f32 reach() const override { return 1.5f; }
    u32 capacity() const override { return 3; }
    // Enclosed cabin: a very tall "wall" means crates can never be launched out (no spills).
    CargoBed bed() const override { return {{-0.95f, 0.8f, -0.6f}, {0.65f, 1.8f, 0.6f}, 5.0f}; }
    Vec2 footprint() const override { return Vec2{1.35f, 0.8f}; }
};

// The registry of one instance per type, indexed by Wagon::type.
const std::vector<std::unique_ptr<VehicleType>>& vehicle_types();
const VehicleType& vehicle_type(u8 i);
inline u8 vehicle_type_count() { return static_cast<u8>(vehicle_types().size()); }

// Horse meshes (the puller for carriages): a body (no legs) + a single leg drawn x4 and
// swung by the client for a walk gait. Faces local +X.
MeshData build_horse_body();
MeshData build_horse_leg();
inline constexpr f32 kHorseLegHipY = 0.95f; // hip height the legs pivot from
// Local leg mount offsets (fore/aft x, lateral z); legs hang from y = kHorseLegHipY.
inline const Vec3 kHorseLegs[4] = {
    {0.55f, kHorseLegHipY, 0.22f},
    {0.55f, kHorseLegHipY, -0.22f},
    {-0.55f, kHorseLegHipY, 0.22f},
    {-0.55f, kHorseLegHipY, -0.22f},
};

// Ox meshes (the draft animals that pull the cargo wagon - drawn as a yoked PAIR): a chunky horned
// body + a single sturdy leg drawn x4 with a walk gait. Faces local +X, like the horse.
MeshData build_ox_body();
MeshData build_ox_leg();
inline constexpr f32 kOxLegHipY = 0.9f;
inline const Vec3 kOxLegs[4] = {
    {0.38f, kOxLegHipY, 0.24f},
    {0.38f, kOxLegHipY, -0.24f},
    {-0.5f, kOxLegHipY, 0.24f},
    {-0.5f, kOxLegHipY, -0.24f},
};

// Deer meshes (ambient wildlife): a slender stag body + a slim leg drawn x4 with a walk gait.
MeshData build_deer_body();
MeshData build_deer_leg();
inline constexpr f32 kDeerLegHipY = 0.98f;
inline const Vec3 kDeerLegs[4] = {
    {0.3f, kDeerLegHipY, 0.13f},
    {0.3f, kDeerLegHipY, -0.13f},
    {-0.4f, kDeerLegHipY, 0.13f},
    {-0.4f, kDeerLegHipY, -0.13f},
};

// A small low-poly fish (ambient wildlife in the water): a laterally-flattened spindle body with
// a forked tail + dorsal/pectoral fins, facing local +X. Built in a light base colour so the
// client can tint it per-fish by biome (bright tropical vs silver/dark freshwater).
MeshData build_fish_body();

} // namespace alryn
