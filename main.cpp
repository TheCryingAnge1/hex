#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Node {
    enum class Role {
        center,
        branch,
        leaf,
        split,
        frontier,
    };

    std::size_t id = 0;
    Role role = Role::frontier;
    std::array<int, 4> adj{-1, -1, -1, -1};

    int degree() const {
        int n = 0;
        for (int nb : adj) {
            if (nb != -1) {
                ++n;
            }
        }
        return n;
    }

    int free_slots() const { return 4 - degree(); }

    bool attach(int nb) {
        for (int& slot : adj) {
            if (slot == nb) {
                return true;
            }
            if (slot == -1) {
                slot = nb;
                return true;
            }
        }
        return false;
    }
};

struct SearchContext;
struct Graph;
void note_local_cache_hit(SearchContext* ctx);
void note_local_completion_state(SearchContext* ctx);
void note_generation_progress(SearchContext* ctx, const char* reason);
void note_root_candidate_call(SearchContext* ctx);
void note_variant_attempt(SearchContext* ctx);
void note_variant_materialized(SearchContext* ctx);
void note_capacity_prune(SearchContext* ctx);
void note_legal_edge_prune(SearchContext* ctx);
void note_dead_root_cache_hit(SearchContext* ctx);
void note_impossible_root_prune(SearchContext* ctx);
bool is_search_timed_out(SearchContext* ctx);
bool should_use_fast_bounds(SearchContext* ctx);
struct RootGenerationEstimate {
    std::uint64_t order_cost = 0;
    bool any_feasible_variant = false;
};

RootGenerationEstimate analyze_root_generation(const Graph& graph, int root,
                                               SearchContext* ctx = nullptr);

struct CompletionFingerprint {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    bool operator==(const CompletionFingerprint& other) const {
        return lo == other.lo && hi == other.hi;
    }
};

struct LocalBoundKey {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    bool operator==(const LocalBoundKey& other) const {
        return lo == other.lo && hi == other.hi;
    }
};

struct LocalBoundKeyHasher {
    std::size_t operator()(const LocalBoundKey& value) const {
        return static_cast<std::size_t>(value.lo ^ (value.hi * 0x94d049bb133111ebULL));
    }
};

struct CompletionFingerprintHasher {
    std::size_t operator()(const CompletionFingerprint& value) const {
        return static_cast<std::size_t>(value.lo ^ (value.hi * 0x517cc1b727220a95ULL));
    }
};

static std::unordered_map<CompletionFingerprint, int, CompletionFingerprintHasher>
    g_variant_upper_bound_cache;

bool has_dead_root_cache(SearchContext* ctx, const CompletionFingerprint& fp);
void remember_dead_root_cache(SearchContext* ctx, const CompletionFingerprint& fp);

struct FingerprintBuilder {
    std::uint64_t lo = 1469598103934665603ULL;
    std::uint64_t hi = 1099511628211ULL;

    void mix(std::uint64_t value) {
        lo ^= value + 0x9e3779b97f4a7c15ULL + (lo << 6) + (lo >> 2);
        lo *= 1099511628211ULL;
        hi += value + 0x9e3779b97f4a7c15ULL + (hi << 6) + (hi >> 2);
        hi ^= (value << 17) | (value >> 47);
        hi *= 14029467366897019727ULL;
    }
};

struct Graph {
    std::vector<Node> nodes;

    static constexpr int kMinGirth = 6;
    static constexpr int kChildrenPerBranch = 3;
    static constexpr int kBranchCount = 4;

    int add_node() {
        nodes.push_back(Node{.id = nodes.size()});
        return static_cast<int>(nodes.size() - 1);
    }

    int add_node(Node::Role role) {
        nodes.push_back(Node{.id = nodes.size(), .role = role});
        return static_cast<int>(nodes.size() - 1);
    }

