#ifndef VRP_CORE_SOLVER_PARAMETER_HPP_
#define VRP_CORE_SOLVER_PARAMETER_HPP_

#include <string>

namespace vrp::core {

// A solver parameter by name, as results record them (ADR 0028).
struct SolverParameter {
  std::string name;
  double value = 0.0;
};

}  // namespace vrp::core

#endif  // VRP_CORE_SOLVER_PARAMETER_HPP_
