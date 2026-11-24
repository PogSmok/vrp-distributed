#include "vrp/worker/plan_router.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "vrp/core/leg_router.hpp"
#include "vrp/core/result.hpp"
#include "vrp/core/status.hpp"
#include "vrp/core/types.hpp"
#include "vrp/v1/vrp.pb.h"

namespace vrp::worker {
namespace {

constexpr double kUnitsPerDegree = 1e7;
constexpr std::int32_t kMinStep = 400;  // about 4 m
// Under the 16 MiB that the orchestrator and client accept, with room for the
// rest of a poll. 5,000 customers on Mazowieckie take about 6.5 MB.
constexpr std::size_t kMaxPlanBytes = 15'000'000;
constexpr std::chrono::milliseconds kStopPoll{50};

// The search must keep the CPU whenever it wants it.
void LowerThreadPriority() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  // On Linux, a nice value applies to one thread.
  static_cast<void>(
      setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10));
#endif
}

std::int32_t ToUnits(double degrees) {
  return static_cast<std::int32_t>(std::lround(degrees * kUnitsPerDegree));
}

void FillRoute(const core::ResultRoute& route, v1::RoadRoute& out) {
  out.mutable_stops()->Add(route.stops.begin(), route.stops.end());
  out.set_cost(route.cost);
  std::int32_t last_latitude = 0;
  std::int32_t last_longitude = 0;
  for (std::size_t index = 0; index < route.polyline.size(); ++index) {
    const std::int32_t latitude = ToUnits(route.polyline[index].latitude);
    const std::int32_t longitude = ToUnits(route.polyline[index].longitude);
    const bool at_end = index == 0 || index + 1 == route.polyline.size();
    if (!at_end && std::abs(latitude - last_latitude) < kMinStep &&
        std::abs(longitude - last_longitude) < kMinStep) {
      continue;
    }
    out.add_latitudes(latitude - last_latitude);
    out.add_longitudes(longitude - last_longitude);
    last_latitude = latitude;
    last_longitude = longitude;
  }
}

}  // namespace

v1::RoadPlan ToRoadPlan(const v1::Attempt& attempt, std::uint32_t island,
                        core::RouteCost cost,
                        const std::vector<core::ResultRoute>& routes) {
  v1::RoadPlan plan;
  *plan.mutable_attempt() = attempt;
  plan.set_island(island);
  plan.set_cost(cost);
  for (const core::ResultRoute& route : routes) {
    FillRoute(route, *plan.add_routes());
  }
  return plan;
}

PlanRouter::PlanRouter(Task task, LoadRoads load,
                       std::function<void(v1::RoadPlan)> send,
                       std::function<void(const std::string&)> warn,
                       std::function<void()> while_stopping)
    : task_(std::move(task)),
      load_(std::move(load)),
      send_(std::move(send)),
      warn_(std::move(warn)),
      while_stopping_(std::move(while_stopping)),
      thread_([this](const std::stop_token& stop) {
        Run(stop);
        finished_ = true;
      }) {}

PlanRouter::~PlanRouter() {
  thread_.request_stop();
  while (!finished_) {
    while_stopping_();
    std::this_thread::sleep_for(kStopPoll);
  }
}

void PlanRouter::Offer(std::uint32_t island, core::RouteCost cost,
                       core::Solution solution) {
  {
    const std::scoped_lock lock(mutex_);
    next_ = Offered{
        .island = island, .cost = cost, .solution = std::move(solution)};
  }
  ready_.notify_one();
}

void PlanRouter::Run(const std::stop_token& stop) {
  LowerThreadPriority();
  try {
    const core::StatusOr<Roads> roads =
        load_([&stop] { return stop.stop_requested(); });
    if (!roads.IsOk()) {
      if (roads.GetStatus().Code() != core::StatusCode::kCancelled) {
        warn_("no live road plans: " + roads.GetStatus().Message());
      }
      return;
    }
    while (true) {
      std::optional<Offered> plan;
      {
        std::unique_lock lock(mutex_);
        // Returns with nothing waiting only once stopped.
        static_cast<void>(
            ready_.wait(lock, stop, [this] { return next_.has_value(); }));
        plan.swap(next_);
      }
      if (!plan.has_value()) {
        return;
      }
      RouteAndSend(roads.Value(), *plan, stop);
    }
  } catch (const std::exception& error) {
    warn_(std::string("no live road plans: ") + error.what());
  }
}

void PlanRouter::RouteAndSend(const Roads& roads, const Offered& plan,
                              const std::stop_token& stop) {
  const core::StatusOr<std::vector<core::ResultRoute>> routes =
      core::RouteSolution(*roads.graph, *roads.geometry, task_.edge_cost,
                          task_.problem, task_.stop_nodes, plan.solution,
                          plan.cost, core::SequentialFor,
                          [&stop] { return stop.stop_requested(); });
  if (!routes.IsOk()) {
    if (routes.GetStatus().Code() != core::StatusCode::kCancelled) {
      warn_("a live road plan failed: " + routes.GetStatus().Message());
    }
    return;
  }
  v1::RoadPlan message =
      ToRoadPlan(task_.attempt, plan.island, plan.cost, routes.Value());
  if (message.ByteSizeLong() > kMaxPlanBytes) {
    warn_("a live road plan is too large to send");
    return;
  }
  send_(std::move(message));
}

}  // namespace vrp::worker