    std::vector<int> neighbors(int id) const {
        std::vector<int> out;
        for (int nb : nodes[id].adj) {
            if (nb != -1) {
                out.push_back(nb);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool adjacent(int a, int b) const {
        for (int nb : nodes[a].adj) {
            if (nb == b) {
                return true;
            }
        }
        return false;
    }

    int shared_neighbor(int a, int b) const {
        for (int x : neighbors(a)) {
            if (x != b && adjacent(x, b)) {
                return x;
            }
        }
        return -1;
    }

    int bfs_dist(int src, int dst, int max_dist) const {
        if (src == dst) {
            return 0;
        }

        std::vector<int> dist(nodes.size(), -1);
        std::queue<int> q;
        dist[src] = 0;
        q.push(src);

        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            if (dist[cur] >= max_dist) {
                continue;
            }
            for (int nb : neighbors(cur)) {
                if (dist[nb] != -1) {
                    continue;
                }
                dist[nb] = dist[cur] + 1;
                if (nb == dst) {
                    return dist[nb];
                }
                q.push(nb);
            }
        }

        return max_dist + 1;
    }

    bool girth_ok(int a, int b, int min_girth = kMinGirth) const {
        if (a == b) {
            return false;
        }
        return bfs_dist(a, b, min_girth - 2) > min_girth - 2;
    }

    bool connect(int a, int b, int min_girth = kMinGirth) {
        if (a == b || adjacent(a, b) || !nodes[a].free_slots() || !nodes[b].free_slots()) {
            return false;
        }
        if (!girth_ok(a, b, min_girth)) {
            return false;
        }
        return nodes[a].attach(b) && nodes[b].attach(a);
    }

    int connect_new(int parent, Node::Role role = Node::Role::frontier) {
        int fresh = add_node(role);
        if (!connect(parent, fresh)) {
            return -1;
        }
        return fresh;
    }

    std::vector<int> ensure_neighbors(int node, int target, int exclude = -1,
                                      Node::Role role = Node::Role::frontier) {
        std::vector<int> out;
        for (int nb : neighbors(node)) {
            if (nb != exclude) {
                out.push_back(nb);
            }
        }
        while (static_cast<int>(out.size()) < target) {
            int fresh = connect_new(node, role);
            if (fresh == -1) {
                break;
            }
            out.push_back(fresh);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool can_split_with_new_node(int a, int b) const {
        return a != b && nodes[a].free_slots() > 0 && nodes[b].free_slots() > 0 &&
               bfs_dist(a, b, kMinGirth - 2) > kMinGirth - 3;
    }

    int ensure_split(int a, int b) {
        int existing = shared_neighbor(a, b);
        if (existing != -1) {
            return existing;
        }
        if (!can_split_with_new_node(a, b)) {
            return -1;
        }
        int middle = add_node(Node::Role::split);
        if (!connect(a, middle) || !connect(middle, b)) {
            return -1;
        }
        return middle;
    }

    struct Neighborhood {
        std::array<int, 4> branches{};
        std::array<std::array<int, 3>, 4> children{};
    };

    std::optional<Neighborhood> materialize_center_neighborhood(int root) {
        Neighborhood n{};
        auto branches = ensure_neighbors(root, kBranchCount, -1, Node::Role::branch);
        if (static_cast<int>(branches.size()) != kBranchCount) {
            return std::nullopt;
        }

        for (int i = 0; i < kBranchCount; ++i) {
            n.branches[i] = branches[i];
            nodes[branches[i]].role = Node::Role::branch;
        }

        for (int i = 0; i < kBranchCount; ++i) {
            auto kids = ensure_neighbors(n.branches[i], kChildrenPerBranch, root, Node::Role::leaf);
            if (static_cast<int>(kids.size()) != kChildrenPerBranch) {
                return std::nullopt;
            }
            for (int j = 0; j < kChildrenPerBranch; ++j) {
                n.children[i][j] = kids[j];
                nodes[kids[j]].role = Node::Role::leaf;
            }
        }

        return n;
    }

    std::optional<Neighborhood> materialize_branch_neighborhood(int root) {
        Neighborhood n{};
        std::vector<int> center_neighbors;
        std::vector<int> leaf_neighbors;
        for (int nb : neighbors(root)) {
            if (nodes[nb].role == Node::Role::center) {
                center_neighbors.push_back(nb);
            } else {
                leaf_neighbors.push_back(nb);
            }
        }
        if (center_neighbors.empty()) {
            return std::nullopt;
        }
        std::sort(center_neighbors.begin(), center_neighbors.end());
        std::sort(leaf_neighbors.begin(), leaf_neighbors.end());
        if (static_cast<int>(leaf_neighbors.size()) != 3) {
            return std::nullopt;
        }

        auto siblings = ensure_neighbors(center_neighbors.front(), 4, root, Node::Role::branch);
        siblings.erase(std::remove(siblings.begin(), siblings.end(), root), siblings.end());
        std::sort(siblings.begin(), siblings.end());
        if (static_cast<int>(siblings.size()) != 3) {
            return std::nullopt;
        }
        for (int i = 0; i < 3; ++i) {
            n.branches[i] = siblings[i];
            nodes[siblings[i]].role = Node::Role::branch;
        }
        n.branches[3] = center_neighbors.front();

        for (int i = 0; i < 3; ++i) {
            auto outward = ensure_neighbors(leaf_neighbors[i], 3, root, Node::Role::frontier);
            outward.erase(std::remove(outward.begin(), outward.end(), root), outward.end());
            std::sort(outward.begin(), outward.end());
            if (static_cast<int>(outward.size()) != 3) {
                return std::nullopt;
            }
            for (int j = 0; j < 3; ++j) {
                n.children[i][j] = outward[j];
            }
        }

        auto center_children = neighbors(center_neighbors.front());
        center_children.erase(std::remove(center_children.begin(), center_children.end(), root),
                              center_children.end());
        std::sort(center_children.begin(), center_children.end());
        if (static_cast<int>(center_children.size()) != 3) {
            return std::nullopt;
        }
        for (int j = 0; j < 3; ++j) {
            n.children[3][j] = center_children[j];
        }

        return n;
    }

    std::optional<Neighborhood> materialize_generic_neighborhood(int root) {
        Neighborhood n{};
        auto branches = ensure_neighbors(root, kBranchCount, -1, Node::Role::frontier);
        if (static_cast<int>(branches.size()) != kBranchCount) {
            return std::nullopt;
        }

        for (int i = 0; i < kBranchCount; ++i) {
            n.branches[i] = branches[i];
        }

        for (int i = 0; i < kBranchCount; ++i) {
            auto kids = ensure_neighbors(n.branches[i], kChildrenPerBranch, root, Node::Role::frontier);
            kids.erase(std::remove(kids.begin(), kids.end(), root), kids.end());
            std::sort(kids.begin(), kids.end());
            if (static_cast<int>(kids.size()) < kChildrenPerBranch) {
                return std::nullopt;
            }
            for (int j = 0; j < kChildrenPerBranch; ++j) {
                n.children[i][j] = kids[j];
            }
        }

        return n;
    }

    std::optional<Neighborhood> materialize_leaf_neighborhood_with_choices(
        int root, int branch_choice, std::array<int, 2> split_choices, int outward_choice) {
        Neighborhood n{};
        std::vector<int> branch_side;
        std::vector<int> split_side;
        std::vector<int> outward_side;

        for (int nb : neighbors(root)) {
            if (nodes[nb].role == Node::Role::branch || nodes[nb].role == Node::Role::center) {
                branch_side.push_back(nb);
            } else if (nodes[nb].role == Node::Role::split) {
                split_side.push_back(nb);
            } else {
                outward_side.push_back(nb);
            }
        }

        if (branch_side.empty()) {
            auto all = neighbors(root);
            if (all.empty()) {
                return std::nullopt;
            }
            branch_side.push_back(all.front());
        }

        while (static_cast<int>(split_side.size()) < 2 && nodes[root].free_slots() > 0) {
            int fresh = connect_new(root, Node::Role::split);
            if (fresh == -1) {
                break;
            }
            split_side.push_back(fresh);
        }
        if (split_side.size() < 2) {
            return std::nullopt;
        }

        if (outward_side.empty()) {
            int fresh = connect_new(root, Node::Role::frontier);
            if (fresh != -1) {
                outward_side.push_back(fresh);
            }
        }
        if (outward_side.empty()) {
            return std::nullopt;
        }

        if (branch_choice < 0 || branch_choice >= static_cast<int>(branch_side.size())) {
            return std::nullopt;
        }
        if (split_choices[0] < 0 || split_choices[1] < 0 ||
            split_choices[0] >= static_cast<int>(split_side.size()) ||
            split_choices[1] >= static_cast<int>(split_side.size()) ||
            split_choices[0] == split_choices[1]) {
            return std::nullopt;
        }
        if (outward_choice < 0 || outward_choice >= static_cast<int>(outward_side.size())) {
            return std::nullopt;
        }

        n.branches[0] = branch_side[branch_choice];

        std::array<int, 2> split_parent{-1, -1};
        for (int i = 0; i < 2; ++i) {
            int split = split_side[split_choices[i]];
            int opposite = -1;
            for (int nb : neighbors(split)) {
                if (nb != root) {
                    opposite = nb;
                    break;
                }
            }
            if (opposite == -1) {
                opposite = connect_new(split, Node::Role::leaf);
                if (opposite == -1) {
                    return std::nullopt;
                }
                nodes[opposite].role = Node::Role::leaf;
            }
            n.branches[i + 1] = opposite;
            split_parent[i] = split;
        }
        n.branches[3] = outward_side[outward_choice];

        auto fill_children = [&](int slot, int branch_node, int exclude,
                                 Node::Role role) -> bool {
            auto kids = ensure_neighbors(branch_node, 3, exclude, role);
            kids.erase(std::remove(kids.begin(), kids.end(), exclude), kids.end());
            std::sort(kids.begin(), kids.end());
            if (static_cast<int>(kids.size()) < 3) {
                return false;
            }
            for (int j = 0; j < 3; ++j) {
                n.children[slot][j] = kids[j];
            }
            return true;
        };

        if (!fill_children(0, n.branches[0], root, Node::Role::leaf)) {
            return std::nullopt;
        }
        if (!fill_children(1, n.branches[1], split_parent[0], Node::Role::frontier)) {
            return std::nullopt;
        }
        if (!fill_children(2, n.branches[2], split_parent[1], Node::Role::frontier)) {
            return std::nullopt;
        }
        if (!fill_children(3, n.branches[3], root, Node::Role::frontier)) {
            return std::nullopt;
        }

        return n;
    }

    std::optional<Neighborhood> materialize_leaf_neighborhood(int root) {
        return materialize_leaf_neighborhood_with_choices(root, 0, {0, 1}, 0);
    }

    std::optional<Neighborhood> materialize_neighborhood(int root) {
        if (nodes[root].role == Node::Role::branch) {
            return materialize_branch_neighborhood(root);
        }
        if (nodes[root].role == Node::Role::leaf) {
            auto leaf_n = materialize_leaf_neighborhood(root);
            if (leaf_n && (rooted_loop_count(*leaf_n) >= 12 || has_completion_move(*leaf_n))) {
                return leaf_n;
            }
            return materialize_center_neighborhood(root);
        }
        if (nodes[root].role == Node::Role::split || nodes[root].role == Node::Role::frontier) {
            auto generic_n = materialize_generic_neighborhood(root);
            if (generic_n && (rooted_loop_count(*generic_n) >= 12 ||
                              has_completion_move(*generic_n))) {
                return generic_n;
            }
            return materialize_center_neighborhood(root);
        }
        return materialize_center_neighborhood(root);
    }

    static std::array<int, 12> flatten(const Neighborhood& n,
                                       const std::array<int, 4>& branch_order,
                                       const std::array<std::array<int, 3>, 4>& child_order) {
        std::array<int, 12> flat{};
        for (int b = 0; b < 4; ++b) {
            for (int c = 0; c < 3; ++c) {
                flat[b * 3 + c] = n.children[branch_order[b]][child_order[b][c]];
            }
        }
        return flat;
    }

    struct CompletionPlan {
        std::vector<std::pair<int, int>> add_pairs;
        int reused_pairs = 0;
        int new_nodes = 0;
    };

    static constexpr int branch_pair_index(int a, int b) {
        if (a > b) {
            std::swap(a, b);
        }
        int idx = 0;
        for (int i = 0; i < 4; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                if (i == a && j == b) {
                    return idx;
                }
                ++idx;
            }
        }
        return -1;
    }

    static int total_loops_from_pair_counts(const std::array<int, 6>& pair_counts) {
        int loops = 0;
        for (int c : pair_counts) {
            loops += c;
        }
        return loops;
    }

    bool pair_balance_satisfied(const std::array<int, 6>& pair_counts) const {
        for (int c : pair_counts) {
            if (c != 2) {
                return false;
            }
        }
        return true;
    }

    bool pair_balance_feasible(const std::array<int, 6>& pair_counts) const {
        for (int c : pair_counts) {
            if (c > 2) {
                return false;
            }
        }
        return true;
    }

    std::array<int, 6> rooted_pair_loop_counts(const Neighborhood& n) const {
        std::array<int, 6> pair_counts{};
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                int idx = branch_pair_index(a, b);
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        if (shared_neighbor(n.children[a][i], n.children[b][j]) != -1) {
                            ++pair_counts[idx];
                        }
                    }
                }
            }
        }
        return pair_counts;
    }

    int rooted_loop_count(const Neighborhood& n) const {
        return total_loops_from_pair_counts(rooted_pair_loop_counts(n));
    }

    CompletionFingerprint completion_fingerprint(int root, const Neighborhood& n) const {
        std::array<int, 12> flat{};
        for (int a = 0; a < 4; ++a) {
            for (int i = 0; i < 3; ++i) {
                flat[a * 3 + i] = n.children[a][i];
            }
        }

        FingerprintBuilder builder;
        builder.mix(static_cast<std::uint64_t>(static_cast<int>(nodes[root].role)));
        for (int idx : flat) {
            builder.mix(static_cast<std::uint64_t>(static_cast<int>(nodes[idx].role)));
            builder.mix(static_cast<std::uint64_t>(nodes[idx].degree()));
            builder.mix(static_cast<std::uint64_t>(nodes[idx].free_slots()));
        }
        for (int lhs = 0; lhs < 12; ++lhs) {
            for (int rhs = lhs + 1; rhs < 12; ++rhs) {
                if (lhs / 3 == rhs / 3) {
                    continue;
                }
                int a = flat[lhs];
                int b = flat[rhs];
                builder.mix(static_cast<std::uint64_t>(shared_neighbor(a, b) != -1));
                builder.mix(static_cast<std::uint64_t>(can_split_with_new_node(a, b)));
            }
        }
        return CompletionFingerprint{.lo = builder.lo, .hi = builder.hi};
    }

    int rooted_participation_count(const Neighborhood& n, int child) const {
        int count = 0;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        int lhs = n.children[a][i];
                        int rhs = n.children[b][j];
                        if (lhs != child && rhs != child) {
                            continue;
                        }
                        if (shared_neighbor(lhs, rhs) != -1) {
                            ++count;
                        }
                    }
                }
            }
        }
        return count;
    }

    bool participation_limit_reached(const Neighborhood& n, int root, int child) const {
        if (nodes[root].role != Node::Role::center) {
            return false;
        }
        return rooted_participation_count(n, child) >= 2;
    }

    int participation_capacity_left(const Neighborhood& n, int root, int child) const {
        if (nodes[root].role != Node::Role::center) {
            return nodes[child].free_slots();
        }
        return std::min(nodes[child].free_slots(), 2 - rooted_participation_count(n, child));
    }

    int max_additional_loops_capacity_bound(const Neighborhood& n, int root) const {
        int total_capacity = 0;
        for (int a = 0; a < 4; ++a) {
            for (int i = 0; i < 3; ++i) {
                total_capacity += std::max(0, participation_capacity_left(n, root, n.children[a][i]));
            }
        }
        return total_capacity / 2;
    }

    int max_additional_loops_legal_edge_bound(
        const Neighborhood& n, int root,
        const std::vector<std::pair<int, int>>& legal_edges) const {
        std::array<int, 12> flat{};
        std::array<int, 12> capacities{};
        std::unordered_map<int, int> index_by_child;
        index_by_child.reserve(12);

        for (int a = 0; a < 4; ++a) {
            for (int i = 0; i < 3; ++i) {
                int idx = a * 3 + i;
                int child = n.children[a][i];
                flat[idx] = child;
                capacities[idx] = std::max(0, participation_capacity_left(n, root, child));
                index_by_child.emplace(child, idx);
            }
        }

        std::array<int, 12> incident_counts{};
        for (const auto& [a, b] : legal_edges) {
            auto ita = index_by_child.find(a);
            auto itb = index_by_child.find(b);
            if (ita == index_by_child.end() || itb == index_by_child.end()) {
                continue;
            }
            ++incident_counts[ita->second];
            ++incident_counts[itb->second];
        }

        int total_usable_capacity = 0;
        for (int idx = 0; idx < 12; ++idx) {
            total_usable_capacity += std::min(capacities[idx], incident_counts[idx]);
        }
        return total_usable_capacity / 2;
    }

    static constexpr std::array<std::pair<int, int>, 54> child_pair_positions() {
        std::array<std::pair<int, int>, 54> pairs{};
        int pos = 0;
        for (int a = 0; a < 12; ++a) {
            for (int b = a + 1; b < 12; ++b) {
                if (a / 3 == b / 3) {
                    continue;
                }
                pairs[pos++] = {a, b};
            }
        }
        return pairs;
    }

    static LocalBoundKey pack_local_bound_key(const std::array<int, 12>& capacities,
                                              std::uint64_t edge_mask) {
        std::uint64_t lo = edge_mask;
        std::uint64_t hi = 0;
        for (int i = 0; i < 12; ++i) {
            hi |= (static_cast<std::uint64_t>(capacities[i] & 0xf) << (i * 4));
        }
        return LocalBoundKey{.lo = lo, .hi = hi};
    }

    int max_additional_loops_exact_bound(
        const Neighborhood& n, int root,
        const std::vector<std::pair<int, int>>& legal_edges) const {
        static constexpr auto kPairs = child_pair_positions();

        std::array<int, 12> capacities{};
        std::unordered_map<int, int> index_by_child;
        index_by_child.reserve(12);
        for (int a = 0; a < 4; ++a) {
            for (int i = 0; i < 3; ++i) {
                int idx = a * 3 + i;
                int child = n.children[a][i];
                capacities[idx] = std::max(0, participation_capacity_left(n, root, child));
                index_by_child.emplace(child, idx);
            }
        }

        std::uint64_t edge_mask = 0;
        for (const auto& [lhs_child, rhs_child] : legal_edges) {
            auto it_lhs = index_by_child.find(lhs_child);
            auto it_rhs = index_by_child.find(rhs_child);
            if (it_lhs == index_by_child.end() || it_rhs == index_by_child.end()) {
                continue;
            }
            int lhs = std::min(it_lhs->second, it_rhs->second);
            int rhs = std::max(it_lhs->second, it_rhs->second);
            for (int pos = 0; pos < static_cast<int>(kPairs.size()); ++pos) {
                if (kPairs[pos].first == lhs && kPairs[pos].second == rhs) {
                    edge_mask |= (1ULL << pos);
                    break;
                }
            }
        }

        std::unordered_map<LocalBoundKey, int, LocalBoundKeyHasher> memo;
        memo.reserve(1 << 14);

        auto dfs = [&](auto&& self, std::array<int, 12> cur_caps,
                       std::uint64_t cur_mask) -> int {
            if (cur_mask == 0) {
                return 0;
            }

            LocalBoundKey key = pack_local_bound_key(cur_caps, cur_mask);
            auto it = memo.find(key);
            if (it != memo.end()) {
                return it->second;
            }

            int best_pos = -1;
            int best_score = 1000;
            for (int pos = 0; pos < static_cast<int>(kPairs.size()); ++pos) {
                if (((cur_mask >> pos) & 1ULL) == 0) {
                    continue;
                }
                const auto& [u, v] = kPairs[pos];
                if (cur_caps[u] <= 0 || cur_caps[v] <= 0) {
                    continue;
                }
                int score = cur_caps[u] + cur_caps[v];
                if (score < best_score) {
                    best_score = score;
                    best_pos = pos;
                }
            }

            if (best_pos == -1) {
                memo.emplace(key, 0);
                return 0;
            }

            const auto& [u, v] = kPairs[best_pos];
            int best = self(self, cur_caps, cur_mask & ~(1ULL << best_pos));

            std::array<int, 12> next_caps = cur_caps;
            --next_caps[u];
            --next_caps[v];
            std::uint64_t next_mask = cur_mask & ~(1ULL << best_pos);
            for (int pos = 0; pos < static_cast<int>(kPairs.size()); ++pos) {
                if (((next_mask >> pos) & 1ULL) == 0) {
                    continue;
                }
                const auto& [a, b] = kPairs[pos];
                if ((a == u && next_caps[u] == 0) || (b == u && next_caps[u] == 0) ||
                    (a == v && next_caps[v] == 0) || (b == v && next_caps[v] == 0)) {
                    next_mask &= ~(1ULL << pos);
                }
            }
            best = std::max(best, 1 + self(self, next_caps, next_mask));

            memo.emplace(key, best);
            return best;
        };

        return dfs(dfs, capacities, edge_mask);
    }

    bool has_completion_move(const Neighborhood& n) const {
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        int lhs = n.children[a][i];
                        int rhs = n.children[b][j];
                        if (shared_neighbor(lhs, rhs) != -1 ||
                            can_split_with_new_node(lhs, rhs)) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    std::vector<CompletionPlan> generate_completion_candidates_for_materialized(
        int root, const Neighborhood& n, SearchContext* ctx = nullptr,
        std::size_t max_results = 0,
        const std::vector<std::pair<int, int>>* pending_roots = nullptr,
        int max_depth = -1) const {
        const bool enforce_pair_balance = (nodes[root].role == Node::Role::center);
        std::set<std::vector<std::pair<int, int>>> seen;
        std::vector<CompletionPlan> out;
        auto initial_pairs = rooted_pair_loop_counts(n);
        if ((enforce_pair_balance && pair_balance_satisfied(initial_pairs)) ||
            (!enforce_pair_balance && rooted_loop_count(n) >= 12)) {
            out.push_back(CompletionPlan{});
            return out;
        }

        std::optional<CompletionPlan> greedy_plan;
        {
            Graph greedy = *this;
            Neighborhood greedy_n = n;
            std::vector<std::pair<int, int>> chosen;

            while (true) {
                if (is_search_timed_out(ctx)) {
                    chosen.clear();
                    break;
                }
                std::array<int, 6> pair_counts = greedy.rooted_pair_loop_counts(greedy_n);
                if ((enforce_pair_balance && greedy.pair_balance_satisfied(pair_counts)) ||
                    (!enforce_pair_balance && greedy.rooted_loop_count(greedy_n) >= 12)) {
                    break;
                }
                if (enforce_pair_balance && !greedy.pair_balance_feasible(pair_counts)) {
                    chosen.clear();
                    break;
                }
                int best_a = -1;
                int best_b = -1;
                int best_score = -1;

                for (int a = 0; a < 4; ++a) {
                    for (int b = a + 1; b < 4; ++b) {
                        for (int i = 0; i < 3; ++i) {
                            for (int j = 0; j < 3; ++j) {
                                int lhs = greedy_n.children[a][i];
                                int rhs = greedy_n.children[b][j];
                                int pair_idx = branch_pair_index(a, b);
                                if (enforce_pair_balance && pair_counts[pair_idx] >= 2) {
                                    continue;
                                }
                                if (greedy.participation_limit_reached(greedy_n, root, lhs) ||
                                    greedy.participation_limit_reached(greedy_n, root, rhs)) {
                                    continue;
                                }
                                if (greedy.shared_neighbor(lhs, rhs) != -1 ||
                                    !greedy.can_split_with_new_node(lhs, rhs)) {
                                    continue;
                                }
                                int score = 10 * (greedy.nodes[lhs].free_slots() +
                                                  greedy.nodes[rhs].free_slots()) -
                                            (greedy.nodes[lhs].degree() +
                                             greedy.nodes[rhs].degree());
                                if (score > best_score) {
                                    best_score = score;
                                    best_a = std::min(lhs, rhs);
                                    best_b = std::max(lhs, rhs);
                                }
                            }
                        }
                    }
                }

                if (best_a == -1 || greedy.ensure_split(best_a, best_b) == -1) {
                    chosen.clear();
                    break;
                }
                chosen.push_back({best_a, best_b});
            }

            std::array<int, 6> final_pair_counts = greedy.rooted_pair_loop_counts(greedy_n);
            if ((enforce_pair_balance && greedy.pair_balance_satisfied(final_pair_counts)) ||
                (!enforce_pair_balance && greedy.rooted_loop_count(greedy_n) >= 12)) {
                std::sort(chosen.begin(), chosen.end());
                chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
                greedy_plan = CompletionPlan{
                    .add_pairs = chosen,
                    .reused_pairs = greedy.rooted_loop_count(root) - static_cast<int>(chosen.size()),
                    .new_nodes = static_cast<int>(chosen.size()),
                };
            }
        }

        if (greedy_plan && seen.insert(greedy_plan->add_pairs).second) {
            out.push_back(*greedy_plan);
        }
        CompletionFingerprint initial_fp = completion_fingerprint(root, n);
        if (has_dead_root_cache(ctx, initial_fp)) {
            note_dead_root_cache_hit(ctx);
            return out;
        }

        struct LocalCompletionState {
            CompletionFingerprint fp;
            std::uint64_t forbidden_mask = 0;

            bool operator==(const LocalCompletionState& other) const {
                return fp == other.fp && forbidden_mask == other.forbidden_mask;
            }
        };

        struct LocalCompletionStateHasher {
            std::size_t operator()(const LocalCompletionState& value) const {
                return static_cast<std::size_t>(
                    (value.fp.lo ^ (value.fp.hi * 0x517cc1b727220a95ULL)) ^
                    (value.forbidden_mask * 0x9e3779b97f4a7c15ULL));
            }
        };

        std::unordered_set<LocalCompletionState, LocalCompletionStateHasher> local_seen;
        local_seen.reserve(1 << 15);

        static constexpr auto kPairs = child_pair_positions();

        auto search = [&](auto&& self, const Graph& cur_graph,
                          const Neighborhood& cur_n, std::uint64_t forbidden_mask,
                          std::vector<std::pair<int, int>>& chosen) -> void {
                if (is_search_timed_out(ctx)) {
                    return;
                }
                if (max_results != 0 && out.size() >= max_results) {
                    return;
                }

                LocalCompletionState local_state{
                    .fp = cur_graph.completion_fingerprint(root, cur_n),
                    .forbidden_mask = forbidden_mask,
                };
                if (!local_seen.insert(local_state).second) {
                    note_local_cache_hit(ctx);
                    return;
                }
                note_local_completion_state(ctx);

                if (pending_roots && chosen.size() >= 4) {
                    for (const auto& [pending_root, pending_depth] : *pending_roots) {
                        if (max_depth >= 0 && pending_depth > max_depth) {
                            continue;
                        }
                        if (!analyze_root_generation(cur_graph, pending_root, ctx)
                                 .any_feasible_variant) {
                            note_impossible_root_prune(ctx);
                            return;
                        }
                    }
                }

                std::array<int, 6> pair_counts = cur_graph.rooted_pair_loop_counts(cur_n);
                int loops = total_loops_from_pair_counts(pair_counts);
                if (enforce_pair_balance && !cur_graph.pair_balance_feasible(pair_counts)) {
                    return;
                }
                if ((enforce_pair_balance && cur_graph.pair_balance_satisfied(pair_counts)) ||
                    (!enforce_pair_balance && loops >= 12)) {
                    std::vector<std::pair<int, int>> normalized = chosen;
                    std::sort(normalized.begin(), normalized.end());
                    normalized.erase(std::unique(normalized.begin(), normalized.end()),
                                     normalized.end());
                    if (!seen.insert(normalized).second) {
                        return;
                    }
                    out.push_back(CompletionPlan{
                        .add_pairs = normalized,
                        .reused_pairs = loops - static_cast<int>(normalized.size()),
                        .new_nodes = static_cast<int>(normalized.size()),
                    });
                    return;
                }

                int remaining_needed = 12 - loops;
                if (cur_graph.max_additional_loops_capacity_bound(cur_n, root) < remaining_needed) {
                    note_capacity_prune(ctx);
                    return;
                }

                struct CandidateEdge {
                    int pos;
                    int a;
                    int b;
                    int score;
                };

                std::vector<CandidateEdge> edges;
                std::array<int, 6> candidate_edges_by_pair{};
                for (int pos = 0; pos < static_cast<int>(kPairs.size()); ++pos) {
                    if (((forbidden_mask >> pos) & 1ULL) != 0) {
                        continue;
                    }
                    const auto& [lhs_idx, rhs_idx] = kPairs[pos];
                    int pair_idx = branch_pair_index(lhs_idx / 3, rhs_idx / 3);
                    if (enforce_pair_balance && pair_counts[pair_idx] >= 2) {
                        continue;
                    }
                    int lhs = cur_n.children[lhs_idx / 3][lhs_idx % 3];
                    int rhs = cur_n.children[rhs_idx / 3][rhs_idx % 3];
                    if (cur_graph.participation_limit_reached(cur_n, root, lhs) ||
                        cur_graph.participation_limit_reached(cur_n, root, rhs)) {
                        continue;
                    }
                    if (cur_graph.shared_neighbor(lhs, rhs) != -1 ||
                        !cur_graph.can_split_with_new_node(lhs, rhs)) {
                        continue;
                    }
                    edges.push_back(CandidateEdge{
                        .pos = pos,
                        .a = std::min(lhs, rhs),
                        .b = std::max(lhs, rhs),
                        .score = 10 * (cur_graph.nodes[lhs].free_slots() +
                                       cur_graph.nodes[rhs].free_slots()) -
                                 (cur_graph.nodes[lhs].degree() +
                                  cur_graph.nodes[rhs].degree()),
                    });
                    if (enforce_pair_balance) {
                        ++candidate_edges_by_pair[pair_idx];
                    }
                }

                if (enforce_pair_balance) {
                    for (int pair_idx = 0; pair_idx < 6; ++pair_idx) {
                        int deficit = std::max(0, 2 - pair_counts[pair_idx]);
                        if (candidate_edges_by_pair[pair_idx] < deficit) {
                            note_legal_edge_prune(ctx);
                            return;
                        }
                    }
                }

                std::sort(edges.begin(), edges.end(), [](const CandidateEdge& lhs,
                                                         const CandidateEdge& rhs) {
                    if (lhs.score != rhs.score) {
                        return lhs.score > rhs.score;
                    }
                    if (lhs.a != rhs.a) {
                        return lhs.a < rhs.a;
                    }
                    return lhs.b < rhs.b;
                });
                edges.erase(std::unique(edges.begin(), edges.end(),
                                        [](const CandidateEdge& lhs,
                                           const CandidateEdge& rhs) {
                                            return lhs.a == rhs.a && lhs.b == rhs.b;
                                        }),
                            edges.end());

                if (static_cast<int>(edges.size()) < remaining_needed) {
                    return;
                }

                std::vector<std::pair<int, int>> legal_pairs;
                legal_pairs.reserve(edges.size());
                for (const auto& edge : edges) {
                    legal_pairs.push_back({edge.a, edge.b});
                }
                const bool use_fast_bound_only = should_use_fast_bounds(ctx);
                int additional_upper =
                    use_fast_bound_only
                        ? cur_graph.max_additional_loops_legal_edge_bound(cur_n, root, legal_pairs)
                        : cur_graph.max_additional_loops_exact_bound(cur_n, root, legal_pairs);
                if (additional_upper < remaining_needed) {
                    note_legal_edge_prune(ctx);
                    return;
                }

                std::unordered_map<int, int> incident_counts;
                incident_counts.reserve(12);
                for (const auto& edge : edges) {
                    ++incident_counts[edge.a];
                    ++incident_counts[edge.b];
                }
                for (auto& edge : edges) {
                    int lhs_options = incident_counts[edge.a];
                    int rhs_options = incident_counts[edge.b];
                    edge.score = 1000 - 100 * (lhs_options + rhs_options) +
                                 10 * (cur_graph.nodes[edge.a].free_slots() +
                                       cur_graph.nodes[edge.b].free_slots()) -
                                 (cur_graph.nodes[edge.a].degree() +
                                  cur_graph.nodes[edge.b].degree());
                }

                const CandidateEdge edge = edges.front();

                Graph include_graph = cur_graph;
                if (include_graph.ensure_split(edge.a, edge.b) != -1) {
                    chosen.push_back({edge.a, edge.b});
                    self(self, include_graph, cur_n, forbidden_mask | (1ULL << edge.pos), chosen);
                    chosen.pop_back();
                    if (max_results != 0 && out.size() >= max_results) {
                        return;
                    }
                }

                self(self, cur_graph, cur_n, forbidden_mask | (1ULL << edge.pos), chosen);
        };

        std::vector<std::pair<int, int>> chosen;
        search(search, *this, n, 0, chosen);

        if (out.empty()) {
            remember_dead_root_cache(ctx, initial_fp);
        }

        std::sort(out.begin(), out.end(), [](const CompletionPlan& lhs,
                                             const CompletionPlan& rhs) {
            if (lhs.reused_pairs != rhs.reused_pairs) {
                return lhs.reused_pairs > rhs.reused_pairs;
            }
            return lhs.new_nodes < rhs.new_nodes;
        });
        if (max_results != 0 && out.size() > max_results) {
            out.resize(max_results);
        }
        return out;
    }

    std::vector<CompletionPlan> generate_completion_candidates(int root,
                                                               std::size_t max_results = 0) const {
        Graph base = *this;
        auto maybe_n = base.materialize_neighborhood(root);
        if (!maybe_n) {
            return {};
        }
        return base.generate_completion_candidates_for_materialized(root, *maybe_n, nullptr,
                                                                    max_results);
    }

    int rooted_loop_count(int root) const {
        auto branches = neighbors(root);
        if (static_cast<int>(branches.size()) != 4) {
            return -1;
        }

        std::array<std::vector<int>, 4> children{};
        for (int i = 0; i < 4; ++i) {
            children[i] = neighbors(branches[i]);
            children[i].erase(std::remove(children[i].begin(), children[i].end(), root),
                              children[i].end());
            if (static_cast<int>(children[i].size()) != 3) {
                return -1;
            }
        }

        std::array<int, 6> pair_counts{};
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                int idx = branch_pair_index(a, b);
                for (int i : children[a]) {
                    for (int j : children[b]) {
                        if (shared_neighbor(i, j) != -1) {
                            ++pair_counts[idx];
                        }
                    }
                }
            }
        }
        return total_loops_from_pair_counts(pair_counts);
    }

    std::array<int, 6> rooted_pair_loop_counts(int root) const {
        auto branches = neighbors(root);
        if (static_cast<int>(branches.size()) != 4) {
            return {-1, -1, -1, -1, -1, -1};
        }

        std::array<std::vector<int>, 4> children{};
        for (int i = 0; i < 4; ++i) {
            children[i] = neighbors(branches[i]);
            children[i].erase(std::remove(children[i].begin(), children[i].end(), root),
                              children[i].end());
            if (static_cast<int>(children[i].size()) != 3) {
                return {-1, -1, -1, -1, -1, -1};
            }
        }

        std::array<int, 6> pair_counts{};
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                int idx = branch_pair_index(a, b);
                for (int i : children[a]) {
                    for (int j : children[b]) {
                        if (shared_neighbor(i, j) != -1) {
                            ++pair_counts[idx];
                        }
                    }
                }
            }
        }
        return pair_counts;
    }

    std::vector<int> distances_from(int start, int max_depth) const {
        std::vector<int> dist(nodes.size(), -1);
        std::queue<int> q;
        dist[start] = 0;
        q.push(start);

        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            if (dist[cur] == max_depth) {
                continue;
            }

            for (int nb : neighbors(cur)) {
                if (dist[nb] != -1) {
                    continue;
                }
                dist[nb] = dist[cur] + 1;
                q.push(nb);
            }
        }

        return dist;
    }

    bool has_short_cycle_through(int edge_u, int edge_v, int min_girth) const {
        std::vector<int> dist(nodes.size(), -1);
        std::queue<int> q;
        dist[edge_u] = 0;
        q.push(edge_u);

        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            if (dist[cur] >= min_girth - 2) {
                continue;
            }

            for (int nb : neighbors(cur)) {
                if ((cur == edge_u && nb == edge_v) || (cur == edge_v && nb == edge_u)) {
                    continue;
                }
                if (nb == edge_v) {
                    return true;
                }
                if (dist[nb] == -1) {
                    dist[nb] = dist[cur] + 1;
                    q.push(nb);
                }
            }
        }

        return false;
    }

    bool validate_depth(int start, int max_depth) const {
        std::vector<int> dist = distances_from(start, max_depth);

        for (const Node& node : nodes) {
            if (node.id < dist.size() && dist[node.id] != -1 && dist[node.id] <= max_depth &&
                node.degree() != 4) {
                std::cout << "degree failure at node " << node.id
                          << ": degree=" << node.degree() << '\n';
                return false;
            }
        }

        for (const Node& node : nodes) {
            for (int nb : neighbors(node.id)) {
                if (node.id < static_cast<std::size_t>(nb) &&
                    has_short_cycle_through(static_cast<int>(node.id), nb, kMinGirth)) {
                    std::cout << "girth failure on edge " << node.id << "-" << nb << '\n';
                    return false;
                }
            }
        }

        for (const Node& node : nodes) {
            if (node.id < dist.size() && dist[node.id] != -1 && dist[node.id] <= max_depth) {
                int loops = rooted_loop_count(static_cast<int>(node.id));
                std::array<int, 6> pair_counts =
                    rooted_pair_loop_counts(static_cast<int>(node.id));
                if (loops != 12 || !pair_balance_satisfied(pair_counts)) {
                    std::cout << "loop failure at node " << node.id
                              << ": loops=" << loops << " pair-counts=[";
                    for (int i = 0; i < 6; ++i) {
                        std::cout << pair_counts[i] << (i + 1 == 6 ? "" : ",");
                    }
                    std::cout << "]\n";
                    return false;
                }
            }
        }

        return true;
    }
};

