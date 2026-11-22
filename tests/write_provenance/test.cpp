#include "runtime/write_provenance.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <random>
#include <limits>
#include <vector>

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  }
}

bool same(const rt::WordWriteProvenance& a, const rt::WordWriteProvenance& b) {
  return a.unknown == b.unknown && a.i960 == b.i960 && a.tgp == b.tgp &&
         a.geometry_port == b.geometry_port && a.last_mask == b.last_mask &&
         a.writes == b.writes;
}

bool fresh(const rt::WordWriteProvenance& w) {
  rt::WordWriteProvenance init;
  return same(w, init);
}

bool partitioned(const rt::WordWriteProvenance& w) {
  const std::uint32_t u = w.unknown;
  const std::uint32_t a = w.i960;
  const std::uint32_t b = w.tgp;
  const std::uint32_t c = w.geometry_port;
  if ((u | a | b | c) != 0xFFFFFFFFu) {
    return false;
  }
  return (u & a) == 0u && (u & b) == 0u && (u & c) == 0u && (a & b) == 0u &&
         (a & c) == 0u && (b & c) == 0u;
}

std::vector<rt::WordWriteProvenance> snap(const rt::BufferWriteProvenance& buf) {
  std::vector<rt::WordWriteProvenance> out;
  out.reserve(buf.size());
  for (std::size_t i = 0; i < buf.size(); ++i) {
    out.push_back(buf.at(i));
  }
  return out;
}

bool same_all(const rt::BufferWriteProvenance& buf,
              const std::vector<rt::WordWriteProvenance>& saved) {
  if (saved.size() != buf.size()) {
    return false;
  }
  for (std::size_t i = 0; i < buf.size(); ++i) {
    if (!same(buf.at(i), saved[i])) {
      return false;
    }
  }
  return true;
}

std::uint32_t gather(const rt::WriteOrigin bits[32], rt::WriteOrigin who) {
  std::uint32_t mask = 0u;
  for (int bit = 0; bit < 32; ++bit) {
    if (bits[bit] == who) {
      mask |= (std::uint32_t{1} << bit);
    }
  }
  return mask;
}

bool matches_oracle(const rt::WordWriteProvenance& w, const rt::WriteOrigin bits[32]) {
  return w.unknown == gather(bits, rt::WriteOrigin::Unknown) &&
         w.i960 == gather(bits, rt::WriteOrigin::I960) &&
         w.tgp == gather(bits, rt::WriteOrigin::Tgp) &&
         w.geometry_port == gather(bits, rt::WriteOrigin::GeometryPort);
}

void test_basic() {
  rt::BufferWriteProvenance buf(4);
  expect(buf.size() == 4u, "size");
  for (std::size_t i = 0; i < buf.size(); ++i) {
    expect(fresh(buf.at(i)), "initial unknown");
    expect(partitioned(buf.at(i)), "initial partition");
  }

  expect(buf.record(0, 0xFFFFFFFFu, rt::WriteOrigin::Tgp), "full record");
  {
    const rt::WordWriteProvenance& w = buf.at(0);
    expect(w.unknown == 0u && w.i960 == 0u && w.tgp == 0xFFFFFFFFu &&
               w.geometry_port == 0u && w.last_mask == 0xFFFFFFFFu &&
               w.writes == 1u,
           "full tgp");
    expect(partitioned(w), "full partition");
    expect(fresh(buf.at(1)) && fresh(buf.at(2)) && fresh(buf.at(3)),
           "other words idle after full");
  }

  expect(buf.record(1, 0xFFFF0000u, rt::WriteOrigin::I960), "partial i960");
  expect(buf.record(1, 0x00FFFF00u, rt::WriteOrigin::GeometryPort), "partial geo");
  {
    const rt::WordWriteProvenance& w = buf.at(1);
    expect(w.i960 == 0xFF000000u, "mixed i960");
    expect(w.geometry_port == 0x00FFFF00u, "mixed geo");
    expect(w.tgp == 0u, "mixed tgp clear");
    expect(w.unknown == 0x000000FFu, "mixed unknown remainder");
    expect(w.last_mask == 0x00FFFF00u && w.writes == 2u, "mixed meta");
    expect(partitioned(w), "mixed partition");
  }

  {
    const auto before = snap(buf);
    expect(buf.record(1, 0u, rt::WriteOrigin::Tgp), "zero mask");
    expect(same_all(buf, before), "zero mask preserves");
    expect(buf.record(0, 0u, static_cast<rt::WriteOrigin>(9)) == false,
           "zero mask still validates enum");
    expect(buf.record(9, 0u, rt::WriteOrigin::I960) == false,
           "zero mask still validates index");
    expect(same_all(buf, before), "rejected zero preserves");
  }

  expect(buf.record(2, 0x0000FF00u, rt::WriteOrigin::I960), "same origin first");
  expect(buf.record(2, 0x00000FF0u, rt::WriteOrigin::I960), "same origin second");
  {
    const rt::WordWriteProvenance& w = buf.at(2);
    expect(w.i960 == 0x0000FFF0u, "same origin bits");
    expect(w.unknown == 0xFFFF000Fu, "same origin unknown");
    expect(w.tgp == 0u && w.geometry_port == 0u, "same origin others");
    expect(w.last_mask == 0x00000FF0u && w.writes == 2u, "same origin meta");
    expect(partitioned(w), "same origin partition");
  }

  expect(buf.record(2, 0x01020408u, rt::WriteOrigin::Unknown), "sparse unknown");
  {
    const rt::WordWriteProvenance& w = buf.at(2);
    expect(w.unknown == (0xFFFF000Fu | 0x01020408u), "sparse unknown owns bits");
    expect((w.i960 & 0x01020408u) == 0u, "sparse cleared from i960");
    expect(w.last_mask == 0x01020408u && w.writes == 3u, "sparse meta");
    expect(partitioned(w), "sparse partition");
  }

  {
    const auto before = snap(buf);
    expect(buf.record(4, 0x1u, rt::WriteOrigin::I960) == false, "index == size");
    expect(buf.record(100, 0xFFFFFFFFu, rt::WriteOrigin::GeometryPort) == false,
           "index far");
    expect(buf.record(static_cast<std::size_t>(-1), 0x7u, rt::WriteOrigin::Tgp) == false,
           "index max");
    expect(same_all(buf, before), "oor preserves all");
  }

  {
    const auto before = snap(buf);
    expect(buf.record(0, 0x1u, static_cast<rt::WriteOrigin>(4)) == false, "enum 4");
    expect(buf.record(3, 0x80000000u, static_cast<rt::WriteOrigin>(255)) == false,
           "enum 255");
    expect(buf.record(1, 0xFu, static_cast<rt::WriteOrigin>(7)) == false, "enum 7");
    expect(same_all(buf, before), "bad enum preserves all");
  }

  buf.reset();
  expect(buf.size() == 4u, "reset preserves size");
  for (std::size_t i = 0; i < buf.size(); ++i) {
    expect(fresh(buf.at(i)), "reset fresh");
    expect(partitioned(buf.at(i)), "reset partition");
  }
}

