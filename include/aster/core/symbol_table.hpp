// Aster — SymbolTable.
//
// Interns string <-> SymbolID. Cold path only (startup + output). Lives in
// its own header so that core hot-path headers (types.hpp) don't pull in
// <string>, <string_view>, <unordered_map>, <vector>.

#pragma once

#include "aster/core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aster {

class SymbolTable {
 public:
  SymbolID intern(std::string_view name) {
    auto it = rev_.find(name);
    if (it != rev_.end()) return it->second;
    SymbolID id = static_cast<SymbolID>(names_.size());
    names_.emplace_back(name);
    std::string_view stored(names_.back());
    rev_[stored] = id;
    return id;
  }

  std::string_view lookup(SymbolID id) const {
    return names_[static_cast<std::size_t>(id)];
  }

  std::size_t size() const { return names_.size(); }

  void reserve(std::size_t n) {
    names_.reserve(n);
    rev_.reserve(n);
  }

 private:
  std::vector<std::string> names_;
  std::unordered_map<std::string_view, SymbolID> rev_;
};

}  // namespace aster
