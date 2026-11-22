#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rt {

enum class WriteOrigin : std::uint8_t {
  Unknown = 0,
  I960 = 1,
  Tgp = 2,
  GeometryPort = 3
};

struct WordWriteProvenance {
  std::uint32_t unknown = 0xFFFFFFFFu;
  std::uint32_t i960 = 0u;
  std::uint32_t tgp = 0u;
  std::uint32_t geometry_port = 0u;
  std::uint32_t last_mask = 0u;
  std::uint64_t writes = 0u;
};

class BufferWriteProvenance {
 public:
  explicit BufferWriteProvenance(std::size_t words) : words_(words) {}

  // Validates index and origin before any mutation. A zero mask is a no-op
  // and does not increment writes. A nonzero mask clears those bits from
  // every origin, assigns them to origin, stores last_mask, and increments
  // the per-word count. UINT64_MAX rejects the call with state unchanged.
  bool record(std::size_t index, std::uint32_t mask, WriteOrigin origin) {
    if (index >= words_.size() || !valid_origin(origin)) {
      return false;
    }
    if (mask == 0u) {
      return true;
    }
    WordWriteProvenance& word = words_[index];
    if (word.writes == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }
    word.unknown &= ~mask;
    word.i960 &= ~mask;
    word.tgp &= ~mask;
    word.geometry_port &= ~mask;
    switch (origin) {
      case WriteOrigin::Unknown:
        word.unknown |= mask;
        break;
      case WriteOrigin::I960:
        word.i960 |= mask;
        break;
      case WriteOrigin::Tgp:
        word.tgp |= mask;
        break;
      case WriteOrigin::GeometryPort:
        word.geometry_port |= mask;
        break;
    }
    word.last_mask = mask;
    word.writes += 1u;
    return true;
  }

  const WordWriteProvenance& at(std::size_t index) const {
    if (index >= words_.size()) {
      throw std::out_of_range("write provenance index");
    }
    return words_[index];
  }

  void reset() {
    for (WordWriteProvenance& word : words_) {
      word = WordWriteProvenance{};
    }
  }

  std::size_t size() const { return words_.size(); }

 private:
  static bool valid_origin(WriteOrigin origin) {
    switch (origin) {
      case WriteOrigin::Unknown:
      case WriteOrigin::I960:
      case WriteOrigin::Tgp:
      case WriteOrigin::GeometryPort:
        return true;
    }
    return false;
  }

  std::vector<WordWriteProvenance> words_;
};

}  // namespace rt
