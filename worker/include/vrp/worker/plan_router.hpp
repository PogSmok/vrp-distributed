#ifndef VRP_WORKER_PLAN_ROUTER_HPP_
#define VRP_WORKER_PLAN_ROUTER_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "vrp/core/edge_geometry.hpp"
#include "vrp/core/graph.hpp"
#include "vrp/core/result.hpp"
#include "vrp/core/solution.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"
#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {

// `routes` for the live map, as differences in 1e-7 degrees, without points
// a few metres from the one before.
[[nodiscard]] v1::RoadPlan ToRoadPlan(
    const v1::Attempt& attempt, std::uint32_t island, core::RouteCost cost,
    const std::vector<core::ResultRoute>& routes);

// Routes a solve task's best plan on the roads for the live map (ADR 0030),
// on one low-priority thread, so that the search does not wait for it. Only
// the latest plan offered is routed. A failure here is logged, and never
// affects the task. Destroying it stops the thread.
class PlanRouter {
 public:
  struct Roads {
    const core::RoadGraph* graph = nullptr;
    const core::EdgeGeometry* geometry = nullptr;
  };

  struct Task {
    v1::Attempt attempt;
    core::EdgeCost edge_cost = core::EdgeCost::kLength;
    core::Problem problem;
    std::vector<core::NodeId> stop_nodes;
  };

  using LoadRoads =
      std::function<core::StatusOr<Roads>(const std::function<bool()>&)>;

  // `load` runs on the router's thread, and gives up once the check it is
  // passed turns true. `send` and `warn` are called from that thread.
  // `while_stopping` runs repeatedly on the thread destroying this router
  // while it waits, since parsing a graph cannot be interrupted.
  PlanRouter(Task task, LoadRoads load, std::function<void(v1::RoadPlan)> send,
             std::function<void(const std::string&)> warn,
             std::function<void()> while_stopping);
  ~PlanRouter();

  PlanRouter(const PlanRouter&) = delete;
  PlanRouter& operator=(const PlanRouter&) = delete;
  PlanRouter(PlanRouter&&) = delete;
  PlanRouter& operator=(PlanRouter&&) = delete;

  void Offer(std::uint32_t island, core::RouteCost cost,
             core::Solution solution);

 private:
  struct Offered {
    std::uint32_t island = 0;
    core::RouteCost cost = 0;
    core::Solution solution;
  };

  void Run(const std::stop_token& stop);
  void RouteAndSend(const Roads& roads, const Offered& plan,
                    const std::stop_token& stop);

  Task task_;
  LoadRoads load_;
  std::function<void(v1::RoadPlan)> send_;
  std::function<void(const std::string&)> warn_;
  std::function<void()> while_stopping_;
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::optional<Offered> next_;
  std::atomic<bool> finished_{false};
  std::jthread thread_;  // last: it uses everything above
};

}  // namespace vrp::worker

#endif  // VRP_WORKER_PLAN_ROUTER_HPP_
