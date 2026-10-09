#pragma once

#include <Alryn/Core/Math.h>
#include <Alryn/Core/Types.h>
#include <Alryn/Renderer/Mesh.h>

#include <string>
#include <vector>

namespace alryn {

// Which pipeline a prop part is drawn with.
//   Opaque   - lit + shadowed (trunks, rocks, logs, posts, walls, furniture)
//   Foliage  - alpha-blended, lit, no depth write (leaves, bushes)
//   Emissive - self-lit, full bright (lantern glass, hearth fire, lit windows)
//   Roof     - opaque, but the client fades it when the player is inside (the whole
//              house shell: outer walls + roof + partitions, for a dollhouse view)
//   Glow     - self-lit + additive, depth-tested (light shafts + pools spilling from windows)
//   Door     - opaque, but animated: the client swings it about `hinge` (or lifts it, a portcullis)
//              open while anyone - a hero, a townsperson, a traveller - is near it
enum class PropLayer : u8 { Opaque, Foliage, Emissive, Roof, Glow, Door };

struct PropPart {
    MeshData mesh;
    PropLayer layer = PropLayer::Opaque;
    // A Door part: its mesh is modelled SHUT; opening turns it `swing` radians about the vertical axis
    // through `hinge` (both prop-local), or - with `lift` > 0 - raises it that far (a portcullis).
    Vec3 hinge{0.0f};
    f32 swing = 0.0f;
    f32 lift = 0.0f;
};

// A static box collider in the prop's local space (e.g. a fallen log / fence). xz
// blocking only; `height` is the vertical extent upward from center.y.
struct BoxCollider {
    Vec3 center{0.0f};
    Vec2 half_extents{0.5f};
    f32 height = 1.0f;
    f32 yaw = 0.0f;
};

// A spotlight attached to a prop (e.g. a path lantern), in the prop's local space.
struct PropLight {
    Vec3 offset{0.0f};                  // local position of the light
    Vec3 direction{0.0f, -1.0f, 0.0f};  // local spot direction
    Vec3 color{1.0f, 0.78f, 0.45f};     // warm lantern glow
    f32 range = 12.0f;
    f32 intensity = 1.4f;
    f32 cone_deg = 110.0f;
    // A building's light that shines OUTSIDE it (the warm spill out of a lit window onto the ground
    // beneath, a lantern hung by the door). Unlike the hearth / lamp light within, it isn't walled in, so
    // the client lights it from the cheap unshadowed pool rather than spending a shadow map on it.
    bool spill = false;
};

// A catalogue entry: a multi-part low-poly prop plus collision boxes + lights.
// `footprint` (houses) is the interior xz half-extent so the client can fade the
// roof when the local player steps inside.
struct PropDef {
    std::string name;
    std::vector<PropPart> parts;
    std::vector<BoxCollider> colliders;
    std::vector<PropLight> lights;
    Vec2 footprint{0.0f};
    f32 wall_height = 0.0f;
    // Houses: where the resident villager sleeps + a spot just outside the door, in
    // the house's local space. Vary per house variant so any shape places NPCs right.
    Vec3 bed_spot{0.0f};
    Vec3 door_spot{0.0f, 0.0f, 3.5f};
    // A spot just inside the front door, clear of the furniture: anyone walking in from door_spot gets
    // here (the tests walk that line against the colliders).
    Vec3 inside_spot{0.0f};
    // Top of the chimney pot in local space; the client anchors drifting hearth
    // smoke here. Zero = no chimney (no smoke).
    Vec3 chimney_spot{0.0f};
};

// Categories the world scatter can place. Trees and ground vegetation keep their
// own optimised paths; these are the discrete props: forest debris, the
// fences/lanterns that line the winding paths, and the medieval village pieces
// (cottages, perimeter walls and gate towers).
enum class PropCategory : u8 {
    Bush, Rock, Log, Fence, Lantern, House, Wall, Gate, Well, Bridge, Market,
    Path, Planter, Fountain, FenceRail,
    Decor, // medieval clutter that fills a town: barrels, crates, hay, stalls, signposts...
    River, // a sunken water channel tile (banks + water) for river-towns
    Crystal, // glowing magic crystal clusters scattered in the wilds (emissive + coloured light)
    GlowShroom, // bioluminescent mushroom clusters that glow at night
    Campfire,   // a cosy campfire (logs + flame + warm light) - rare wilderness rest spots
    Monument,   // weathered stone obelisk / broken pillar / standing stones (wilderness landmark)
    Watchtower  // a wooden lookout tower in the wilds
};

// How many crystal colour variants `PropLibrary` builds (amethyst, sapphire, emerald, ...).
inline constexpr u32 kCrystalVariants = 4;
// Glowing-mushroom colour variants (cyan, amber, violet).
inline constexpr u32 kGlowShroomVariants = 3;
// Monument variants (carved obelisk, broken pillar, standing-stones trio).
inline constexpr u32 kMonumentVariants = 3;

// How many distinct ordinary house variants `PropLibrary` builds (cottages, longhouses, two-
// storey houses, manors, jettied merchants' houses, crofts, farmhouses, ...). The village scatter
// fills with `variant % kHouseVariants`.
inline constexpr u32 kHouseVariants = 14;

// Special landmark buildings (a townhouse, a pub, a blacksmith) sit at indices kHouseVariants..
// kHouseDefs-1 in `PropLibrary::houses()`. The town layout sprinkles a few of these in among the
// ordinary homes (see Village.h), so a town has a tavern + smithy. `build_house(i)` dispatches to
// the matching `build_townhouse/pub/blacksmith` for these indices.
inline constexpr u32 kHouseTownhouse = kHouseVariants + 0; // a tall jettied townhouse
inline constexpr u32 kHousePub = kHouseVariants + 1;       // the tavern
inline constexpr u32 kHouseBlacksmith = kHouseVariants + 2; // the smithy
inline constexpr u32 kHouseChapel = kHouseVariants + 3;    // a stone chapel + bell tower (towns, cities)
inline constexpr u32 kHouseKeep = kHouseVariants + 4;      // a city's great stone keep
inline constexpr u32 kHouseBakery = kHouseVariants + 5;    // a bakery: a bread oven, loaves on the counter
inline constexpr u32 kHouseShop = kHouseVariants + 6;      // a merchant's shop: shelves of goods, an awning
inline constexpr u32 kHouseDefs = kHouseVariants + 7;      // distinct buildings (one season)
// The landmarks (chapel, keep) a town keeps clear around: no goods piles, gardens or flower barrels.
inline constexpr bool is_landmark(u32 variant) {
    return variant % kHouseDefs == kHouseChapel || variant % kHouseDefs == kHouseKeep;
}

// SNOWBOUND towns (above the snowline) build every structure's snow-capped twin: `houses()` holds the
// kHouseDefs buildings and then their snowy versions at variant + kSnowHouses; walls and gate towers
// likewise at + kSnowWalls / + kSnowGates, and the market at kSnowMarket.
inline constexpr u32 kSnowHouses = kHouseDefs;
inline constexpr u8 kSnowWalls = 2;
inline constexpr u8 kSnowGates = 2;
inline constexpr u8 kSnowMarket = 1;

// How many distinct Decor props `PropLibrary` builds (barrel, crates, hay, market stall,
// signpost, trough, woodpile, sacks, street bench, flower cart, banner poles (crimson / blue), notice
// board, flower barrel, pennant bunting, a fire brazier, a snowman). The town scatter picks a specific
// one by index.
inline constexpr u32 kDecorVariants = 17;
inline constexpr u8 kDecorBench = 8;
inline constexpr u8 kDecorFlowerCart = 9;
inline constexpr u8 kDecorBannerRed = 10;
inline constexpr u8 kDecorBannerBlue = 11;
inline constexpr u8 kDecorNoticeBoard = 12;
inline constexpr u8 kDecorFlowerBarrel = 13;
inline constexpr u8 kDecorBunting = 14; // a unit-long string of pennants (stretched to its span)
inline constexpr u8 kDecorBrazier = 15; // an iron fire-basket on a post: a warm blaze + light by night
inline constexpr u8 kDecorSnowman = 16; // the townsfolk's snowman (snowbound towns)

struct PropInstance {
    PropCategory category = PropCategory::Bush;
    u8 variant = 0;
    Vec3 position{0.0f}; // base on the ground
    f32 yaw = 0.0f;
    f32 scale = 1.0f;
    // Extra stretch along the prop's LOCAL +X axis (1 = none). A fence rail is modelled
    // unit-length and stretched by this to bridge the exact gap to the next post, so a run
    // of posts is joined by rails of varying length.
    f32 length = 1.0f;
};

} // namespace alryn