struct SearchState {
    Graph graph;
    std::deque<std::pair<int, int>> todo;
    std::vector<int> depth_seen;
    std::vector<bool> completed;
};

struct RootCandidate {
    Graph graph;
    Graph::CompletionPlan plan;
};

std::vector<RootCandidate> generate_root_candidates(const Graph& source, int root,
                                                    SearchContext* ctx = nullptr,
                                                    std::size_t max_results = 0,
                                                    const std::vector<std::pair<int, int>>* pending_roots = nullptr,
                                                    int max_depth = -1) {
    std::vector<RootCandidate> out;
    std::set<std::vector<std::pair<int, int>>> seen_plans;
    note_root_candidate_call(ctx);

    auto append_variant = [&](Graph& base, const Graph::Neighborhood& n) {
        std::vector<Graph::CompletionPlan> plans =
            base.generate_completion_candidates_for_materialized(root, n, ctx, max_results,
                                                                 pending_roots, max_depth);
        for (const auto& plan : plans) {
            if (!seen_plans.insert(plan.add_pairs).second) {
                continue;
            }
            out.push_back(RootCandidate{
                .graph = base,
                .plan = plan,
            });
            if (max_results != 0 && out.size() >= max_results) {
                return;
            }
        }
    };

    if (source.nodes[root].role == Node::Role::leaf) {
        std::vector<int> branch_side;
        std::vector<int> split_side;
        std::vector<int> outward_side;
        for (int nb : source.neighbors(root)) {
            if (source.nodes[nb].role == Node::Role::branch ||
                source.nodes[nb].role == Node::Role::center) {
                branch_side.push_back(nb);
            } else if (source.nodes[nb].role == Node::Role::split) {
                split_side.push_back(nb);
            } else {
                outward_side.push_back(nb);
            }
        }
        if (branch_side.empty()) {
            branch_side = source.neighbors(root);
        }

        std::set<std::array<int, 4>> seen_branch_assignments;
        const int branch_choice_count = std::max<int>(1, static_cast<int>(branch_side.size()));
        const int outward_choice_count = std::max<int>(1, static_cast<int>(outward_side.size()));
        const int split_choice_count = std::max<int>(2, static_cast<int>(split_side.size()));

        for (int branch_choice = 0; branch_choice < branch_choice_count; ++branch_choice) {
            for (int split_a = 0; split_a < split_choice_count; ++split_a) {
                for (int split_b = split_a + 1; split_b < split_choice_count; ++split_b) {
                    for (int outward_choice = 0; outward_choice < outward_choice_count;
                         ++outward_choice) {
                        note_variant_attempt(ctx);
                        Graph base = source;
                        auto maybe_n = base.materialize_leaf_neighborhood_with_choices(
                            root, branch_choice, {split_a, split_b}, outward_choice);
                        if (!maybe_n) {
                            continue;
                        }
                        if (!seen_branch_assignments.insert(maybe_n->branches).second) {
                            continue;
                        }
                        note_variant_materialized(ctx);
                        append_variant(base, *maybe_n);
                        if (max_results != 0 && out.size() >= max_results) {
                            return out;
                        }
                    }
                }
            }
        }

        Graph generic = source;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic && seen_branch_assignments.insert(maybe_generic->branches).second) {
            note_variant_materialized(ctx);
            append_variant(generic, *maybe_generic);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }

        Graph fallback = source;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            note_variant_materialized(ctx);
            append_variant(fallback, *maybe_fallback);
        }
    } else if (source.nodes[root].role == Node::Role::branch) {
        Graph branch = source;
        auto maybe_branch = branch.materialize_branch_neighborhood(root);
        if (maybe_branch) {
            note_variant_materialized(ctx);
            append_variant(branch, *maybe_branch);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }

        Graph generic = source;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic) {
            note_variant_materialized(ctx);
            append_variant(generic, *maybe_generic);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }

        Graph fallback = source;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            note_variant_materialized(ctx);
            append_variant(fallback, *maybe_fallback);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }
    } else if (source.nodes[root].role == Node::Role::split ||
               source.nodes[root].role == Node::Role::frontier) {
        Graph generic = source;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic) {
            note_variant_materialized(ctx);
            append_variant(generic, *maybe_generic);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }

        Graph fallback = source;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            note_variant_materialized(ctx);
            append_variant(fallback, *maybe_fallback);
            if (max_results != 0 && out.size() >= max_results) {
                return out;
            }
        }
    } else {
        Graph base = source;
        auto maybe_n = base.materialize_neighborhood(root);
        if (maybe_n) {
            note_variant_materialized(ctx);
            append_variant(base, *maybe_n);
        }
    }

    return out;
}

