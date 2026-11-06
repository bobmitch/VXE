#pragma once
// -----------------------------------------------------------------------------
// StructureGraph: structural-integrity propagation for grid-snapped pieces.
//
// Model (inspired by Valheim's support system, re-derived):
//   * Each piece has a material with maxSupport, minSupport, and fractional
//     losses for vertical and horizontal transfer.
//   * A grounded piece (anchored in terrain) has support = maxSupport.
//   * Support flows across contacts:
//         s(q) = max over neighbours p of  min(max(q), s(p) - loss(q, dir) * max(q))
//     where dir is Vertical when p and q touch through a horizontal face
//     (stacked) and Horizontal otherwise (cantilevered).
//   * A piece with s < minSupport is broken and does not transmit support.
//
// Because s is non-increasing along any path, the fixed point is a widest-path
// problem: one Dijkstra pass with a max-heap seeded from grounded pieces gives
// the exact result for a connected component in O(E log V). Changes (placement,
// removal, terrain edits changing grounding) recompute only the affected
// components. Broken pieces are reported; the owner removes them and the
// next solve cascades naturally (they are excluded already, so one pass is
// final for a given set of anchors).
// -----------------------------------------------------------------------------

#include "vxe/core/Math.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vxe {

using PieceId = uint32_t;
inline constexpr PieceId kInvalidPiece = 0;

enum class StructuralMaterial : uint8_t { Wood, CoreWood, Stone, Iron, Count };

struct MaterialStrength {
    float maxSupport;
    float minSupport;
    float verticalLoss;     // fraction of maxSupport lost per stacked transfer
    float horizontalLoss;   // fraction of maxSupport lost per sideways transfer
};

constexpr MaterialStrength strengthOf(StructuralMaterial m) {
    switch (m) {
    case StructuralMaterial::Wood:     return {100.f, 10.f, 0.125f, 0.20f};
    case StructuralMaterial::CoreWood: return {140.f, 10.f, 0.10f, 0.15f};
    case StructuralMaterial::Stone:    return {1000.f, 100.f, 0.10f, 1.00f};   // cannot cantilever
    case StructuralMaterial::Iron:     return {1500.f, 20.f, 0.077f, 0.077f};
    default:                           return {100.f, 10.f, 0.125f, 0.20f};
    }
}

enum class ContactDir : uint8_t { Vertical, Horizontal };

class StructureGraph {
public:
    struct Node {
        StructuralMaterial material = StructuralMaterial::Wood;
        bool grounded = false;
        float support = 0.f;
        bool broken = false;
        std::vector<std::pair<PieceId, ContactDir>> edges;
    };

    void addNode(PieceId id, StructuralMaterial m, bool grounded);
    void removeNode(PieceId id);              // also removes incident edges
    void connect(PieceId a, PieceId b, ContactDir dir);
    void setGrounded(PieceId id, bool grounded);

    // Recomputes support for every component containing a seed. Returns the
    // pieces that are now broken (support < minSupport).
    std::vector<PieceId> solve(std::span<const PieceId> seeds);
    std::vector<PieceId> solveAll();

    const Node* node(PieceId id) const {
        auto it = m_nodes.find(id);
        return it == m_nodes.end() ? nullptr : &it->second;
    }
    size_t size() const { return m_nodes.size(); }

private:
    std::vector<PieceId> collectComponent(PieceId seed, std::unordered_set<PieceId>& visited) const;
    std::unordered_map<PieceId, Node> m_nodes;
};

} // namespace vxe
