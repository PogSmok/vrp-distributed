#ifndef VRP_CORE_SOLVER_POPULATION_HPP_
#define VRP_CORE_SOLVER_POPULATION_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {

// A solution as the solver keeps it: its routes, and the giant tour that
// concatenates them (ADR 0018).
struct Individual {
  std::vector<Route> routes;
  std::vector<std::uint32_t> tour;
  Evaluation evaluation;
};

// An individual for `routes`, whose evaluation is `evaluation`.
[[nodiscard]] Individual MakeIndividual(std::vector<Route> routes,
                                        Evaluation evaluation);

struct PopulationParameters {
  std::size_t min_size = 25;         // mu: survivors of each selection
  std::size_t generation_size = 40;  // lambda: children before a selection
  std::size_t elite_count = 4;       // ranked by cost alone
  std::size_t close_count = 5;       // neighbours that measure diversity
};

// The population of HGS-CVRP (Vidal 2022, in core/README.md's References):
// feasible and infeasible solutions apart, each ranked by a biased fitness
// that adds diversity, the broken-pairs distance to its closest neighbours,
// to cost.
class Population {
 public:
  // `customer_count` is the number of customers of the problem.
  // Precondition: `parameters.min_size` is at least 1.
  Population(std::size_t customer_count, const PopulationParameters& parameters,
             double penalty);
  Population(Population&&) noexcept;
  Population& operator=(Population&&) noexcept;
  ~Population();

  // Adds `individual`; a subpopulation that reaches min_size +
  // generation_size keeps its min_size best by biased fitness, clones
  // first to go.
  void Add(Individual individual);

  // A binary tournament on biased fitness over both subpopulations.
  // Precondition: the population is not empty.
  [[nodiscard]] const Individual& Select(Random& random);

  // Changes the penalty, re-ranking the infeasible solutions.
  void SetPenalty(double penalty);

  [[nodiscard]] std::size_t Size() const;

 private:
  struct Member;
  using Subpopulation = std::vector<std::unique_ptr<Member>>;

  [[nodiscard]] double Distance(const Member& a, const Member& b) const;
  void SortByCost(Subpopulation& subpopulation) const;
  void UpdateFitness(Subpopulation& subpopulation) const;
  void RemoveWorst(Subpopulation& subpopulation);

  std::size_t customer_count_;
  PopulationParameters parameters_;
  double penalty_;
  Subpopulation feasible_;
  Subpopulation infeasible_;
};

}  // namespace vrp::core::solver

#endif  // VRP_CORE_SOLVER_POPULATION_HPP_