std::vector<std::pair<int, int>> collect_legal_completion_pairs(const Graph& graph,
                                                                const Graph::Neighborhood& n,
                                                                int root) {
    std::vector<std::pair<int, int>> legal_pairs;
    std::array<int, 6> pair_counts = graph.rooted_pair_loop_counts(n);
    const bool enforce_pair_balance = (graph.nodes[root].role == Node::Role::center);
    legal_pairs.reserve(54);
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            int pair_idx = Graph::branch_pair_index(a, b);
            if (enforce_pair_balance && pair_counts[pair_idx] >= 2) {
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    int lhs = n.children[a][i];
                    int rhs = n.children[b][j];
                    if (graph.participation_limit_reached(n, root, lhs) ||
                        graph.participation_limit_reached(n, root, rhs)) {
                        continue;
                    }
                    if (graph.shared_neighbor(lhs, rhs) != -1 ||
                        !graph.can_split_with_new_node(lhs, rhs)) {
                        continue;
                    }
                    legal_pairs.push_back({std::min(lhs, rhs), std::max(lhs, rhs)});
                }
            }
        }
    }
    std::sort(legal_pairs.begin(), legal_pairs.end());
    legal_pairs.erase(std::unique(legal_pairs.begin(), legal_pairs.end()), legal_pairs.end());
    return legal_pairs;
}

