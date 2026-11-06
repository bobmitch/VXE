#pragma once
// -----------------------------------------------------------------------------
// BuildingSystem: grid-snapped player structures, fully decoupled from terrain.
//
// Separation rules (enforced by construction):
//   1. Structures never write density and the voxel field never stores
//      structures. Pieces live on their own 0.5 m world-aligned build grid,
//      independent of the 1 m voxel lattice and of chunk boundaries.
//   2. Terrain influences structures only through ITerrainQuery: anchoring
//      (is the piece's base embedded in / resting on solid terrain?) and
//      placement validation (is the piece mostly buried?).
//   3. Terrain edits notify the BuildingSystem (ITerrainEditListener); pieces
//      whose foundations intersect the edited region are re-anchored and their
//      components re-solved. Unsupported pieces collapse (CollapseEvent ->
//      the game spawns debris bodies). Policy ProtectFoundations instead lets
//      tools refuse edits under anchored pieces (allowsTerrainEdit()).
//   4. Collision for pieces is owned by the game layer (one static Jolt box or
//      compound per building) - not by the terrain baker.
// -----------------------------------------------------------------------------

#include "vxe/building/StructureGraph.h"
#include "vxe/building/TerrainQuery.h"
#include "vxe/terraform/Terraformer.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vxe {

using PieceTypeId = uint16_t;

struct PieceType {
    std::string name;
    Vec3i sizeCells{4, 4, 1};              // in build-grid cells, unrotated (x, y, z)
    StructuralMaterial material = StructuralMaterial::Wood;
    bool canAnchor = true;                 // foundations/poles may ground on terrain; e.g. roofs cannot
};

struct BuildPiece {
    PieceId id = kInvalidPiece;
    PieceTypeId type = 0;
    Vec3i cellMin{}, cellMax{};            // inclusive build-grid cells
    uint8_t rotation = 0;                  // 0..3 quarter turns about Y
    bool grounded = false;
    Aabbd bounds(double cellSize) const { return {Vec3d(cellMin) * cellSize, Vec3d(cellMax + Vec3i(1)) * cellSize}; }
};

enum class TerrainEditPolicy : uint8_t { AllowAndCollapse, ProtectFoundations };

struct BuildingConfig {
    double cellSize = 0.5;                 // metres
    float maxBuriedFraction = 0.6f;        // reject placements more buried than this
    float anchorSampleFraction = 0.25f;    // base samples that must touch terrain to anchor
    float anchorTolerance = 0.15f;         // metres below the base still counted as contact
    TerrainEditPolicy editPolicy = TerrainEditPolicy::AllowAndCollapse;
};

enum class PlacementError : uint8_t { None, Overlap, Buried, Unsupported, UnknownType };

struct PlacementResult {
    PlacementError error = PlacementError::None;
    PieceId id = kInvalidPiece;
    float support = 0.f;
    bool ok() const { return error == PlacementError::None; }
};

struct CollapseEvent {
    PieceId id;
    PieceTypeId type;
    Aabbd bounds;
    StructuralMaterial material;
};

class BuildingSystem final : public ITerrainEditListener {
public:
    BuildingSystem(const ITerrainQuery& terrain, const BuildingConfig& cfg = {});

    PieceTypeId registerType(const PieceType& t);
    const PieceType& type(PieceTypeId id) const { return m_types.at(id); }

    // Snaps worldPos to the grid (piece min corner) and validates.
    PlacementResult tryPlace(PieceTypeId type, const Vec3d& worldPos, uint8_t rotation = 0);
    void remove(PieceId id);                       // player deconstruction; cascades

    // ITerrainEditListener
    void onTerrainEdited(const Aabbd& region) override;
    bool allowsTerrainEdit(const Aabbd& region) const;

    std::vector<CollapseEvent> drainCollapses() { return std::exchange(m_collapses, {}); }
    const BuildPiece* piece(PieceId id) const;
    float support(PieceId id) const;
    size_t pieceCount() const { return m_pieces.size(); }
    Vec3i snap(const Vec3d& p) const;

private:
    Vec3i rotatedSize(PieceTypeId t, uint8_t rot) const;
    bool evaluateGrounded(const BuildPiece& p) const;
    float buriedFraction(const BuildPiece& p) const;
    std::vector<std::pair<PieceId, ContactDir>> findContacts(const BuildPiece& p) const;
    void occupy(const BuildPiece& p, bool add);
    void collapse(std::vector<PieceId> broken);

    const ITerrainQuery& m_terrain;
    BuildingConfig m_cfg;
    std::vector<PieceType> m_types;
    std::unordered_map<PieceId, BuildPiece> m_pieces;
    std::unordered_map<Vec3i, PieceId, Vec3iHash> m_cells;    // occupied build-grid cells
    StructureGraph m_graph;
    std::vector<CollapseEvent> m_collapses;
    PieceId m_nextId = 1;
};

} // namespace vxe
