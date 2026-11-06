#include "vxe/building/StructureGraph.h"

#include <algorithm>
#include <queue>

namespace vxe {

void StructureGraph::addNode(PieceId id, StructuralMaterial m, bool grounded) {
    Node& n = m_nodes[id];
    n.material = m;
    n.grounded = grounded;
    n.support = grounded ? strengthOf(m).maxSupport : 0.f;
    n.broken = false;
    n.edges.clear();
}

void StructureGraph::removeNode(PieceId id) {
    auto it = m_nodes.find(id);
    if (it == m_nodes.end()) return;
    for (const auto& [other, dir] : it->second.edges) {
        auto o = m_nodes.find(other);
        if (o == m_nodes.end()) continue;
        std::erase_if(o->second.edges, [id](const auto& e) { return e.first == id; });
    }
    m_nodes.erase(it);
}

void StructureGraph::connect(PieceId a, PieceId b, ContactDir dir) {
    if (a == b) return;
    auto ia = m_nodes.find(a), ib = m_nodes.find(b);
    if (ia == m_nodes.end() || ib == m_nodes.end()) return;
    auto has = [](const Node& n, PieceId o) {
        return std::any_of(n.edges.begin(), n.edges.end(), [o](const auto& e) { return e.first == o; });
    };
    if (!has(ia->second, b)) ia->second.edges.emplace_back(b, dir);
    if (!has(ib->second, a)) ib->second.edges.emplace_back(a, dir);
}

void StructureGraph::setGrounded(PieceId id, bool grounded) {
    auto it = m_nodes.find(id);
    if (it != m_nodes.end()) it->second.grounded = grounded;
}

std::vector<PieceId> StructureGraph::collectComponent(PieceId seed, std::unordered_set<PieceId>& visited) const {
    std::vector<PieceId> comp;
    if (!m_nodes.count(seed) || visited.count(seed)) return comp;
    std::vector<PieceId> stack{seed};
    visited.insert(seed);
    while (!stack.empty()) {
        const PieceId p = stack.back();
        stack.pop_back();
        comp.push_back(p);
        for (const auto& [q, dir] : m_nodes.at(p).edges)
            if (visited.insert(q).second) stack.push_back(q);
    }
    return comp;
}

std::vector<PieceId> StructureGraph::solve(std::span<const PieceId> seeds) {
    std::vector<PieceId> broken;
    std::unordered_set<PieceId> visited;
    for (PieceId seed : seeds) {
        const std::vector<PieceId> comp = collectComponent(seed, visited);
        if (comp.empty()) continue;

        // Widest-path Dijkstra from grounded pieces.
        using Item = std::pair<float, PieceId>;
        std::priority_queue<Item> heap;
        for (PieceId p : comp) {
            Node& n = m_nodes[p];
            const MaterialStrength ms = strengthOf(n.material);
            n.support = n.grounded ? ms.maxSupport : -1e30f;
            if (n.grounded) heap.emplace(n.support, p);
        }
        while (!heap.empty()) {
            const auto [s, p] = heap.top();
            heap.pop();
            Node& np = m_nodes[p];
            if (s < np.support) continue;                                     // stale entry
            if (s < strengthOf(np.material).minSupport) continue;             // broken pieces don't transmit
            for (const auto& [q, dir] : np.edges) {
                Node& nq = m_nodes[q];
                const MaterialStrength mq = strengthOf(nq.material);
                const float loss = (dir == ContactDir::Vertical ? mq.verticalLoss : mq.horizontalLoss) * mq.maxSupport;
                const float cand = std::min(mq.maxSupport, s - loss);
                if (cand > nq.support) {
                    nq.support = cand;
                    heap.emplace(cand, q);
                }
            }
        }
        for (PieceId p : comp) {
            Node& n = m_nodes[p];
            n.support = std::max(n.support, 0.f);
            n.broken = n.support < strengthOf(n.material).minSupport;
            if (n.broken) broken.push_back(p);
        }
    }
    return broken;
}

std::vector<PieceId> StructureGraph::solveAll() {
    std::vector<PieceId> all;
    all.reserve(m_nodes.size());
    for (const auto& [id, n] : m_nodes) all.push_back(id);
    return solve(all);
}

} // namespace vxe
