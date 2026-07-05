// Aster — ITCH parser implementation.

#include "aster/replay/parser.hpp"
#include "aster/replay/itch.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#if defined(__APPLE__)
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace aster::replay {

// Helper: read a big-endian integral from a byte pointer.
template <typename T>
static T read_be(const std::byte* p) {
  T v = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    v = static_cast<T>((v << 8) | static_cast<std::uint8_t>(p[i]));
  }
  return v;
}

ItchParser::ItchParser(const std::filesystem::path& path) {
  // Open file and get size.
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  struct stat st;
  if (::fstat(fd, &st) < 0) {
    ::close(fd);
    return;
  }
  size_ = static_cast<std::size_t>(st.st_size);
  if (size_ < sizeof(FileHeader)) {
    ::close(fd);
    return;
  }
  void* addr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (addr == MAP_FAILED) return;
  data_ = addr;

  // Parse header.
  auto* base = static_cast<const std::byte*>(data_);
  FileHeader hdr;
  std::memcpy(hdr.magic, base, 4);
  hdr.version = read_be<std::uint32_t>(base + 4);
  hdr.symbol_count = read_be<std::uint32_t>(base + 8);
  pos_ = 12;

  // Parse symbol table.
  symbols_.reserve(hdr.symbol_count);
  for (std::uint32_t i = 0; i < hdr.symbol_count; ++i) {
    if (pos_ >= size_) break;
    std::uint8_t name_len = static_cast<std::uint8_t>(base[pos_]);
    ++pos_;
    // Validate: name must be non-empty and fit within the file.
    if (name_len == 0 || pos_ + name_len + 2 > size_) break;
    std::string name(reinterpret_cast<const char*>(base + pos_), name_len);
    pos_ += name_len;
    SymbolID id = read_be<SymbolID>(base + pos_);
    pos_ += 2;
    symbols_.push_back({id, std::move(name)});
  }
}

ItchParser::~ItchParser() {
  if (data_ && size_ > 0) {
    ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
  }
}

std::size_t ItchParser::error_count() const noexcept { return errors_; }

ItchParser::ItchParser(ItchParser&& o) noexcept
    : data_(o.data_), size_(o.size_), pos_(o.pos_), count_(o.count_),
      symbols_(std::move(o.symbols_)) {
  o.data_ = nullptr;
  o.size_ = 0;
}
ItchParser& ItchParser::operator=(ItchParser&& o) noexcept {
  if (this != &o) {
    if (data_ && size_ > 0) ::munmap(data_, size_);
    data_ = o.data_; size_ = o.size_; pos_ = o.pos_; count_ = o.count_;
    symbols_ = std::move(o.symbols_);
    o.data_ = nullptr; o.size_ = 0;
  }
  return *this;
}

bool ItchParser::next(Message& out) {
  auto* base = static_cast<const std::byte*>(data_);
  // Each message: first byte is type, then timestamp (7 bytes BE), then payload.
  if (pos_ + 8 > size_) return false;
  char type = static_cast<char>(base[pos_]);
  // Timestamp is 7 bytes (56 bits) big-endian.
  std::uint64_t ts = 0;
  for (int i = 1; i < 8; ++i) {
    ts = (ts << 8) | static_cast<std::uint8_t>(base[pos_ + i]);
  }
  pos_ += 8;

  bool ok = true;
  switch (type) {
    case 'S': {
      if (pos_ + 1 > size_) { ok = false; break; }
      auto code = static_cast<SystemEventCode>(base[pos_]);
      ++pos_;
      out = SystemEventMsg{ts, code};
      break;
    }
    case 'A': {
      // order_id:8, symbol:2, side:1, price:8, qty:4 = 23 bytes
      if (pos_ + 23 > size_) { ok = false; break; }
      OrderID oid = read_be<OrderID>(base + pos_);
      pos_ += 8;
      SymbolID sym = read_be<SymbolID>(base + pos_);
      pos_ += 2;
      Side side = static_cast<Side>(base[pos_]);
      ++pos_;
      Price price = read_be<Price>(base + pos_);
      pos_ += 8;
      Qty qty = read_be<Qty>(base + pos_);
      pos_ += 4;
      // Validate: side must be 0 (Buy) or 1 (Sell); qty must be non-zero.
      if (static_cast<std::uint8_t>(side) > 1 || qty == 0) { ok = false; break; }
      out = OrderAddMsg{ts, oid, sym, side, price, qty};
      break;
    }
    case 'E': {
      if (pos_ + 12 > size_) { ok = false; break; }
      OrderID oid = read_be<OrderID>(base + pos_);
      pos_ += 8;
      Qty qty = read_be<Qty>(base + pos_);
      pos_ += 4;
      out = OrderExecuteMsg{ts, oid, qty};
      break;
    }
    case 'C': {
      if (pos_ + 12 > size_) { ok = false; break; }
      OrderID oid = read_be<OrderID>(base + pos_);
      pos_ += 8;
      Qty qty = read_be<Qty>(base + pos_);
      pos_ += 4;
      out = OrderCancelMsg{ts, oid, qty};
      break;
    }
    case 'D': {
      if (pos_ + 8 > size_) { ok = false; break; }
      OrderID oid = read_be<OrderID>(base + pos_);
      pos_ += 8;
      out = OrderDeleteMsg{ts, oid};
      break;
    }
    case 'L': {
      // L2 aggregate: symbol:2, side:1, price:8, qty:4, order_count:4 = 19 bytes
      if (pos_ + 19 > size_) { ok = false; break; }
      SymbolID sym = read_be<SymbolID>(base + pos_);
      pos_ += 2;
      Side side = static_cast<Side>(base[pos_]);
      ++pos_;
      Price price = read_be<Price>(base + pos_);
      pos_ += 8;
      Qty qty = read_be<Qty>(base + pos_);
      pos_ += 4;
      std::uint32_t order_count = read_be<std::uint32_t>(base + pos_);
      pos_ += 4;
      out = L2AggregateMsg{ts, sym, side, price, qty, order_count};
      break;
    }
    default:
      ok = false;
      break;
  }
  if (!ok) {
    ++errors_;
    return false;
  }
  ++count_;
  return true;
}

}  // namespace aster::replay