std::array<int, 6> legal_completion_pair_capacity(const Graph& graph,
                                                  const Graph::Neighborhood& n, int root) {
    std::array<int, 6> capacity{};
    std::array<int, 6> pair_counts = graph.rooted_pair_loop_counts(n);
    const bool enforce_pair_balance = (graph.nodes[root].role == Node::Role::center);
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            int pair_idx = Graph::branch_pair_index(a, b);
            if (enforce_pair_balance && pair_counts[pair_idx] >= 2) {
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    int lhs = n.children[a][i];
                    int rhs = n.children[b][j];
                    if (graph.participation_limit_reached(n, root, lhs) ||
                        graph.participation_limit_reached(n, root, rhs)) {
                        continue;
                    }
                    if (graph.shared_neighbor(lhs, rhs) != -1 ||
                        !graph.can_split_with_new_node(lhs, rhs)) {
                        continue;
                    }
                    ++capacity[pair_idx];
                }
            }
        }
    }
    return capacity;
}

RootGenerationEstimate analyze_root_generation(const Graph& graph, int root,
                                               SearchContext* ctx) {
    std::uint64_t variant_count = 0;
    int best_upper_bound = std::numeric_limits<int>::max();
    int worst_upper_bound = std::numeric_limits<int>::min();
    int best_slack = std::numeric_limits<int>::max();

    auto consider_variant = [&](Graph& base, const Graph::Neighborhood& n) {
        if (is_search_timed_out(ctx)) {
            return;
        }
        ++variant_count;
        CompletionFingerprint fp = base.completion_fingerprint(root, n);
        int upper_bound = 0;
        auto it = g_variant_upper_bound_cache.find(fp);
        if (it != g_variant_upper_bound_cache.end()) {
            upper_bound = it->second;
        } else {
            std::array<int, 6> pair_counts = base.rooted_pair_loop_counts(n);
            const bool enforce_pair_balance = (base.nodes[root].role == Node::Role::center);
            if (enforce_pair_balance && !base.pair_balance_feasible(pair_counts)) {
                upper_bound = -1;
            } else {
                int loops = Graph::total_loops_from_pair_counts(pair_counts);
                std::vector<std::pair<int, int>> legal_pairs =
                    collect_legal_completion_pairs(base, n, root);
                int global_upper = loops + base.max_additional_loops_exact_bound(n, root, legal_pairs);

                int pair_upper = global_upper;
                if (enforce_pair_balance) {
                    std::array<int, 6> pair_legal_capacity =
                        legal_completion_pair_capacity(base, n, root);
                    pair_upper = 0;
                    for (int pair_idx = 0; pair_idx < 6; ++pair_idx) {
                        pair_upper +=
                            std::min(2, pair_counts[pair_idx] + pair_legal_capacity[pair_idx]);
                    }
                }
                upper_bound = std::min(global_upper, pair_upper);
            }
            g_variant_upper_bound_cache.emplace(fp, upper_bound);
        }
        worst_upper_bound = std::max(worst_upper_bound, upper_bound);
        int slack = std::max(0, upper_bound - 12);
        best_upper_bound = std::min(best_upper_bound, upper_bound);
        best_slack = std::min(best_slack, slack);
    };

    if (graph.nodes[root].role == Node::Role::leaf) {
        std::vector<int> branch_side;
        std::vector<int> split_side;
        std::vector<int> outward_side;
        for (int nb : graph.neighbors(root)) {
            if (graph.nodes[nb].role == Node::Role::branch ||
                graph.nodes[nb].role == Node::Role::center) {
                branch_side.push_back(nb);
            } else if (graph.nodes[nb].role == Node::Role::split) {
                split_side.push_back(nb);
            } else {
                outward_side.push_back(nb);
            }
        }
        if (branch_side.empty()) {
            branch_side = graph.neighbors(root);
        }

        std::set<std::array<int, 4>> seen_branch_assignments;
        const int branch_choice_count = std::max<int>(1, static_cast<int>(branch_side.size()));
        const int outward_choice_count = std::max<int>(1, static_cast<int>(outward_side.size()));
        const int split_choice_count = std::max<int>(2, static_cast<int>(split_side.size()));

        for (int branch_choice = 0; branch_choice < branch_choice_count; ++branch_choice) {
            for (int split_a = 0; split_a < split_choice_count; ++split_a) {
                for (int split_b = split_a + 1; split_b < split_choice_count; ++split_b) {
                    for (int outward_choice = 0; outward_choice < outward_choice_count;
                         ++outward_choice) {
                        if (is_search_timed_out(ctx)) {
                            return RootGenerationEstimate{};
                        }
                        Graph base = graph;
                        auto maybe_n = base.materialize_leaf_neighborhood_with_choices(
                            root, branch_choice, {split_a, split_b}, outward_choice);
                        if (!maybe_n) {
                            continue;
                        }
                        if (!seen_branch_assignments.insert(maybe_n->branches).second) {
                            continue;
                        }
                        consider_variant(base, *maybe_n);
                    }
                }
            }
        }

        Graph generic = graph;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic && seen_branch_assignments.insert(maybe_generic->branches).second) {
            consider_variant(generic, *maybe_generic);
        }

        Graph fallback = graph;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            consider_variant(fallback, *maybe_fallback);
        }
    } else if (graph.nodes[root].role == Node::Role::branch) {
        if (is_search_timed_out(ctx)) {
            return RootGenerationEstimate{};
        }
        Graph branch = graph;
        auto maybe_branch = branch.materialize_branch_neighborhood(root);
        if (maybe_branch) {
            consider_variant(branch, *maybe_branch);
        }

        Graph generic = graph;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic) {
            consider_variant(generic, *maybe_generic);
        }

        Graph fallback = graph;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            consider_variant(fallback, *maybe_fallback);
        }
    } else if (graph.nodes[root].role == Node::Role::split ||
               graph.nodes[root].role == Node::Role::frontier) {
        if (is_search_timed_out(ctx)) {
            return RootGenerationEstimate{};
        }
        Graph generic = graph;
        auto maybe_generic = generic.materialize_generic_neighborhood(root);
        if (maybe_generic) {
            consider_variant(generic, *maybe_generic);
        }

        Graph fallback = graph;
        auto maybe_fallback = fallback.materialize_center_neighborhood(root);
        if (maybe_fallback) {
            consider_variant(fallback, *maybe_fallback);
        }
    } else {
        Graph base = graph;
        auto maybe_n = base.materialize_neighborhood(root);
        if (maybe_n) {
            consider_variant(base, *maybe_n);
        }
    }

    if (variant_count == 0) {
        return RootGenerationEstimate{};
    }

    if (worst_upper_bound < 12) {
        return RootGenerationEstimate{
            .order_cost = 0,
            .any_feasible_variant = false,
        };
    }

    return RootGenerationEstimate{
        .order_cost = static_cast<std::uint64_t>(best_slack) * 1000000ULL + variant_count,
        .any_feasible_variant = true,
    };
}

