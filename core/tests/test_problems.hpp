#ifndef VRP_CORE_TESTS_TEST_PROBLEMS_HPP_
#define VRP_CORE_TESTS_TEST_PROBLEMS_HPP_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/solution.hpp"

namespace vrp::core::test_support {

struct Cvrp {
  DistanceMatrix matrix;
  Problem problem;
};

// A random CVRP: stops in a square, costs their rounded distances made up to
// 20% asymmetric, demands from 1 to 10, capacity 30, and vehicles for about
// twice the total demand.
Cvrp MakeRandomCvrp(std::size_t customer_count, std::uint32_t seed);

// A random giant tour of the problem's customers.
std::vector<std::uint32_t> MakeRandomTour(const Problem& problem,
                                          std::uint32_t seed);

}  // namespace vrp::core::test_support

#endif  // VRP_CORE_TESTS_TEST_PROBLEMS_HPP_
