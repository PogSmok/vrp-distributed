#ifndef VRP_CORE_GPU_ISLAND_HPP_
#define VRP_CORE_GPU_ISLAND_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/local_search.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/population.hpp"
#include "vrp/core/solver/random.hpp"
#include "vrp/core/solver_parameter.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::gpu {

// Defaults are those of HGS-CVRP where it has them.
struct IslandParameters {
  solver::PopulationParameters population;
  std::size_t neighbor_count = 20;  // of the granular local search
  std::size_t batch_size = 8;       // children improved together on the GPU
  std::size_t initial_size = 100;   // random children before crossover starts
  double target_feasible = 0.2;     // share of feasible children aimed for
  std::size_t penalty_interval = 100;  // children between penalty updates
  double repair_probability = 0.5;  // of retrying an infeasible child with 10x
                                    // the penalty
};

// Every field of `parameters` by name, as results record them (ADR 0028).
[[nodiscard]] std::vector<SolverParameter> NamedParameters(
    const IslandParameters& parameters);

// One island of the solver (ADR 0018): an HGS-style memetic search whose
// local search runs on the GPU. The task runner calls Step repeatedly,
// checking for cancellation and exchanging migrants in between (ADR 0004,
// 0024).
class Island {
 public:
  // `matrix` must outlive the island. Fails with kInvalidArgument if
  // ValidateProblem rejects the problem, the matrix has another number of
  // stops, or a parameter is out of range, and with kInternal or kDeviceLost
  // if a CUDA call fails.
  static StatusOr<Island> Create(const DistanceMatrix& matrix,
                                 const Problem& problem,
                                 const IslandParameters& parameters,
                                 std::uint32_t seed);
  // A temporary matrix would be freed while the island still uses it.
  static StatusOr<Island> Create(DistanceMatrix&&, const Problem&,
                                 const IslandParameters&,
                                 std::uint32_t) = delete;

  // One batch of children: random tours until the initial population is
  // complete, then crossovers of parents, each split, improved and added to
  // the population. Fails with kInternal or kDeviceLost if a CUDA call fails.
  Status Step();

  // Adds a solution from another island (ADR 0004). Fails with
  // kInvalidArgument unless ValidateSolution accepts it.
  Status Import(const Solution& migrant);

  // The best feasible solution found so far, or nothing: only feasible
  // solutions are reported or exported (ADR 0018).
  [[nodiscard]] const std::optional<solver::Individual>& Best() const {
    return best_;
  }

 private:
  Island(const DistanceMatrix& matrix, const Problem& problem,
         const IslandParameters& parameters, std::uint32_t seed,
         LocalSearch search, double penalty);

  // Adds a child to the population, and keeps it if it is the best.
  void Keep(std::vector<Route> routes, const solver::Evaluation& evaluation);
  void UpdatePenalty();

  const DistanceMatrix* matrix_;
  Problem problem_;
  IslandParameters parameters_;
  solver::Random random_;
  LocalSearch search_;
  double penalty_;
  solver::Population population_;
  std::size_t children_ = 0;
  std::size_t recent_children_ = 0;
  std::size_t recent_feasible_ = 0;
  std::optional<solver::Individual> best_;
};

}  // namespace vrp::core::gpu

#endif  // VRP_CORE_GPU_ISLAND_HPP_
