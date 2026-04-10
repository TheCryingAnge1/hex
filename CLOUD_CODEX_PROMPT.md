Goal

Find or synthesize an infinite or indefinitely extensible graph family consistent
with these constraints:

1. Every node has degree exactly 4.
2. The graph has girth at least 6.
3. For any chosen node treated as the current root, there are exactly 12 distinct
   cycles of length 6 visible from that root.

Seed / context

- The repository contains a C++ search prototype in `main.cpp`.
- `pattern.doc` describes the original hand-drawn seed pattern and the later
  typed-expansion interpretation.
- The original seed is a 17-node rooted shape plus 12 split nodes from the
  drawing.
- A deeper hand sketch suggests that shifted roots are not all governed by the
  same local completion rule.

What has already been tried

1. A fixed one-shot constructor for the original image pattern.
2. A greedy completion pass that reused existing local structure after a root
   shift and filled missing local loops.
3. A typed expansion approach with separate center / branch / leaf / split /
   frontier roles.
4. A semi-bruteforce local search that:
   - materializes a local neighborhood around a root,
   - enumerates branch / child orderings,
   - tries multiple split-node pairings,
   - backtracks over local completion choices.

Current state / failure

- The current code builds and runs.
- It validates the seed at depth 0.
- It still fails by depth 1 under the present search and neighborhood model.
- The main issue appears to be that earlier local pairings can sabotage later
  shifted-root completions, and the current neighborhood abstraction is still too
  rigid.

Requested direction

Do not assume the exact hand-drawn deeper pattern must be copied literally.
Use it only as a guide.

Preferred approach:

1. Reframe the task as a constrained graph search / CSP / SAT-like problem.
2. Allow reevaluation of earlier local choices when later roots become blocked.
3. Search for a repeatable local rule or repeating motif that actually supports
   indefinite growth.
4. If the stated constraints are inconsistent, produce a concrete argument or a
   smallest counterexample / search failure explanation.

Expected deliverables

1. A clear statement whether a depth-7 model satisfying all three constraints was
   found.
2. If found:
   - the construction rule,
   - code implementing it,
   - a validator.
3. If not found:
   - the strongest partial result,
   - where the search fails,
   - whether the constraints appear incompatible.