void test_oracle() {
  constexpr std::size_t kWords = 4;
  rt::BufferWriteProvenance buf(kWords);
  rt::WriteOrigin bits[kWords][32];
  std::uint32_t last[kWords];
  std::uint64_t writes[kWords];
  for (std::size_t w = 0; w < kWords; ++w) {
    last[w] = 0u;
    writes[w] = 0u;
    for (int bit = 0; bit < 32; ++bit) {
      bits[w][bit] = rt::WriteOrigin::Unknown;
    }
  }

  std::mt19937 rng(0xC0FFEEu);
  auto next = [&rng]() { return rng(); };

  constexpr int kSteps = 256;
  for (int step = 0; step < kSteps; ++step) {
    const std::size_t slot = static_cast<std::size_t>(next() % kWords);
    const auto origin = static_cast<rt::WriteOrigin>(next() % 4u);
    std::uint32_t mask = next();
    if (step % 17 == 0) {
      mask = 0u;
    } else if (step % 17 == 1) {
      mask = 0xFFFFFFFFu;
    } else if (step % 17 == 2) {
      mask = 0x01020408u;
    } else if (step % 17 == 3) {
      mask = std::uint32_t{1} << (next() % 32u);
    }

    expect(buf.record(slot, mask, origin), "oracle record");
    if (mask != 0u) {
      for (int bit = 0; bit < 32; ++bit) {
        if ((mask & (std::uint32_t{1} << bit)) != 0u) {
          bits[slot][bit] = origin;
        }
      }
      last[slot] = mask;
      writes[slot] += 1u;
    }

    for (std::size_t w = 0; w < kWords; ++w) {
      const rt::WordWriteProvenance& word = buf.at(w);
      expect(matches_oracle(word, bits[w]), "oracle masks");
      expect(partitioned(word), "oracle partition");
      expect(word.last_mask == last[w], "oracle last");
      expect(word.writes == writes[w], "oracle writes");
    }
  }
}

void test_all_origin_transfers_and_overflow() {
  for (unsigned from=0;from<4;++from) for (unsigned to=0;to<4;++to)
      for (const std::uint32_t mask : {0x5aa58142u, 0xffffffffu}) {
    rt::BufferWriteProvenance buf(1);
    expect(buf.record(0,0xffffffffu,static_cast<rt::WriteOrigin>(from)), "transfer setup");
    expect(buf.record(0,mask,static_cast<rt::WriteOrigin>(to)), "transfer write");
    rt::WriteOrigin bits[32];
    for(unsigned bit=0;bit<32;++bit)
      bits[bit]=static_cast<rt::WriteOrigin>((mask & (std::uint32_t{1}<<bit)) ? to : from);
    expect(matches_oracle(buf.at(0),bits), "every origin transfer bit oracle");
    expect(partitioned(buf.at(0)), "every origin transfer partition");
    expect(buf.at(0).last_mask == mask && buf.at(0).writes == 2, "every transfer metadata");
  }
  rt::BufferWriteProvenance buf(1);
  const auto fresh_state=snap(buf);
  expect(buf.record(0,0,rt::WriteOrigin::Tgp), "fresh zero mask accepted");
  expect(same_all(buf,fresh_state), "fresh zero mask unchanged");
  // Legal test seam: buf is non-const; production API deliberately returns const.
  const_cast<rt::WordWriteProvenance&>(buf.at(0)).writes=std::numeric_limits<std::uint64_t>::max();
  const auto full=snap(buf);
  expect(!buf.record(0,1,rt::WriteOrigin::Tgp), "overflow rejected");
  expect(same_all(buf,full), "overflow leaves all fields unchanged");
  expect(buf.record(0,0,rt::WriteOrigin::Tgp), "zero mask at max is still no-op");
  expect(same_all(buf,full), "zero mask at max unchanged");
  bool threw=false;try { (void)buf.at(1); } catch(const std::out_of_range&) { threw=true; }
  expect(threw,"at out of range throws");
}

}  // namespace

int main() {
  test_basic();
  test_oracle();
  test_all_origin_transfers_and_overflow();
  if (g_fails != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
