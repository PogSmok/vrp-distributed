#ifndef VRP_CORE_LEG_ROUTER_HPP_
#define VRP_CORE_LEG_ROUTER_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <span>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core {

// A shortest road path between two nodes (ADR 0025).
struct Leg {
  Cost cost = 0;
  // The nodes from start to end, both included.
  std::vector<NodeId> path;
};

struct LegQuery {
  NodeId from = 0;
  NodeId to = 0;
};

// Point-to-point shortest paths by bidirectional Dijkstra, the production
// router for route geometry in FINALIZE (ADR 0025). The workspace is reused
// between queries, so a query costs time proportional to the part of the
// graph it explores, not to the graph's size. Not thread-safe: use one
// LegRouter per thread.
class LegRouter {
 public:
  // The graph must outlive the router.
  LegRouter(const RoadGraph& graph, EdgeCost cost);

  // Fails with kInvalidArgument if a node does not exist, kFailedPrecondition
  // if there is no path, and kOutOfRange if the cost does not fit in Cost.
  StatusOr<Leg> Route(NodeId from, NodeId to);

 private:
  struct QueueEntry {
    std::uint64_t cost = 0;
    NodeId node = 0;
  };
  struct LaterFirst {
    bool operator()(const QueueEntry& a, const QueueEntry& b) const {
      return a.cost > b.cost;
    }
  };
  // One direction of the search.
  struct Search {
    const Adjacency* adjacency = nullptr;
    std::vector<std::uint64_t> costs;
    std::vector<NodeId> parents;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, LaterFirst> queue;
  };

  void Reset();
  void Start(Search& search, NodeId node);
  // Settles the closest queued node of `search` and relaxes its edges,
  // updating the best meeting point with `other`.
  void Expand(Search& search, const Search& other);
  [[nodiscard]] std::vector<NodeId> BuildPath() const;

  const RoadGraph* graph_;
  EdgeCost cost_;
  Search forward_;
  Search backward_;
  // Nodes whose entries must be reset before the next query.
  std::vector<NodeId> touched_;
  std::uint64_t best_cost_ = 0;
  NodeId meeting_node_ = 0;
};

// Runs body(begin, end) over sub-ranges that together cover [0, count)
// exactly once, possibly concurrently. Supplied by the caller (ADR 0023), so
// that vrp-core starts no threads of its own.
using ParallelFor = std::function<void(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body)>;

// A ParallelFor that calls body(0, count) on the calling thread.
void SequentialFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body);

// Routes every leg; element i of the result answers legs[i]. Each sub-range
// gets its own LegRouter, which allocates 24 bytes per graph node, so
// `parallel_for` should make one sub-range per thread, not many small ones.
// `stop_requested` is checked before each leg
// (ADR 0024), possibly from several threads at once. Fails with the error of
// the lowest-numbered failing leg, kCancelled if `stop_requested` returned
// true, or with kInternal if parallel_for skipped a leg.
StatusOr<std::vector<Leg>> RouteLegs(
    const RoadGraph& graph, EdgeCost cost, std::span<const LegQuery> legs,
    const ParallelFor& parallel_for,
    const std::function<bool()>& stop_requested);

}  // namespace vrp::core

#endif  // VRP_CORE_LEG_ROUTER_HPP_