struct StateFingerprint {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;

    bool operator==(const StateFingerprint& other) const {
        return lo == other.lo && hi == other.hi;
    }
};

struct StateFingerprintHasher {
    std::size_t operator()(const StateFingerprint& value) const {
        return static_cast<std::size_t>(value.lo ^ (value.hi * 0x9e3779b97f4a7c15ULL));
    }
};

StateFingerprint fingerprint_state(const SearchState& state) {
    const int node_count = static_cast<int>(state.graph.nodes.size());
    if (node_count == 0) {
        return {};
    }

    std::vector<int> colors(node_count, 0);
    for (int i = 0; i < node_count; ++i) {
        const Node& node = state.graph.nodes[i];
        const int depth = i < static_cast<int>(state.depth_seen.size()) ? state.depth_seen[i] : -1;
        const int done =
            (i < static_cast<int>(state.completed.size()) && state.completed[i]) ? 1 : 0;
        int role_mask = 0;
        for (int nb : node.adj) {
            if (nb != -1) {
                role_mask |= 1 << static_cast<int>(state.graph.nodes[nb].role);
            }
        }
        colors[i] = static_cast<int>(node.role) * 10000 + node.degree() * 1000 +
                    node.free_slots() * 100 + done * 10 + (depth + 1);
        colors[i] = colors[i] * 64 + role_mask;
    }

    for (int round = 0; round < 8; ++round) {
        std::unordered_map<std::string, int> intern;
        intern.reserve(node_count * 2);
        std::vector<int> next_colors(node_count, 0);
        int next_id = 1;

        for (int i = 0; i < node_count; ++i) {
            std::vector<int> neighbor_colors;
            neighbor_colors.reserve(4);
            for (int nb : state.graph.nodes[i].adj) {
                neighbor_colors.push_back(nb == -1 ? -1 : colors[nb]);
            }
            std::sort(neighbor_colors.begin(), neighbor_colors.end());

            std::string key;
            key.reserve(64);
            key += std::to_string(colors[i]);
            key.push_back('|');
            for (int color : neighbor_colors) {
                key += std::to_string(color);
                key.push_back(',');
            }

            auto [it, inserted] = intern.emplace(key, next_id);
            if (inserted) {
                ++next_id;
            }
            next_colors[i] = it->second;
        }

        if (next_colors == colors) {
            break;
        }
        colors = std::move(next_colors);
    }

    std::vector<std::tuple<int, int, int, std::array<int, 4>>> descriptors;
    descriptors.reserve(node_count);
    for (int i = 0; i < node_count; ++i) {
        std::array<int, 4> neighbor_colors{-1, -1, -1, -1};
        for (int j = 0; j < 4; ++j) {
            int nb = state.graph.nodes[i].adj[j];
            neighbor_colors[j] = nb == -1 ? -1 : colors[nb];
        }
        std::sort(neighbor_colors.begin(), neighbor_colors.end());
        const int depth = i < static_cast<int>(state.depth_seen.size()) ? state.depth_seen[i] : -1;
        const int done =
            (i < static_cast<int>(state.completed.size()) && state.completed[i]) ? 1 : 0;
        descriptors.push_back({colors[i], depth, done, neighbor_colors});
    }
    std::sort(descriptors.begin(), descriptors.end());

    std::vector<std::pair<int, int>> todo_summary;
    todo_summary.reserve(state.todo.size());
    for (const auto& [node, depth] : state.todo) {
        todo_summary.push_back({depth, colors[node]});
    }
    std::sort(todo_summary.begin(), todo_summary.end());

    FingerprintBuilder builder;
    builder.mix(static_cast<std::uint64_t>(node_count));
    for (const auto& [color, depth, done, neighbor_colors] : descriptors) {
        builder.mix(static_cast<std::uint64_t>(color));
        builder.mix(static_cast<std::uint64_t>(depth + 2));
        builder.mix(static_cast<std::uint64_t>(done));
        for (int neighbor_color : neighbor_colors) {
            builder.mix(static_cast<std::uint64_t>(neighbor_color + 2));
        }
    }
    builder.mix(static_cast<std::uint64_t>(todo_summary.size()));
    for (const auto& [depth, color] : todo_summary) {
        builder.mix(static_cast<std::uint64_t>(depth + 2));
        builder.mix(static_cast<std::uint64_t>(color));
    }
    return StateFingerprint{.lo = builder.lo, .hi = builder.hi};
}

