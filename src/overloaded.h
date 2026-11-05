#pragma once

namespace minisql {

// The usual helper for std::visit with a set of lambdas.
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace minisql
