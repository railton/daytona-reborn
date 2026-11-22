// Real geometry handlers, synthetic zero-filled memories, trap TGP execution.
#include "runtime/m2_tgp_board.h"
#include "runtime/port_observation.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
namespace rt::tgpgen {
const uint32_t program_crc32 = 0, program_words = 0;
void run(Tgp&, uint64_t) { throw std::runtime_error("unexpected TGP execution"); }
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
#ifdef _WIN32
    CHECK(_putenv_s("M2_WRITE_TRACE", "tracking-only-no-file-created") == 0);
#else
    CHECK(setenv("M2_WRITE_TRACE", "tracking-only-no-file-created", 1) == 0);
#endif
    auto b = std::make_unique<rt::TgpBoard>(std::vector<uint8_t>(0x40000), std::vector<uint8_t>(0x800000));
    rt::PortObservation o;
    auto call = [&](auto handler) { rt::observe_port_write(o, b->write_provenance(), b->geo_r(0x2008/4), handler); };
    call([&] { b->geo_w(0x1008/4, 12); }); // cursor change only
    CHECK(!o.buffer_write_observed && o.buffer_write_count == 0);
    call([&] { b->geo_prg_w(42); });
    CHECK(o.buffer_write_observed && o.buffer_word_written == 3 && o.buffer_write_count == 1);
    CHECK(b->buffer_r(12) == 42);
    b->geoctl_w(0x80000000);
    call([&] { b->geo_prg_w(99); }); // ignored program upload
    CHECK(!o.buffer_write_observed && b->geo_r(0x2008/4) == 16);
    b->geoctl_w(0);
    call([&] { b->geo_w(1, 0); }); // ignored subregister lane
    CHECK(!o.buffer_write_observed && b->geo_r(0x2008/4) == 16);
    call([&] { b->geo_w(0x1008/4, 12); });
    call([&] { b->geo_w(0, 0); }); // full output word even when effective input is zero
    CHECK(o.buffer_write_observed && o.buffer_word_written == 3 && o.buffer_write_count == 2);
    CHECK(b->buffer_r(12) == 0);
    b->buffer_w(12, 7, 0xffffffffu); // another origin advances the same counter
    b->geo_w(0x1008/4, 12);
    call([&] { b->geo_prg_w(88); });
    CHECK(o.buffer_write_count == 4 && b->buffer_r(12) == 88);
    b->geo_w(0x1008/4, 0x20000);
    bool rejected = false;
    try { call([&] { b->geo_prg_w(1); }); } catch (const rt::TgpFatal&) { rejected = true; }
    CHECK(rejected && !o.buffer_write_observed);
    call([&] { b->geo_w(0x1008/4, 4); }); // valid cursor reset from out of range
    CHECK(!o.buffer_write_observed);
    call([&] { b->geo_prg_w(5); });
    CHECK(o.buffer_word_written == 1 && o.buffer_write_count == 1);
    return 0;
}
