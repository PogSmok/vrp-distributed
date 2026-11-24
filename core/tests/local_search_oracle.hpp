#ifndef VRP_CORE_TESTS_LOCAL_SEARCH_ORACLE_HPP_
#define VRP_CORE_TESTS_LOCAL_SEARCH_ORACLE_HPP_

#include <cstddef>
#include <functional>
#include <vector>

#include "test_problems.hpp"
#include "vrp/core/solution.hpp"

// Checks of a local search's results, shared by the GPU search's tests and
// its CPU reference's.
namespace vrp::core::test_support {

// `count` random giant tours, cut by Split.
std::vector<std::vector<Route>> StartingSolutions(const Cvrp& cvrp,
                                                  double penalty,
                                                  std::size_t count);

void ExpectEveryCustomerOnce(const Cvrp& cvrp,
                             const std::vector<Route>& routes);

// Carries out, by rearranging routes, every move the search considers when
// every customer is a neighbour, and expects none to beat `routes`.
void ExpectLocalOptimum(const Cvrp& cvrp, std::vector<Route> routes,
                        double penalty);

// A ParallelFor on four threads, so that the CPU search runs concurrently.
void FourThreadFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end)>& body);

}  // namespace vrp::core::test_support

#endif  // VRP_CORE_TESTS_LOCAL_SEARCH_ORACLE_HPP_
