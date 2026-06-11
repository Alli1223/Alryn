#pragma once

#include <Alryn/Core/Types.h>
#include <Alryn/World/Prop.h>

#include <vector>

namespace alryn {

// The low-poly prop catalogue: bushes, rocks and houses (with attached lanterns).
// Built once on construction as CPU MeshData; the client uploads GPU meshes from
// it. Trees and flowers live in MeshPrimitives but are conceptually part of the
// same set. House/lantern geometry is generated procedurally here.
class PropLibrary {
public:
    PropLibrary();

    const std::vector<PropDef>& bushes() const { return bushes_; }
    const std::vector<PropDef>& rocks() const { return rocks_; }
    const std::vector<PropDef>& houses() const { return houses_; }

    // Look up a placed instance's definition.
    const PropDef& resolve(const PropInstance& inst) const;

    // Building blocks, exposed for tests/tools.
    static PropDef build_bush(int variant);
    static PropDef build_rock(int variant);
    static PropDef build_lantern();
    static PropDef build_house(u32 seed);

private:
    std::vector<PropDef> bushes_;
    std::vector<PropDef> rocks_;
    std::vector<PropDef> houses_;
};

} // namespace alryn
