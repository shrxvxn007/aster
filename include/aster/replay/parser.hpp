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

  // Iterate messages. Returns false at end or on error. Call error_count()
  // after the loop to distinguish a clean end-of-file (0) from a truncated or
  // corrupt stream (>0).
  bool next(Message& out);

  // Number of messages parsed so far.
  std::uint64_t count() const { return count_; }

  // Number of parse errors encountered (truncated/corrupt messages, invalid
  // fields). A clean replay ends with error_count() == 0.
  std::size_t error_count() const noexcept;

 private:
  void* data_ = nullptr;  // mmap'd buffer
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
  std::uint64_t count_ = 0;
  std::size_t errors_ = 0;
  std::vector<SymbolEntry> symbols_;
};

}  // namespace aster::replay
