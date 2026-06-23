#pragma once

#include <Alryn/Core/Types.h>
#include <Alryn/World/Prop.h>

#include <vector>

namespace alryn {

// Cart wheel geometry, shared by the wagon mesh + the client's rolling animation.
inline constexpr f32 kWagonWheelRadius = 0.42f;
inline constexpr f32 kWagonWheelX = 0.8f;  // axle offset fore/aft (local x)
inline constexpr f32 kWagonWheelZ = 0.62f; // wheel offset left/right (local z)

// The low-poly forest-prop catalogue: bushes, rocks and fallen logs. Built once on
// construction as CPU MeshData; the client uploads GPU meshes from it. Trees and
// ground vegetation live in MeshPrimitives but are conceptually part of the set.
class PropLibrary {
public:
    PropLibrary();

    const std::vector<PropDef>& bushes() const { return bushes_; }
    const std::vector<PropDef>& rocks() const { return rocks_; }
    const std::vector<PropDef>& logs() const { return logs_; }
    const std::vector<PropDef>& fences() const { return fences_; }
    const std::vector<PropDef>& fence_rails() const { return fence_rails_; }
    const std::vector<PropDef>& lanterns() const { return lanterns_; }
    const std::vector<PropDef>& houses() const { return houses_; }
    const std::vector<PropDef>& walls() const { return walls_; }
    const std::vector<PropDef>& gates() const { return gates_; }
    const std::vector<PropDef>& wells() const { return wells_; }
    const std::vector<PropDef>& bridges() const { return bridges_; }
    const std::vector<PropDef>& markets() const { return markets_; }
    const std::vector<PropDef>& paths() const { return paths_; }
    const std::vector<PropDef>& planters() const { return planters_; }
    const std::vector<PropDef>& fountains() const { return fountains_; }
    const std::vector<PropDef>& decor() const { return decor_; }
    const std::vector<PropDef>& rivers() const { return rivers_; }
    const std::vector<PropDef>& crystals() const { return crystals_; }
    const std::vector<PropDef>& glow_shrooms() const { return glow_shrooms_; }
    const std::vector<PropDef>& campfires() const { return campfires_; }
    const std::vector<PropDef>& monuments() const { return monuments_; }
    const std::vector<PropDef>& watchtowers() const { return watchtowers_; }

    // Look up a placed instance's definition.
    const PropDef& resolve(const PropInstance& inst) const;

    // Building blocks, exposed for tests/tools.
    static PropDef build_bush(int variant);
    static PropDef build_rock(int variant);
    static PropDef build_log(int variant);
    static PropDef build_fence(int variant);      // a single fence post
    static PropDef build_fence_rail(int variant); // unit-length rails (stretched to the gap)
    static PropDef build_lantern_post();
    static PropDef build_house(u32 variant); // a village house (style by kHouseStyles)
    static Vec2 house_half_extents(u32 variant); // (w,d) footprint, for collision-free layout
    static PropDef build_townhouse();  // tall narrow jettied 3-storey (house variant)
    static PropDef build_pub();        // 2-storey tavern w/ hanging sign + beer garden (house variant)
    static PropDef build_blacksmith(); // workshop w/ open forge + anvil (house variant)
    static PropDef build_wall(int variant);  // stone perimeter wall segment
    static PropDef build_gate();             // lit stone gate tower (placed at gate gaps)
    static PropDef build_tower();            // plain unlit wall tower (periodic boundary towers)
    static PropDef build_well();             // village well (water source for firefighting)
    static PropDef build_bridge();           // raised walkway joining two houses
    static PropDef build_market();           // central marketplace (stalls + market cross)
    static PropDef build_wagon();            // a goods cart body (the networked transport entity)
    static PropDef build_wagon_wheel();      // a single cart wheel (drawn x4, spun by the client)
    static PropDef build_path_tile();        // a raised cobblestone street tile
    static PropDef build_planter();          // a pot of greenery / flowers
    static PropDef build_fountain();         // a stone plaza fountain
    static PropDef build_decor(int variant); // medieval town clutter (see kDecorVariants)
    static PropDef build_river();            // a sunken river-channel tile (banks + water)
    static PropDef build_crystal(int variant); // a glowing magic crystal cluster (see kCrystalVariants)
    static PropDef build_glow_shroom(int variant); // a bioluminescent mushroom cluster
    static PropDef build_campfire();               // a cosy campfire (logs + flame + warm light)
    static PropDef build_monument(int variant);    // weathered stone obelisk / pillar / standing stones
    static PropDef build_watchtower();             // a wooden lookout tower
    static PropDef build_stone_bridge();     // an arched stone road bridge across a river

private:
    std::vector<PropDef> bushes_;
    std::vector<PropDef> rocks_;
    std::vector<PropDef> logs_;
    std::vector<PropDef> fences_;
    std::vector<PropDef> fence_rails_;
    std::vector<PropDef> lanterns_;
    std::vector<PropDef> houses_;
    std::vector<PropDef> walls_;
    std::vector<PropDef> gates_;
    std::vector<PropDef> wells_;
    std::vector<PropDef> bridges_;
    std::vector<PropDef> markets_;
    std::vector<PropDef> paths_;
    std::vector<PropDef> planters_;
    std::vector<PropDef> fountains_;
    std::vector<PropDef> decor_;
    std::vector<PropDef> rivers_;
    std::vector<PropDef> crystals_;
    std::vector<PropDef> glow_shrooms_;
    std::vector<PropDef> campfires_;
    std::vector<PropDef> monuments_;
    std::vector<PropDef> watchtowers_;
};

} // namespace alryn
