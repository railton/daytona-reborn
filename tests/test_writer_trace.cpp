// Real board-write hooks, with a trap stub for TGP execution (no game data).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "runtime/m2_tgp_board.h"
#include "runtime/geo.h"
namespace rt::tgpgen {
const uint32_t program_crc32 = 0, program_words = 0;
void run(Tgp &, uint64_t) { throw std::runtime_error("test must not run TGP"); }
}
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
void env(const char *value) {
#ifdef _WIN32
    if (_putenv_s("M2_WRITE_TRACE", value)) throw std::runtime_error("setenv");
#else
    if (setenv("M2_WRITE_TRACE", value, 1)) throw std::runtime_error("setenv");
#endif
}
void writes(rt::TgpBoard &board) {
    board.mem_w(0x400014, 1u << 23);
    board.buffer_w(20*4, 1u << 23, 0x00ff0000u); // Same value, mixed bit origins.
    board.geo_w(0x1008/4, 21*4);
    board.geo_prg_w(4);
    board.buffer_w(22*4, 8, 0xffffffffu);
    board.mem_w(0x400017, 0x800000);
    board.mem_w(0x400018, 1);
    board.mem_w(0x400019, 15u << 23);
    board.buffer_w(20*4, 0xffffffffu, 0); // No written bits.
    board.mem_w(0x200014, 0); // Not buffer RAM.
    board.geoctl_w(0x80000000);
    board.geo_prg_w(123); // Program upload path: no buffer write.
    board.geoctl_w(0);
}
int main(int argc, char **argv) {
    CHECK(argc==2);
    std::vector<uint8_t> tables(0x40000), data(0x800000);
    env(""); auto disabled=std::make_unique<rt::TgpBoard>(tables,data);
    CHECK(!disabled->write_provenance()); writes(*disabled);
    env(argv[1]); auto enabled=std::make_unique<rt::TgpBoard>(tables,data);
    CHECK(enabled->write_provenance()); writes(*enabled);
    for (unsigned i=0;i<0x8000;++i) CHECK(disabled->buffer()[i]==enabled->buffer()[i]);
    const auto *provenance=enabled->write_provenance();
    CHECK(provenance->at(20).tgp==0xff00ffffu && provenance->at(20).i960==0x00ff0000u);
    CHECK(provenance->at(20).writes==2 && provenance->at(20).last_mask==0x00ff0000u);
    CHECK(provenance->at(21).geometry_port==0xffffffffu && provenance->at(21).writes==1);
    CHECK(provenance->at(22).i960==0xffffffffu && provenance->at(22).writes==1);
    CHECK(provenance->at(23).tgp==0xffffffffu && provenance->at(24).tgp==0xffffffffu);
    CHECK(provenance->at(26).unknown==0xffffffffu && provenance->at(26).writes==0);
    std::vector<uint8_t> polygons(128), textures(128);
    auto geo=std::make_unique<rt::Geo>(polygons,textures,enabled->buffer_data());
    geo->set_write_provenance(provenance);
    rt::m2trace::frame_begin(7); geo->parse(20*4); rt::m2trace::frame_end(); rt::m2trace::finish();
    auto *file=std::fopen(argv[1],"rb"); CHECK(file); char line[512];
    CHECK(std::fgets(line,sizeof line,file));
    CHECK(std::string(line)=="frame,command_word,argument_index,buffer_word,unknown_mask,i960_mask,tgp_mask,port_mask,last_mask,writes\n");
    for (unsigned argument=0;argument<5;++argument) {
        CHECK(std::fgets(line,sizeof line,file)); std::stringstream stream(line); std::string part;
        std::vector<uint64_t> fields; while(std::getline(stream,part,',')) fields.push_back(std::stoull(part));
        const auto &p=provenance->at(20+argument);
        CHECK(fields==std::vector<uint64_t>({7,20,argument,20+argument,p.unknown,p.i960,p.tgp,p.geometry_port,p.last_mask,p.writes}));
    }
    CHECK(!std::fgets(line,sizeof line,file));std::fclose(file);
    std::puts("three board writers, masked/ignored writes, buffer equivalence and argument snapshots passed");
}