struct SearchContext {
    std::unordered_set<StateFingerprint, StateFingerprintHasher> visited;
    std::unordered_set<CompletionFingerprint, CompletionFingerprintHasher> dead_local_completion_roots;
    std::uint64_t states_entered = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t candidate_batches = 0;
    std::uint64_t candidate_plans = 0;
    std::uint64_t root_candidate_calls = 0;
    std::uint64_t variant_attempts = 0;
    std::uint64_t variants_materialized = 0;
    std::uint64_t local_completion_states = 0;
    std::uint64_t local_completion_cache_hits = 0;
    std::uint64_t dead_root_cache_hits = 0;
    std::uint64_t impossible_root_prunes = 0;
    std::uint64_t local_capacity_prunes = 0;
    std::uint64_t local_legal_edge_prunes = 0;
    std::uint64_t dead_end_no_candidates = 0;
    std::uint64_t materialize_failures = 0;
    std::uint64_t split_apply_failures = 0;
    std::uint64_t solutions_checked = 0;
    std::uint64_t progress_interval_states = 1000;
    double per_seed_time_limit_seconds = 0.0;
    int target_depth = 0;
    int active_root = -1;
    int active_generation_depth = -1;
    std::size_t active_generation_todo = 0;
    std::size_t active_generation_nodes = 0;
    std::vector<std::uint64_t> states_by_depth;
    std::vector<std::uint64_t> dead_ends_by_depth;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_report_time = start_time;
    bool time_limit_hit = false;
    bool time_limit_reported = false;

    void ensure_depth_slot(int depth) {
        if (depth >= static_cast<int>(states_by_depth.size())) {
            states_by_depth.resize(depth + 1, 0);
            dead_ends_by_depth.resize(depth + 1, 0);
        }
    }

    void report_progress(const char* reason, int active_depth, std::size_t cache_size,
                         std::size_t todo_size, std::size_t node_count) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        const double delta_s =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - last_report_time)
                .count();
        if (reason == nullptr) {
            if (states_entered == 0 || states_entered % progress_interval_states != 0) {
                return;
            }
            if (delta_s < 1.0) {
                return;
            }
        }

        std::cout << "[progress] depth=" << target_depth
                  << " reason=" << (reason ? reason : "tick")
                  << " elapsed_s=" << elapsed_s
                  << " states=" << states_entered
                  << " cache_size=" << cache_size
                  << " cache_hits=" << cache_hits
                  << " candidate_batches=" << candidate_batches
                  << " candidate_plans=" << candidate_plans
                  << " root_candidate_calls=" << root_candidate_calls
                  << " variant_attempts=" << variant_attempts
                  << " variants_materialized=" << variants_materialized
                  << " local_completion_states=" << local_completion_states
                  << " local_completion_cache_hits=" << local_completion_cache_hits
                  << " dead_root_cache_hits=" << dead_root_cache_hits
                  << " impossible_root_prunes=" << impossible_root_prunes
                  << " local_capacity_prunes=" << local_capacity_prunes
                  << " local_legal_edge_prunes=" << local_legal_edge_prunes
                  << " dead_no_candidates=" << dead_end_no_candidates
                  << " materialize_failures=" << materialize_failures
                  << " split_apply_failures=" << split_apply_failures
                  << " solutions_checked=" << solutions_checked
                  << " active_depth=" << active_depth
                  << " active_root=" << active_root
                  << " todo=" << todo_size
                  << " nodes=" << node_count
                  << '\n';

        std::cout << "[progress-depths]";
        for (int depth = 0; depth < static_cast<int>(states_by_depth.size()); ++depth) {
            std::cout << " d" << depth << "=" << states_by_depth[depth]
                      << "/dead=" << dead_ends_by_depth[depth];
        }
        std::cout << '\n';
        last_report_time = now;
    }

    void report_generation_progress(const char* reason) {
        const auto now = std::chrono::steady_clock::now();
        const double delta_s =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - last_report_time)
                .count();
        if (reason == nullptr) {
            if (local_completion_states == 0 ||
                local_completion_states % progress_interval_states != 0) {
                return;
            }
            if (delta_s < 1.0) {
                return;
            }
        }
        report_progress(reason ? reason : "root-gen-tick", active_generation_depth,
                        visited.size(), active_generation_todo, active_generation_nodes);
    }

    bool timed_out() {
        if (time_limit_hit) {
            return true;
        }
        if (per_seed_time_limit_seconds <= 0.0) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_time).count();
        if (elapsed_s >= per_seed_time_limit_seconds) {
            time_limit_hit = true;
            return true;
        }
        return false;
    }
};

bool is_search_timed_out(SearchContext* ctx) {
    if (!ctx) {
        return false;
    }
    return ctx->timed_out();
}

bool should_use_fast_bounds(SearchContext* ctx) {
    if (!ctx) {
        return false;
    }
    return ctx->per_seed_time_limit_seconds > 0.0;
}

void note_local_cache_hit(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->local_completion_cache_hits;
    ctx->report_generation_progress(nullptr);
}

void note_local_completion_state(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->local_completion_states;
    ctx->report_generation_progress(nullptr);
}

void note_generation_progress(SearchContext* ctx, const char* reason) {
    if (!ctx) {
        return;
    }
    ctx->report_generation_progress(reason);
}

void note_root_candidate_call(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->root_candidate_calls;
}

void note_variant_attempt(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->variant_attempts;
}

void note_variant_materialized(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->variants_materialized;
    ctx->report_generation_progress(nullptr);
}

void note_capacity_prune(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->local_capacity_prunes;
    ctx->report_generation_progress(nullptr);
}

void note_legal_edge_prune(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->local_legal_edge_prunes;
    ctx->report_generation_progress(nullptr);
}

void note_dead_root_cache_hit(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->dead_root_cache_hits;
    ctx->report_generation_progress(nullptr);
}

void note_impossible_root_prune(SearchContext* ctx) {
    if (!ctx) {
        return;
    }
    ++ctx->impossible_root_prunes;
    ctx->report_generation_progress(nullptr);
}

bool has_dead_root_cache(SearchContext* ctx, const CompletionFingerprint& fp) {
    if (!ctx) {
        return false;
    }
    return ctx->dead_local_completion_roots.contains(fp);
}

void remember_dead_root_cache(SearchContext* ctx, const CompletionFingerprint& fp) {
    if (!ctx) {
        return;
    }
    ctx->dead_local_completion_roots.insert(fp);
}

bool enqueue_neighbors(SearchState& state, int root, int depth, int max_depth) {
    if (depth == max_depth) {
        return true;
    }

    for (int nb : state.graph.neighbors(root)) {
        if (nb >= static_cast<int>(state.depth_seen.size())) {
            state.depth_seen.resize(state.graph.nodes.size(), -1);
            state.completed.resize(state.graph.nodes.size(), false);
        } else if (state.depth_seen.size() < state.graph.nodes.size()) {
            state.depth_seen.resize(state.graph.nodes.size(), -1);
            state.completed.resize(state.graph.nodes.size(), false);
        }

        int next_depth = depth + 1;
        if (state.depth_seen[nb] == -1 || next_depth < state.depth_seen[nb]) {
            state.depth_seen[nb] = next_depth;
            state.todo.push_back({nb, next_depth});
        }
    }
    return true;
}

