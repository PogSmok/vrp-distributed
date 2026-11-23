#ifndef VRP_CORE_STATUS_MACROS_HPP_
#define VRP_CORE_STATUS_MACROS_HPP_

#include <utility>

#include "vrp/core/status.hpp"

// Error propagation
//
//   VRP_RETURN_IF_ERROR(ValidateProblem(problem));
//   VRP_ASSIGN_OR_RETURN(const RoadGraph graph, RoadGraph::Create(...));
//
// Both return the error from the enclosing function, which must return
// Status or StatusOr.

namespace vrp::core::status_internal {

// By value, so that no reference can outlive a temporary argument. Copying an
// OK status is cheap (an empty message).
inline Status ToStatus(const Status& status) { return status; }

template <typename T>
Status ToStatus(const StatusOr<T>& status_or) {
  return status_or.GetStatus();
}

}  // namespace vrp::core::status_internal

// Evaluates `expr`, a Status or StatusOr, once; returns its status from the
// enclosing function if it is an error.
#define VRP_RETURN_IF_ERROR(expr)                                       \
  do {                                                                  \
    const auto& vrp_status_macro_value = (expr);                        \
    const ::vrp::core::Status vrp_status_macro_status =                 \
        ::vrp::core::status_internal::ToStatus(vrp_status_macro_value); \
    if (!vrp_status_macro_status.IsOk()) {                              \
      return vrp_status_macro_status;                                   \
    }                                                                   \
  } while (false)

// Evaluates `rexpr`, a StatusOr, once. On error, returns its status from the
// enclosing function; otherwise moves the value into `lhs`, which may declare
// a new variable. A type containing commas must be wrapped in a type alias.
#define VRP_ASSIGN_OR_RETURN(lhs, rexpr) \
  VRP_ASSIGN_OR_RETURN_IMPL_(            \
      VRP_STATUS_MACROS_CONCAT_(vrp_status_or_, __LINE__), lhs, rexpr)

// `status_or` is a variable name and `lhs` may be a declaration, so neither
// can be parenthesised. The trailing `_` marks these macros as private.
// NOLINTBEGIN(bugprone-macro-parentheses,readability-identifier-naming)
#define VRP_ASSIGN_OR_RETURN_IMPL_(status_or, lhs, rexpr) \
  auto status_or = (rexpr);                               \
  if (!status_or.IsOk()) {                                \
    return status_or.GetStatus();                         \
  }                                                       \
  lhs = std::move(status_or).Value()

#define VRP_STATUS_MACROS_CONCAT_INNER_(x, y) x##y
#define VRP_STATUS_MACROS_CONCAT_(x, y) VRP_STATUS_MACROS_CONCAT_INNER_(x, y)
// NOLINTEND(bugprone-macro-parentheses,readability-identifier-naming)

#endif  // VRP_CORE_STATUS_MACROS_HPP_
