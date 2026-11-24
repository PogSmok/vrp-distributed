#include "solver/local_search_moves.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::solver {
namespace {

// Inserts `chain` after `v`, or at the start if `v` is the depot.
void InsertAfter(Route& route, std::uint32_t v,
                 std::initializer_list<std::uint32_t> chain) {
  const auto at = v == 0 ? route.begin() : std::ranges::find(route, v) + 1;
  route.insert(at, chain);
}

// Applies `move` of customer `u`, at position `pu` of route `ru`. Positions
// are those of the round that found the move.
void ApplyMove(std::vector<Route>& routes, std::uint32_t u, std::uint32_t ru,
               std::uint32_t pu, const Move& move) {
  Route& from = routes[ru];
  Route& to = routes[move.route];
  const auto at_u = from.begin() + static_cast<std::ptrdiff_t>(pu);
  const std::uint32_t v =
      move.position < 0 ? 0 : to[static_cast<std::size_t>(move.position)];
  switch (move.type) {
    case MoveType::kNone:
      return;
    case MoveType::kRelocate:
      from.erase(at_u);
      InsertAfter(to, v, {u});
      return;
    case MoveType::kRelocatePair:
    case MoveType::kRelocatePairReversed: {
      const std::uint32_t x = *(at_u + 1);
      from.erase(at_u, at_u + 2);
      if (move.type == MoveType::kRelocatePair) {
        InsertAfter(to, v, {u, x});
      } else {
        InsertAfter(to, v, {x, u});
      }
      return;
    }
    case MoveType::kSwap:
      std::swap(*at_u, to[static_cast<std::size_t>(move.position)]);
      return;
    case MoveType::kTwoOpt:
      std::reverse(at_u + 1, from.begin() + (move.position + 1));
      return;
    case MoveType::kTwoOptStar: {
      // Parenthesised: the depot's position is -1, before the route's start.
      const auto after_v = to.begin() + (move.position + 1);
      Route new_from(from.begin(), at_u + 1);
      new_from.insert(new_from.end(), after_v, to.end());
      Route new_to(to.begin(), after_v);
      new_to.insert(new_to.end(), at_u + 1, from.end());
      from = std::move(new_from);
      to = std::move(new_to);
      return;
    }
  }
}

// Applies the improving moves of one solution, the best first, skipping
// those that share a route with one already applied: moves on disjoint
// routes change disjoint parts of the cost, so their gains add up. `moves`,
// `route_of` and `position` are indexed by customer. Returns whether any
// move was applied.
bool ApplyBestMoves(std::span<const Move> moves,
                    std::span<const std::uint32_t> route_of,
                    std::span<const std::uint32_t> position,
                    std::vector<Route>& routes, Evaluation& evaluation) {
  std::vector<std::uint32_t> candidates;
  for (std::uint32_t u = 1; u < moves.size(); ++u) {
    if (moves[u].type != MoveType::kNone) {
      candidates.push_back(u);
    }
  }
  std::ranges::sort(candidates, [&](std::uint32_t a, std::uint32_t b) {
    return std::pair(moves[a].gain, a) < std::pair(moves[b].gain, b);
  });

  std::vector<bool> used(routes.size(), false);
  bool applied = false;
  for (const std::uint32_t u : candidates) {
    const Move& move = moves[u];
    const std::uint32_t ru = route_of[u];
    if (used[ru] || used[move.route]) {
      continue;
    }
    used[ru] = true;
    used[move.route] = true;
    ApplyMove(routes, u, ru, position[u], move);
    evaluation.distance = static_cast<RouteCost>(
        static_cast<std::int64_t>(evaluation.distance) + move.distance);
    evaluation.excess = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(evaluation.excess) + move.excess);
    applied = true;
  }
  return applied;
}

