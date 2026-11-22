#ifndef VRP_CORE_TESTS_TEST_GRAPHS_HPP_
#define VRP_CORE_TESTS_TEST_GRAPHS_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "vrp/core/graph.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::test_support {

// Builds a graph from edges, with nodes on a small grid; aborts on invalid
// input, so a test cannot silently run on a broken graph.
RoadGraph MakeGraph(std::size_t node_count, const std::vector<Edge>& edges);

// A random strongly connected graph: a directed cycle through every node,
// plus `extra_edge_count` random edges. Weights are random in [1, 1000],
// chosen independently for length and time.
RoadGraph MakeRandomGraph(std::uint32_t node_count,
                          std::uint32_t extra_edge_count, std::uint32_t seed);

// The cheapest edge from `from` to `to`, or kNoRoute if there is none.
Cost CheapestEdge(const RoadGraph& graph, EdgeCost cost, NodeId from,
                  NodeId to);

// The bytes of the golden file at `path` in docs/formats; aborts if it cannot
// be read, so a test cannot silently run on an empty file.
std::vector<std::byte> ReadGoldenFile(std::string_view path);

// A ParallelFor that splits [0, count) over four threads.
void ThreadedFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body);

}  // namespace vrp::core::test_support

#endif  // VRP_CORE_TESTS_TEST_GRAPHS_HPP_
