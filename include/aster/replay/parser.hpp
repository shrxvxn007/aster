// Aster — ITCH file parser.
//
// Memory-maps the file for zero-copy access. First pass builds the symbol
// table; second pass iterates messages. Deterministic: same file → same output.

#pragma once

#include "itch.hpp"

#include "aster/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace aster::replay {

class ItchParser {
 public:
  explicit ItchParser(const std::filesystem::path& path);
  ~ItchParser();

  // Non-copyable (owns mmap buffer).
  ItchParser(const ItchParser&) = delete;
  ItchParser& operator=(const ItchParser&) = delete;
  ItchParser(ItchParser&&) noexcept;
  ItchParser& operator=(ItchParser&&) noexcept;

  bool is_open() const { return data_ != nullptr; }
  const std::vector<SymbolEntry>& symbols() const { return symbols_; }

  // Iterate messages. Returns false at end or on error.
  bool next(Message& out);

  // Number of messages parsed so far.
  std::uint64_t count() const { return count_; }

 private:
  void* data_ = nullptr;  // mmap'd buffer
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint64_t count_ = 0;
  std::vector<SymbolEntry> symbols_;
};

}  // namespace aster::replay
