#include "vrp/core/gpu/island.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include "vrp/core/distance_matrix.hpp"
#include "vrp/core/gpu/local_search.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/solver/crossover.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/population.hpp"
#include "vrp/core/solver/split.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/status_macros.hpp"
#include "vrp/core/types.hpp"

namespace vrp::core::gpu {
namespace {

// Penalty bounds and factors of HGS-CVRP.
constexpr double kMinPenalty = 0.1;
constexpr double kMaxPenalty = 100'000.0;
constexpr double kMaxInitialPenalty = 1'000.0;
constexpr double kPenaltyIncrease = 1.2;
constexpr double kPenaltyDecrease = 0.85;
constexpr double kTargetTolerance = 0.05;
constexpr double kRepairPenaltyFactor = 10.0;

// HGS-CVRP's starting penalty: the largest cost over the largest demand.
double InitialPenalty(const DistanceMatrix& matrix, const Problem& problem) {
  const Cost largest_cost =
      matrix.Data().empty() ? 0 : std::ranges::max(matrix.Data());
  const std::uint32_t largest_demand =
      std::max(1U, std::ranges::max(problem.demands));
  return std::clamp(static_cast<double>(largest_cost) / largest_demand,
                    kMinPenalty, kMaxInitialPenalty);
}

}  // namespace

StatusOr<Island> Island::Create(const DistanceMatrix& matrix,
                                const Problem& problem,
                                const IslandParameters& parameters,
                                std::uint32_t seed) {
  VRP_RETURN_IF_ERROR(ValidateProblem(problem));
  if (parameters.population.min_size == 0 || parameters.batch_size == 0 ||
      parameters.initial_size == 0 || parameters.penalty_interval == 0) {
    return InvalidArgumentError(
        "the population, batch, initial and penalty interval sizes must be "
        "positive");
  }
  VRP_ASSIGN_OR_RETURN(
      LocalSearch search,
      LocalSearch::Create(matrix, problem, parameters.neighbor_count,
                          parameters.batch_size));
  return Island(matrix, problem, parameters, seed, std::move(search),
                InitialPenalty(matrix, problem));
}

Island::Island(const DistanceMatrix& matrix, const Problem& problem,
               const IslandParameters& parameters, std::uint32_t seed,
               LocalSearch search, double penalty)
    : matrix_(&matrix),
      problem_(problem),
      parameters_(parameters),
      random_(seed),
      search_(std::move(search)),
      penalty_(penalty),
      population_(problem.demands.size() - 1, parameters.population, penalty) {}

Status Island::Step() {
  const std::size_t batch = parameters_.batch_size;
  std::vector<std::vector<Route>> children(batch);
  for (std::size_t index = 0; index < batch; ++index) {
    std::vector<std::uint32_t> tour;
    if (children_ + index < parameters_.initial_size) {
      tour.resize(problem_.demands.size() - 1);
      std::iota(tour.begin(), tour.end(), 1U);
      random_.Shuffle(std::span(tour));
    } else {
      const solver::Individual& first = population_.Select(random_);
      const solver::Individual& second = population_.Select(random_);
      tour = solver::OrderCrossover(first.tour, second.tour, random_);
    }
    children[index] = solver::Split(*matrix_, problem_, tour, penalty_);
  }
  VRP_ASSIGN_OR_RETURN(const std::vector<solver::Evaluation> evaluations,
                       search_.Improve(children, penalty_));

  // Some infeasible children get a second search with a higher penalty.
  std::vector<std::vector<Route>> repairs;
  for (std::size_t index = 0; index < batch; ++index) {
    ++recent_children_;
    if (evaluations[index].Feasible()) {
      ++recent_feasible_;
    } else if (random_.Chance(parameters_.repair_probability)) {
      repairs.push_back(children[index]);
    }
    Keep(std::move(children[index]), evaluations[index]);
  }
  VRP_ASSIGN_OR_RETURN(
      const std::vector<solver::Evaluation> repaired,
      search_.Improve(repairs, penalty_ * kRepairPenaltyFactor));
  for (std::size_t index = 0; index < repairs.size(); ++index) {
    if (repaired[index].Feasible()) {
      Keep(std::move(repairs[index]), repaired[index]);
    }
  }

  children_ += batch;
  if (recent_children_ >= parameters_.penalty_interval) {
    UpdatePenalty();
  }
  return {};
}

Status Island::Import(const Solution& migrant) {
  VRP_RETURN_IF_ERROR(ValidateSolution(problem_, migrant));
  Keep(migrant.routes, solver::Evaluate(*matrix_, problem_, migrant.routes));
  return {};
}

void Island::Keep(std::vector<Route> routes,
                  const solver::Evaluation& evaluation) {
  solver::Individual individual =
      solver::MakeIndividual(std::move(routes), evaluation);
  if (evaluation.Feasible() &&
      (!best_.has_value() ||
       evaluation.distance < best_->evaluation.distance)) {
    best_ = individual;
  }
  population_.Add(std::move(individual));
}

// Moves the share of feasible children towards the target.
void Island::UpdatePenalty() {
  const double share = static_cast<double>(recent_feasible_) /
                       static_cast<double>(recent_children_);
  if (share < parameters_.target_feasible - kTargetTolerance) {
    penalty_ = std::min(penalty_ * kPenaltyIncrease, kMaxPenalty);
  } else if (share > parameters_.target_feasible + kTargetTolerance) {
    penalty_ = std::max(penalty_ * kPenaltyDecrease, kMinPenalty);
  }
  population_.SetPenalty(penalty_);
  recent_children_ = 0;
  recent_feasible_ = 0;
}

}  // namespace vrp::core::gpu
