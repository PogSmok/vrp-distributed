#include "vrp/core/solver/population.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "vrp/core/solution.hpp"
#include "vrp/core/solver/evaluation.hpp"
#include "vrp/core/solver/random.hpp"

namespace vrp::core::solver {
namespace {

// Distances below this make two solutions clones, as in HGS-CVRP.
constexpr double kEpsilon = 1e-5;

}  // namespace

struct Population::Member {
  Individual individual;
  // The stop after and before each customer; 0 is the depot.
  std::vector<std::uint32_t> successor;
  std::vector<std::uint32_t> predecessor;
  // Distances to the other members of the subpopulation, closest first.
  std::vector<std::pair<double, const Member*>> neighbors;
  double fitness = 0.0;
};

Individual MakeIndividual(std::vector<Route> routes, Evaluation evaluation) {
  Individual individual = {
      .routes = std::move(routes), .tour = {}, .evaluation = evaluation};
  for (const Route& route : individual.routes) {
    individual.tour.insert(individual.tour.end(), route.begin(), route.end());
  }
  return individual;
}

Population::Population(std::size_t customer_count,
                       const PopulationParameters& parameters, double penalty)
    : customer_count_(customer_count),
      parameters_(parameters),
      penalty_(penalty) {}

Population::Population(Population&&) noexcept = default;
Population& Population::operator=(Population&&) noexcept = default;
Population::~Population() = default;

void Population::Add(Individual individual) {
  auto member = std::make_unique<Member>();
  member->successor.assign(customer_count_ + 1, 0);
  member->predecessor.assign(customer_count_ + 1, 0);
  for (const Route& route : individual.routes) {
    for (std::size_t index = 0; index < route.size(); ++index) {
      member->predecessor[route[index]] = index == 0 ? 0 : route[index - 1];
      member->successor[route[index]] =
          index + 1 == route.size() ? 0 : route[index + 1];
    }
  }
  const bool feasible = individual.evaluation.Feasible();
  member->individual = std::move(individual);

  Subpopulation& subpopulation = feasible ? feasible_ : infeasible_;
  const auto insert = [](std::vector<std::pair<double, const Member*>>& list,
                         double distance, const Member* other) {
    list.insert(
        std::ranges::upper_bound(list, distance, {},
                                 &std::pair<double, const Member*>::first),
        {distance, other});
  };
  for (const std::unique_ptr<Member>& other : subpopulation) {
    const double distance = Distance(*member, *other);
    insert(member->neighbors, distance, other.get());
    insert(other->neighbors, distance, member.get());
  }
  subpopulation.push_back(std::move(member));
  SortByCost(subpopulation);

  if (subpopulation.size() >=
      parameters_.min_size + parameters_.generation_size) {
    while (subpopulation.size() > parameters_.min_size) {
      RemoveWorst(subpopulation);
    }
  }
}

const Individual& Population::Select(Random& random) {
  UpdateFitness(feasible_);
  UpdateFitness(infeasible_);
  const auto size = static_cast<std::uint32_t>(Size());
  const auto member = [&](std::size_t index) -> const Member& {
    return index < feasible_.size() ? *feasible_[index]
                                    : *infeasible_[index - feasible_.size()];
  };
  const Member& first = member(random.Below(size));
  const Member& second = member(random.Below(size));
  return (first.fitness <= second.fitness ? first : second).individual;
}

void Population::SetPenalty(double penalty) {
  penalty_ = penalty;
  SortByCost(infeasible_);
}

std::size_t Population::Size() const {
  return feasible_.size() + infeasible_.size();
}

// The share of customers whose neighbours differ between the solutions.
double Population::Distance(const Member& a, const Member& b) const {
  if (customer_count_ == 0) {
    return 0.0;
  }
  std::size_t differences = 0;
  for (std::size_t customer = 1; customer <= customer_count_; ++customer) {
    if (a.successor[customer] != b.successor[customer] &&
        a.successor[customer] != b.predecessor[customer]) {
      ++differences;
    }
    if (a.predecessor[customer] == 0 && b.predecessor[customer] != 0 &&
        b.successor[customer] != 0) {
      ++differences;
    }
  }
  return static_cast<double>(differences) /
         static_cast<double>(customer_count_);
}

void Population::SortByCost(Subpopulation& subpopulation) const {
  std::ranges::stable_sort(subpopulation, {}, [this](const auto& member) {
    return member->individual.evaluation.Penalized(penalty_);
  });
}

// Fitness = cost rank + (1 - elite / size) * diversity rank, both in [0, 1]
// and lower being better; the subpopulation is sorted by cost.
void Population::UpdateFitness(Subpopulation& subpopulation) const {
  const std::size_t size = subpopulation.size();
  if (size <= 1) {
    for (const std::unique_ptr<Member>& member : subpopulation) {
      member->fitness = 0.0;
    }
    return;
  }
  // Average distance to the closest neighbours, the larger the better.
  std::vector<std::pair<double, std::size_t>> diversity;
  diversity.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    const auto& neighbors = subpopulation[index]->neighbors;
    const std::size_t count =
        std::min(parameters_.close_count, neighbors.size());
    double sum = 0.0;
    for (std::size_t neighbor = 0; neighbor < count; ++neighbor) {
      sum += neighbors[neighbor].first;
    }
    diversity.emplace_back(count == 0 ? 0.0 : -sum / static_cast<double>(count),
                           index);
  }
  std::ranges::sort(diversity);

  const auto last = static_cast<double>(size - 1);
  const double diversity_weight =
      size <= parameters_.elite_count
          ? 0.0
          : 1.0 - (static_cast<double>(parameters_.elite_count) /
                   static_cast<double>(size));
  for (std::size_t rank = 0; rank < size; ++rank) {
    const std::size_t index = diversity[rank].second;
    subpopulation[index]->fitness =
        (static_cast<double>(index) / last) +
        (diversity_weight * static_cast<double>(rank) / last);
  }
}

// Removes the member with the worst fitness, preferring clones, and never
// the cheapest. Precondition: the subpopulation has at least 2 members.
void Population::RemoveWorst(Subpopulation& subpopulation) {
  UpdateFitness(subpopulation);
  std::size_t worst = 1;
  bool worst_is_clone = false;
  double worst_fitness = -1.0;
  for (std::size_t index = 1; index < subpopulation.size(); ++index) {
    const Member& member = *subpopulation[index];
    const bool clone =
        !member.neighbors.empty() && member.neighbors.front().first < kEpsilon;
    if ((clone && !worst_is_clone) ||
        (clone == worst_is_clone && member.fitness > worst_fitness)) {
      worst = index;
      worst_is_clone = clone;
      worst_fitness = member.fitness;
    }
  }
  const Member* removed = subpopulation[worst].get();
  for (const std::unique_ptr<Member>& member : subpopulation) {
    std::erase_if(member->neighbors, [removed](const auto& entry) {
      return entry.second == removed;
    });
  }
  subpopulation.erase(subpopulation.begin() +
                      static_cast<std::ptrdiff_t>(worst));
}

}  // namespace vrp::core::solver
