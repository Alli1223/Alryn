#include <Alryn/Terrain/Terrain.h>

#include <Alryn/Renderer/Vulkan/VulkanDevice.h>

namespace alryn {

namespace {

// Stylized height/slope palette: grass on flat tops, dirt/rock on slopes, with a
// gentle height tint. Gives the low-poly terrain its DRG/Astroneer feel.
Vec3 default_colorize(const Vec3& position, const Vec3& normal) {
    const f32 up = glm::clamp(normal.y, 0.0f, 1.0f);
    const Vec3 grass{0.33f, 0.49f, 0.30f};
    const Vec3 dirt{0.40f, 0.32f, 0.24f};
    const Vec3 rock{0.40f, 0.40f, 0.43f};

    Vec3 color = glm::mix(rock, grass, glm::smoothstep(0.55f, 0.9f, up));
    color = glm::mix(dirt, color, glm::smoothstep(0.2f, 0.6f, up));

    const f32 height = glm::clamp(position.y * 0.05f + 0.5f, 0.0f, 1.0f);
    return color * (0.8f + 0.4f * height);
}

} // namespace

Terrain::Terrain(const IVec3& dims, f32 voxel_size, int chunk_cells, const Vec3& origin)
    : field_(dims, voxel_size, origin), chunk_cells_(chunk_cells), colorize_(default_colorize) {
    const IVec3 cells = field_.cell_count();
    for (int cz = 0; cz < cells.z; cz += chunk_cells_) {
        for (int cy = 0; cy < cells.y; cy += chunk_cells_) {
            for (int cx = 0; cx < cells.x; cx += chunk_cells_) {
                Chunk chunk;
                chunk.cell_min = IVec3{cx, cy, cz};
                chunk.cell_max = glm::min(IVec3{cx, cy, cz} + IVec3{chunk_cells_}, cells);
                chunk.dirty = true;
                chunks_.push_back(std::move(chunk));
            }
        }
    }
}

void Terrain::generate(const std::function<f32(const Vec3&)>& density) {
    field_.fill(density);
    for (Chunk& chunk : chunks_) {
        chunk.dirty = true;
    }
}

void Terrain::deform(const Vec3& center, f32 radius, f32 amount) {
    mark_dirty(field_.apply_sphere(center, radius, amount));
}

void Terrain::mark_dirty(const VoxelField::Region& region) {
    if (!region.valid) {
        return;
    }
    for (Chunk& chunk : chunks_) {
        // Chunk uses corners [cell_min, cell_max]; overlap with the edited box.
        const bool overlap = glm::all(glm::lessThanEqual(region.min, chunk.cell_max)) &&
                             glm::all(glm::greaterThanEqual(region.max, chunk.cell_min));
        if (overlap) {
            chunk.dirty = true;
        }
    }
}

void Terrain::rebuild_dirty(const vk::Device& device) {
    if (!any_dirty()) {
        return;
    }
    device.wait_idle(); // ensure no in-flight frame is using the meshes we replace
    for (Chunk& chunk : chunks_) {
        if (!chunk.dirty) {
            continue;
        }
        const MeshData data =
            mc::polygonize(field_, chunk.cell_min, chunk.cell_max, iso_, colorize_);
        chunk.mesh.destroy();
        if (!data.indices.empty()) {
            chunk.mesh.create(device, data);
        }
        chunk.dirty = false;
    }
}

bool Terrain::any_dirty() const {
    for (const Chunk& chunk : chunks_) {
        if (chunk.dirty) {
            return true;
        }
    }
    return false;
}

} // namespace alryn
