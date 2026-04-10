#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <iostream>
#include <optional>
#include <queue>
#include <set>
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

    std::optional<Neighborhood> materialize_leaf_neighborhood(int root) {
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

        n.branches[0] = branch_side.front();

        std::array<int, 2> split_parent{-1, -1};
        for (int i = 0; i < 2; ++i) {
            int split = split_side[i];
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
        n.branches[3] = outward_side.front();

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

    int rooted_loop_count(const Neighborhood& n) const {
        int loops = 0;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        if (shared_neighbor(n.children[a][i], n.children[b][j]) != -1) {
                            ++loops;
                        }
                    }
                }
            }
        }
        return loops;
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

    std::vector<CompletionPlan> generate_completion_candidates(int root, int max_results) const {

        Graph base = *this;
        auto maybe_n = base.materialize_neighborhood(root);
        if (!maybe_n) {
            return {};
        }
        const Neighborhood& n = *maybe_n;

        if (base.rooted_loop_count(root) >= 12) {
            return {CompletionPlan{}};
        }

        std::optional<CompletionPlan> greedy_plan;
        {
            Graph greedy = base;
            Neighborhood greedy_n = n;
            std::vector<std::pair<int, int>> chosen;

            while (greedy.rooted_loop_count(root) < 12) {
                int best_a = -1;
                int best_b = -1;
                int best_score = -1;

                for (int a = 0; a < 4; ++a) {
                    for (int b = a + 1; b < 4; ++b) {
                        for (int i = 0; i < 3; ++i) {
                            for (int j = 0; j < 3; ++j) {
                                int lhs = greedy_n.children[a][i];
                                int rhs = greedy_n.children[b][j];
                                int participation_cap =
                                    (nodes[root].role == Node::Role::leaf) ? 3 : 2;
                                if (greedy.rooted_participation_count(greedy_n, lhs) >=
                                        participation_cap ||
                                    greedy.rooted_participation_count(greedy_n, rhs) >=
                                        participation_cap) {
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

            if (greedy.rooted_loop_count(root) >= 12) {
                std::sort(chosen.begin(), chosen.end());
                chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
                greedy_plan = CompletionPlan{
                    .add_pairs = chosen,
                    .reused_pairs = greedy.rooted_loop_count(root) - static_cast<int>(chosen.size()),
                    .new_nodes = static_cast<int>(chosen.size()),
                };
            }
        }

        std::set<std::vector<std::pair<int, int>>> seen;
        std::vector<CompletionPlan> out;
        if (greedy_plan) {
            out.push_back(*greedy_plan);
            seen.insert(greedy_plan->add_pairs);
        }

        auto search = [&](auto&& self, const Graph& cur_graph,
                          const Neighborhood& cur_n,
                          std::vector<std::pair<int, int>>& chosen) -> void {
            if (static_cast<int>(out.size()) >= max_results) {
                return;
            }

            int loops = cur_graph.rooted_loop_count(root);
            if (loops >= 12) {
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

            struct CandidateEdge {
                int a;
                int b;
                int score;
            };

            std::vector<CandidateEdge> edges;
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            int lhs = cur_n.children[a][i];
                            int rhs = cur_n.children[b][j];
                            int participation_cap =
                                (nodes[root].role == Node::Role::leaf) ? 3 : 2;
                            if (cur_graph.rooted_participation_count(cur_n, lhs) >=
                                    participation_cap ||
                                cur_graph.rooted_participation_count(cur_n, rhs) >=
                                    participation_cap) {
                                continue;
                            }
                            if (cur_graph.shared_neighbor(lhs, rhs) != -1 ||
                                !cur_graph.can_split_with_new_node(lhs, rhs)) {
                                continue;
                            }
                            edges.push_back(CandidateEdge{
                                .a = std::min(lhs, rhs),
                                .b = std::max(lhs, rhs),
                                .score = 10 * (cur_graph.nodes[lhs].free_slots() +
                                               cur_graph.nodes[rhs].free_slots()) -
                                         (cur_graph.nodes[lhs].degree() +
                                          cur_graph.nodes[rhs].degree()),
                            });
                        }
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

            int remaining_needed = 12 - loops;
            if (static_cast<int>(edges.size()) < remaining_needed) {
                return;
            }

            static constexpr int kLocalBeam = 6;
            int explored = 0;
            for (const auto& edge : edges) {
                if (explored++ >= kLocalBeam) {
                    break;
                }
                Graph next_graph = cur_graph;
                if (next_graph.ensure_split(edge.a, edge.b) == -1) {
                    continue;
                }
                chosen.push_back({edge.a, edge.b});
                self(self, next_graph, cur_n, chosen);
                chosen.pop_back();
                if (static_cast<int>(out.size()) >= max_results) {
                    return;
                }
            }
        };

        std::vector<std::pair<int, int>> chosen;
        search(search, base, n, chosen);

        std::sort(out.begin(), out.end(), [](const CompletionPlan& lhs,
                                             const CompletionPlan& rhs) {
            if (lhs.reused_pairs != rhs.reused_pairs) {
                return lhs.reused_pairs > rhs.reused_pairs;
            }
            return lhs.new_nodes < rhs.new_nodes;
        });
        if (static_cast<int>(out.size()) > max_results) {
            out.resize(max_results);
        }
        return out;
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

        int loops = 0;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                for (int i : children[a]) {
                    for (int j : children[b]) {
                        if (shared_neighbor(i, j) != -1) {
                            ++loops;
                        }
                    }
                }
            }
        }
        return loops;
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
                if (loops != 12) {
                    std::cout << "loop failure at node " << node.id
                              << ": loops=" << loops << '\n';
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

bool expand_with_backtracking(const SearchState& state, int max_depth, int branching_cap) {
    SearchState cur = state;
    while (!cur.todo.empty() && cur.completed[cur.todo.front().first]) {
        cur.todo.pop_front();
    }

    if (cur.todo.empty()) {
        return cur.graph.validate_depth(0, max_depth);
    }

    int chosen_index = -1;
    int chosen_root = -1;
    int chosen_depth = -1;
    Graph chosen_graph;
    std::vector<Graph::CompletionPlan> chosen_candidates;

    for (int i = 0; i < static_cast<int>(cur.todo.size()); ++i) {
        auto [root, depth] = cur.todo[i];
        Graph probe = cur.graph;
        if (!probe.materialize_neighborhood(root)) {
            std::cout << "failed to materialize neighborhood for root " << root
                      << " at depth " << depth << '\n';
            return false;
        }

        std::vector<Graph::CompletionPlan> candidates =
            probe.generate_completion_candidates(root, branching_cap);
        if (candidates.empty()) {
            std::cout << "no completion candidates for root " << root
                      << " at depth " << depth
                      << " role=" << static_cast<int>(probe.nodes[root].role)
                      << " degree=" << probe.nodes[root].degree() << '\n';
            return false;
        }

        if (chosen_index == -1 ||
            static_cast<int>(candidates.size()) < static_cast<int>(chosen_candidates.size())) {
            chosen_index = i;
            chosen_root = root;
            chosen_depth = depth;
            chosen_graph = std::move(probe);
            chosen_candidates = std::move(candidates);
            if (chosen_candidates.size() == 1) {
                break;
            }
        }
    }

    if (chosen_index == -1) {
        return false;
    }

    cur.todo.erase(cur.todo.begin() + chosen_index);

    for (const auto& cand : chosen_candidates) {
        SearchState next = cur;
        next.graph = chosen_graph;
        bool ok = true;
        for (const auto& [a, b] : cand.add_pairs) {
            if (next.graph.ensure_split(a, b) == -1) {
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
        if (!enqueue_neighbors(next, chosen_root, chosen_depth, max_depth)) {
            continue;
        }
        if (expand_with_backtracking(next, max_depth, branching_cap)) {
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

int main() {
    constexpr int kBranchingCap = 8;
    constexpr int kMaxDepth = 4;

    for (int depth = 0; depth <= kMaxDepth; ++depth) {
        SearchState initial;
        initial.graph = build_seed();
        initial.depth_seen.resize(initial.graph.nodes.size(), -1);
        initial.completed.resize(initial.graph.nodes.size(), false);
        initial.depth_seen[0] = 0;
        initial.todo.push_back({0, 0});

        if (!expand_with_backtracking(initial, depth, kBranchingCap)) {
            std::cout << "failed to validate depth " << depth << '\n';
            return 1;
        }
        std::cout << "validated depth " << depth << '\n';
    }
    return 0;
}