void Stage(const DistanceMatrix& matrix, const Problem& problem,
           std::span<const std::vector<Route>> solutions,
           SearchStaging& staging) {
  const std::size_t stops = matrix.StopCount();
  const std::size_t customers = stops - 1;
  const std::size_t route_count = problem.vehicle_count;
  for (std::size_t solution = 0; solution < solutions.size(); ++solution) {
    const std::size_t stop_base = solution * stops;
    const std::size_t node_base = solution * customers;
    const std::size_t route_base = solution * route_count;
    std::uint32_t next = 0;
    staging.empty_route[solution] = static_cast<std::uint32_t>(route_count);
    for (std::size_t route = 0; route < route_count; ++route) {
      const Route& nodes = solutions[solution][route];
      staging.route_begin[route_base + route] = next;
      staging.route_size[route_base + route] =
          static_cast<std::uint32_t>(nodes.size());
      if (nodes.empty() && staging.empty_route[solution] == route_count) {
        staging.empty_route[solution] = static_cast<std::uint32_t>(route);
      }
      std::int64_t forward = 0;
      std::int64_t reverse = 0;
      std::int64_t load = 0;
      std::uint32_t previous = 0;
      for (std::size_t position = 0; position < nodes.size(); ++position) {
        const std::uint32_t node = nodes[position];
        staging.route_of[stop_base + node] = static_cast<std::uint32_t>(route);
        staging.position[stop_base + node] =
            static_cast<std::uint32_t>(position);
        forward += matrix.At(previous, node);
        if (position > 0) {
          reverse += matrix.At(node, previous);
        }
        load += problem.demands[node];
        const std::size_t index = node_base + next++;
        staging.nodes[index] = node;
        staging.forward[index] = forward;
        staging.reverse[index] = reverse;
        staging.prefix_load[index] = load;
        previous = node;
      }
      staging.route_distance[route_base + route] =
          nodes.empty() ? 0 : forward + matrix.At(previous, 0);
      staging.route_load[route_base + route] = load;
    }
  }
}

}  // namespace

std::vector<std::uint32_t> NearestNeighbors(const DistanceMatrix& matrix,
                                            std::size_t count) {
  const std::size_t stops = matrix.StopCount();
  const std::size_t customers = stops - 1;
  std::vector<std::uint32_t> neighbors(stops * count);
  std::vector<std::uint32_t> others;
  for (std::uint32_t u = 1; u <= customers; ++u) {
    others.clear();
    for (std::uint32_t v = 1; v <= customers; ++v) {
      if (v != u) {
        others.push_back(v);
      }
    }
    const auto closeness = [&](std::uint32_t v) {
      return std::min(matrix.At(u, v), matrix.At(v, u));
    };
    std::ranges::partial_sort(
        others, others.begin() + static_cast<std::ptrdiff_t>(count),
        [&](std::uint32_t a, std::uint32_t b) {
          return std::pair(closeness(a), a) < std::pair(closeness(b), b);
        });
    std::ranges::copy_n(
        others.begin(), static_cast<std::ptrdiff_t>(count),
        neighbors.begin() + static_cast<std::ptrdiff_t>(u * count));
  }
  return neighbors;
}

SearchStaging::SearchStaging(std::size_t batch, std::size_t stops,
                             std::size_t customers, std::size_t routes)
    : route_of(batch * stops),
      position(batch * stops),
      nodes(batch * customers),
      forward(batch * customers),
      reverse(batch * customers),
      prefix_load(batch * customers),
      route_begin(batch * routes),
      route_size(batch * routes),
      route_distance(batch * routes),
      route_load(batch * routes),
      empty_route(batch) {}

StatusOr<std::vector<Evaluation>> ImproveInRounds(
    const DistanceMatrix& matrix, const Problem& problem, std::size_t max_batch,
    std::span<std::vector<Route>> solutions, const FindMoves& find_moves) {
  if (solutions.size() > max_batch) {
    return InvalidArgumentError("the batch holds at most " +
                                std::to_string(max_batch) + " solutions");
  }
  const std::size_t batch = solutions.size();
  const std::size_t stops = matrix.StopCount();
  const std::size_t customers = stops - 1;
  const std::size_t route_count = problem.vehicle_count;

  // Every solution gets `route_count` routes, the unused ones empty.
  std::vector<Evaluation> evaluations(batch);
  for (std::size_t solution = 0; solution < batch; ++solution) {
    if (solutions[solution].size() > route_count) {
      return InvalidArgumentError("solution " + std::to_string(solution) +
                                  " has more routes than vehicles");
    }
    solutions[solution].resize(route_count);
    evaluations[solution] = Evaluate(matrix, problem, solutions[solution]);
  }

  SearchStaging staging(batch, stops, customers, route_count);
  std::vector<Move> moves(batch * stops);
  for (bool improved = batch * customers > 0; improved;) {
    Stage(matrix, problem, solutions, staging);
    VRP_RETURN_IF_ERROR(find_moves(staging, moves));
    improved = false;
    for (std::size_t solution = 0; solution < batch; ++solution) {
      const std::size_t base = solution * stops;
      if (ApplyBestMoves(std::span(moves).subspan(base, stops),
                         std::span(staging.route_of).subspan(base, stops),
                         std::span(staging.position).subspan(base, stops),
                         solutions[solution], evaluations[solution])) {
        improved = true;
      }
    }
  }

  for (std::vector<Route>& routes : solutions) {
    std::erase_if(routes, [](const Route& route) { return route.empty(); });
  }
  return evaluations;
}

}  // namespace vrp::core::solver
