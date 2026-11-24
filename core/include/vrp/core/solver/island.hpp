#ifndef VRP_CORE_SOLVER_ISLAND_HPP_
#define VRP_CORE_SOLVER_ISLAND_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/leg_router.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/population.hpp"
#include "vrp/core/solver/random.hpp"
#include "vrp/core/solver_parameter.hpp"
#include "vrp/core/status.hpp"

namespace vrp::core::solver {

// Defaults are those of HGS-CVRP where it has them.
struct IslandParameters {
  PopulationParameters population;
  std::size_t neighbor_count = 20;  // of the granular local search
  std::size_t batch_size = 8;       // children improved together
  std::size_t initial_size = 100;   // random children before crossover starts
  double target_feasible = 0.2;     // share of feasible children aimed for
  std::size_t penalty_interval = 100;  // children between penalty updates
  double repair_probability = 0.5;  // of retrying an infeasible child with 10x
                                    // the penalty
};

// Every field of `parameters` by name, as results record them (ADR 0028).
[[nodiscard]] std::vector<SolverParameter> NamedParameters(
    const IslandParameters& parameters);

// Improves a batch of children in place, as LocalSearch::Improve does.
using ImproveBatch = std::function<StatusOr<std::vector<Evaluation>>(
    std::span<std::vector<Route>> solutions, double penalty)>;

// One island of the solver (ADR 0018): an HGS-style memetic search whose
// local search runs on the GPU (gpu::CreateIsland) or the CPU. The task
// runner calls Step repeatedly, checking for cancellation and exchanging
// migrants in between (ADR 0004, 0024).
class Island {
 public:
  // With `improve` as its local search. `matrix` must outlive the island.
  // Fails with kInvalidArgument if ValidateProblem rejects the problem, the
  // matrix has another number of stops, or a parameter is out of range.
  static StatusOr<Island> Create(const DistanceMatrix& matrix,
                                 const Problem& problem,
                                 const IslandParameters& parameters,
                                 std::uint32_t seed, ImproveBatch improve);
  static StatusOr<Island> Create(DistanceMatrix&&, const Problem&,
                                 const IslandParameters&, std::uint32_t,
                                 ImproveBatch) = delete;

  // With reference::LocalSearch on `parallel_for`'s threads. With the same
  // seed, it finds the same solutions step by step as on the GPU.
  static StatusOr<Island> CreateOnCpu(const DistanceMatrix& matrix,
                                      const Problem& problem,
                                      const IslandParameters& parameters,
                                      std::uint32_t seed,
                                      ParallelFor parallel_for);
  static StatusOr<Island> CreateOnCpu(DistanceMatrix&&, const Problem&,
                                      const IslandParameters&, std::uint32_t,
                                      ParallelFor) = delete;

  // One batch of children: random tours until the initial population is
  // complete, then crossovers of parents, each split, improved and added to
  // the population. Fails as its local search does.
  Status Step();

  // Adds a solution from another island (ADR 0004). Fails with
  // kInvalidArgument unless ValidateSolution accepts it.
  Status Import(const Solution& migrant);

  // The best feasible solution found so far, or nothing: only feasible
  // solutions are reported or exported (ADR 0018).
  [[nodiscard]] const std::optional<Individual>& Best() const { return best_; }

 private:
  Island(const DistanceMatrix& matrix, const Problem& problem,
         const IslandParameters& parameters, std::uint32_t seed,
         ImproveBatch improve);

  // Adds a child to the population, and keeps it if it is the best.
  void Keep(std::vector<Route> routes, const Evaluation& evaluation);
  void UpdatePenalty();

  const DistanceMatrix* matrix_;
  Problem problem_;
  IslandParameters parameters_;
  Random random_;
  ImproveBatch improve_;
  double penalty_;
  Population population_;
  std::size_t children_ = 0;
  std::size_t recent_children_ = 0;
  std::size_t recent_feasible_ = 0;
  std::optional<Individual> best_;
};

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_ISLAND_HPP_