bool expand_with_backtracking(SearchContext& ctx, const SearchState& state, int max_depth) {
    if (ctx.timed_out()) {
        if (!ctx.time_limit_reported) {
            ctx.report_progress("seed-time-limit", -1, ctx.visited.size(), state.todo.size(),
                                state.graph.nodes.size());
            ctx.time_limit_reported = true;
        }
        return false;
    }
    SearchState cur = state;
    while (!cur.todo.empty() && cur.completed[cur.todo.front().first]) {
        cur.todo.pop_front();
    }

    if (cur.todo.empty()) {
        ++ctx.solutions_checked;
        return cur.graph.validate_depth(0, max_depth);
    }

    int active_depth = cur.todo.front().second;
    ctx.ensure_depth_slot(active_depth);
    ++ctx.states_entered;
    ++ctx.states_by_depth[active_depth];

    StateFingerprint fp = fingerprint_state(cur);
    if (!ctx.visited.insert(fp).second) {
        ++ctx.cache_hits;
        ctx.report_progress(nullptr, active_depth, ctx.visited.size(), cur.todo.size(),
                            cur.graph.nodes.size());
        return false;
    }
    ctx.report_progress(nullptr, active_depth, ctx.visited.size(), cur.todo.size(),
                        cur.graph.nodes.size());

    int chosen_index = -1;
    int chosen_root = -1;
    int chosen_depth = -1;
    std::vector<RootCandidate> chosen_candidates;
    std::uint64_t best_estimate = 0;

    for (int i = 0; i < static_cast<int>(cur.todo.size()); ++i) {
        auto [root, depth] = cur.todo[i];
        RootGenerationEstimate estimate = analyze_root_generation(cur.graph, root, &ctx);
        if (chosen_index == -1 || estimate.order_cost < best_estimate ||
            (estimate.order_cost == best_estimate && depth < chosen_depth) ||
            (estimate.order_cost == best_estimate && depth == chosen_depth && root < chosen_root)) {
            chosen_index = i;
            chosen_root = root;
            chosen_depth = depth;
            best_estimate = estimate.order_cost;
        }
    }

    if (chosen_index != -1) {
        ctx.active_root = chosen_root;
        ctx.active_generation_depth = chosen_depth;
        ctx.active_generation_todo = cur.todo.size();
        ctx.active_generation_nodes = cur.graph.nodes.size();
        std::vector<std::pair<int, int>> pending_roots;
        pending_roots.reserve(cur.todo.size());
        for (int i = 0; i < static_cast<int>(cur.todo.size()); ++i) {
            if (i == chosen_index) {
                continue;
            }
            pending_roots.push_back(cur.todo[i]);
        }
        chosen_candidates = generate_root_candidates(cur.graph, chosen_root, &ctx, 0,
                                                     &pending_roots, max_depth);
        ++ctx.candidate_batches;
        ctx.candidate_plans += chosen_candidates.size();
        if (chosen_candidates.empty()) {
            ++ctx.dead_end_no_candidates;
            ++ctx.dead_ends_by_depth[chosen_depth];
            ctx.report_progress("no-candidates", chosen_depth, ctx.visited.size(), cur.todo.size(),
                                cur.graph.nodes.size());
            std::cout << "no completion candidates for root " << chosen_root
                      << " at depth " << chosen_depth
                      << " role=" << static_cast<int>(cur.graph.nodes[chosen_root].role)
                      << " degree=" << cur.graph.nodes[chosen_root].degree() << '\n';
            return false;
        }
    }

    ctx.active_root = -1;

    if (chosen_index == -1) {
        return false;
    }

    cur.todo.erase(cur.todo.begin() + chosen_index);

    for (const auto& cand : chosen_candidates) {
        SearchState next = cur;
        next.graph = cand.graph;
        bool ok = true;
        for (const auto& [a, b] : cand.plan.add_pairs) {
            if (next.graph.ensure_split(a, b) == -1) {
                ++ctx.split_apply_failures;
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        if (next.completed.size() < next.graph.nodes.size()) {
            next.completed.resize(next.graph.nodes.size(), false);
            next.depth_seen.resize(next.graph.nodes.size(), -1);
        }
        next.completed[chosen_root] = true;
        bool impossible_pending_root = false;
        for (const auto& [pending_root, pending_depth] : next.todo) {
            if (pending_depth > max_depth) {
                continue;
            }
            if (pending_root < static_cast<int>(next.completed.size()) &&
                next.completed[pending_root]) {
                continue;
            }
            if (!analyze_root_generation(next.graph, pending_root, &ctx).any_feasible_variant) {
                impossible_pending_root = true;
                break;
            }
        }
        if (impossible_pending_root) {
            ++ctx.impossible_root_prunes;
            continue;
        }
        if (!enqueue_neighbors(next, chosen_root, chosen_depth, max_depth)) {
            continue;
        }
        if (expand_with_backtracking(ctx, next, max_depth)) {
            return true;
        }
    }

    return false;
}

Graph build_seed() {
    Graph g;
    int root = g.add_node(Node::Role::center);
    auto maybe_n = g.materialize_neighborhood(root);
    if (!maybe_n) {
        return g;
    }

    Graph::Neighborhood n = *maybe_n;
    std::array<int, 4> branch_order{0, 1, 2, 3};
    std::array<std::array<int, 3>, 4> child_order{{
        {{0, 1, 2}},
        {{0, 1, 2}},
        {{0, 1, 2}},
        {{0, 1, 2}},
    }};
    std::array<int, 12> flat = Graph::flatten(n, branch_order, child_order);

    static constexpr std::array<std::pair<int, int>, 12> kImageLeafLinks{{
        {0, 3}, {3, 6}, {6, 9}, {1, 11}, {1, 9}, {7, 10},
        {4, 10}, {4, 8}, {2, 8}, {2, 5}, {5, 7}, {0, 11},
    }};

    for (const auto& [i, j] : kImageLeafLinks) {
        g.ensure_split(flat[i], flat[j]);
    }
    return g;
}

Graph build_minimal_seed() {
    Graph g;
    int root = g.add_node(Node::Role::center);
    auto maybe_n = g.materialize_neighborhood(root);
    if (!maybe_n) {
        return g;
    }
    return g;
}

Graph build_pair_balanced_seed() {
    Graph g = build_minimal_seed();
    auto maybe_n = g.materialize_neighborhood(0);
    if (!maybe_n) {
        return g;
    }
    Graph::Neighborhood n = *maybe_n;

    while (true) {
        std::array<int, 6> pair_counts = g.rooted_pair_loop_counts(n);
        if (g.pair_balance_satisfied(pair_counts) || !g.pair_balance_feasible(pair_counts)) {
            break;
        }

        bool added = false;
        for (int a = 0; a < 4 && !added; ++a) {
            for (int b = a + 1; b < 4 && !added; ++b) {
                int pair_idx = Graph::branch_pair_index(a, b);
                if (pair_counts[pair_idx] >= 2) {
                    continue;
                }
                for (int i = 0; i < 3 && !added; ++i) {
                    for (int j = 0; j < 3 && !added; ++j) {
                        int lhs = n.children[a][i];
                        int rhs = n.children[b][j];
                        if (g.shared_neighbor(lhs, rhs) != -1) {
                            continue;
                        }
                        if (g.ensure_split(lhs, rhs) != -1) {
                            added = true;
                        }
                    }
                }
            }
        }
        if (!added) {
            break;
        }
    }
    return g;
}

Graph build_pair_balanced_seed_random(std::uint64_t rng_seed) {
    Graph g = build_minimal_seed();
    auto maybe_n = g.materialize_neighborhood(0);
    if (!maybe_n) {
        return g;
    }
    Graph::Neighborhood n = *maybe_n;
    std::mt19937_64 rng(rng_seed);

    for (int step = 0; step < 64; ++step) {
        std::array<int, 6> pair_counts = g.rooted_pair_loop_counts(n);
        if (g.pair_balance_satisfied(pair_counts) || !g.pair_balance_feasible(pair_counts)) {
            break;
        }

        std::vector<std::pair<int, int>> pending_pairs;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                int pair_idx = Graph::branch_pair_index(a, b);
                if (pair_counts[pair_idx] < 2) {
                    pending_pairs.push_back({a, b});
                }
            }
        }
        if (pending_pairs.empty()) {
            break;
        }
        std::shuffle(pending_pairs.begin(), pending_pairs.end(), rng);

        bool added = false;
        for (const auto& [a, b] : pending_pairs) {
            std::vector<std::pair<int, int>> child_pairs;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    int lhs = n.children[a][i];
                    int rhs = n.children[b][j];
                    if (g.shared_neighbor(lhs, rhs) != -1) {
                        continue;
                    }
                    if (!g.can_split_with_new_node(lhs, rhs)) {
                        continue;
                    }
                    child_pairs.push_back({lhs, rhs});
                }
            }
            std::shuffle(child_pairs.begin(), child_pairs.end(), rng);
            for (const auto& [lhs, rhs] : child_pairs) {
                if (g.ensure_split(lhs, rhs) != -1) {
                    added = true;
                    break;
                }
            }
            if (added) {
                break;
            }
        }
        if (!added) {
            break;
        }
    }
    return g;
}

int main() {
    std::cout << std::unitbuf;

    constexpr int kMaxDepth = 2;
    constexpr std::size_t kDefaultCacheReserve = 1'000'000;
    constexpr std::uint64_t kDefaultProgressIntervalStates = 10'000;
    constexpr double kDefaultSeedTimeLimitSeconds = 0.0;
    std::size_t cache_reserve = kDefaultCacheReserve;
    std::uint64_t progress_interval_states = kDefaultProgressIntervalStates;
    double seed_time_limit_seconds = kDefaultSeedTimeLimitSeconds;
    if (const char* reserve_env = std::getenv("HEX_CACHE_RESERVE_STATES")) {
        cache_reserve = std::max<std::size_t>(1, std::strtoull(reserve_env, nullptr, 10));
    }
    if (const char* interval_env = std::getenv("HEX_PROGRESS_INTERVAL_STATES")) {
        progress_interval_states = std::max<std::uint64_t>(1, std::strtoull(interval_env, nullptr, 10));
    }
    if (const char* seed_limit_env = std::getenv("HEX_SEED_TIME_LIMIT_SECONDS")) {
        seed_time_limit_seconds = std::max(0.0, std::strtod(seed_limit_env, nullptr));
    }

    std::cout << "transposition cache reserved for ~" << cache_reserve
              << " states in RAM"
              << ", progress interval=" << progress_interval_states << " states"
              << ", per-seed-time-limit=" << seed_time_limit_seconds << "s\n";

    for (int depth = 0; depth <= kMaxDepth; ++depth) {
        std::vector<std::pair<std::string, Graph>> seed_variants;
        seed_variants.push_back({"image-seed", build_seed()});
        seed_variants.push_back({"minimal-seed", build_minimal_seed()});
        seed_variants.push_back({"pair-balanced-seed", build_pair_balanced_seed()});
        for (std::uint64_t seed = 1; seed <= 6; ++seed) {
            seed_variants.push_back({"pair-balanced-rand-" + std::to_string(seed),
                                     build_pair_balanced_seed_random(0x9e3779b97f4a7c15ULL + seed)});
        }

        bool depth_ok = false;
        for (const auto& [seed_name, seed_graph] : seed_variants) {
            SearchContext ctx;
            ctx.visited.max_load_factor(0.7f);
            ctx.visited.reserve(cache_reserve);
            ctx.progress_interval_states = progress_interval_states;
            ctx.per_seed_time_limit_seconds = seed_time_limit_seconds;
            ctx.target_depth = depth;
            ctx.start_time = std::chrono::steady_clock::now();
            ctx.last_report_time = ctx.start_time;

            SearchState initial;
            initial.graph = seed_graph;
            initial.depth_seen.resize(initial.graph.nodes.size(), -1);
            initial.completed.resize(initial.graph.nodes.size(), false);
            initial.depth_seen[0] = 0;
            initial.todo.push_back({0, 0});

            if (expand_with_backtracking(ctx, initial, depth)) {
                std::cout << "depth " << depth << " solved with " << seed_name << '\n';
                ctx.report_progress("depth-complete", -1, ctx.visited.size(), 0,
                                    initial.graph.nodes.size());
                std::cout << "validated depth " << depth
                          << " with cache size=" << ctx.visited.size() << '\n';
                depth_ok = true;
                break;
            }
            if (ctx.time_limit_hit) {
                std::cout << "seed " << seed_name << " hit time limit at depth " << depth
                          << '\n';
            }
        }

        if (!depth_ok) {
            std::cout << "failed to validate depth " << depth << '\n';
            return 1;
        }
    }
    return 0;
}
