#pragma once

#include <Alryn/Core/Types.h>
#include <Alryn/World/Prop.h>

#include <vector>

namespace alryn {

// The low-poly forest-prop catalogue: bushes, rocks and fallen logs. Built once on
// construction as CPU MeshData; the client uploads GPU meshes from it. Trees and
// ground vegetation live in MeshPrimitives but are conceptually part of the set.
class PropLibrary {
public:
    PropLibrary();

    const std::vector<PropDef>& bushes() const { return bushes_; }
    const std::vector<PropDef>& rocks() const { return rocks_; }
    const std::vector<PropDef>& logs() const { return logs_; }

    // Look up a placed instance's definition.
    const PropDef& resolve(const PropInstance& inst) const;

    // Building blocks, exposed for tests/tools.
    static PropDef build_bush(int variant);
    static PropDef build_rock(int variant);
    static PropDef build_log(int variant);

private:
    std::vector<PropDef> bushes_;
    std::vector<PropDef> rocks_;
    std::vector<PropDef> logs_;
};

} // namespace alryn
