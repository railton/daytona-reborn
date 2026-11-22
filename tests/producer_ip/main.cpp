#include "runtime/gen_support.h"
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>
struct Row { uint32_t ip,addr,data,width; uint64_t count; };
struct BoundaryObservation {
 unsigned calls = 0;
 std::size_t writes = 0;
 uint32_t ip = 0;
 uint64_t count = 0;
};
struct ProbeBus:rt::Bus {
 rt::Cpu *cpu=nullptr; rt::Lockstep *ls=nullptr; std::vector<Row> rows;
 BoundaryObservation hook;
 void observe(BoundaryObservation &observation) const {
   observation = {observation.calls + 1, rows.size(), cpu->m_IP, ls->count};
 }
 uint32_t fetch(uint32_t)override{return 0;}
 uint8_t read_byte(uint32_t)override{return 0;}
 uint16_t read_word(uint32_t)override{return 0;}
 uint32_t read_dword(uint32_t)override{return 0;}
 void record(uint32_t a,uint32_t d,uint32_t w){rows.push_back({cpu->m_IP,a,d,w,ls->count});}
 void write_byte(uint32_t a,uint8_t d)override{record(a,d,1);}
 void write_word(uint32_t a,uint16_t d)override{record(a,d,2);}
 void write_dword(uint32_t a,uint32_t d)override{record(a,d,4);}
};
void rt::hook_fixture_before_store(Cpu &cpu) {
 auto &bus = *static_cast<ProbeBus *>(cpu.bus);
 bus.observe(bus.hook);
}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc, char **argv){
 const bool boundary = argc == 2 && std::string_view(argv[1]) == "--boundary";
 CHECK(argc == 1 || boundary);
 CHECK(boundary || PROBE_HOOK == 0);
 ProbeBus bus;rt::Cpu cpu(&bus);rt::Lockstep ls(cpu);bus.cpu=&cpu;bus.ls=&ls;
 cpu.m_IP=0x1000;cpu.m_r[5]=0x11111111;cpu.m_r[6]=0x22222222;cpu.m_r[7]=0xAB;
 BoundaryObservation callback;
 if (boundary) ls.add_callback(1, [&] { bus.observe(callback); });
 ls.end_count=4;gen::Env env{cpu,ls};gen::run(env);
 CHECK(ls.count==4 && bus.rows.size()==3);
 const uint32_t pcs[]={0x1008,0x100c,0x1010};
 const uint32_t values[]={0x11111111,0x22222222,0xAB};
 // Block starting count for each store with no hook, first hook or second hook.
 constexpr unsigned block_starts[][3] = {{0, 0, 0}, {1, 1, 1}, {0, 2, 2}};
 constexpr uint32_t block_pcs[] = {0x1000, 0x1008, 0x100c};
 for(unsigned i=0;i<3;++i){const auto &v=bus.rows[i];
   CHECK(v.addr==(i==2 ? 0x800001u : 0x800000u));CHECK(v.width==(i==2 ? 1u : 4u));CHECK(v.data==values[i]);
   const unsigned block_start = block_starts[PROBE_HOOK][i];
   CHECK(v.ip == (PROBE_BLOCK ? block_pcs[block_start] : pcs[i]));
   CHECK(v.count == (PROBE_BLOCK ? block_start : i + 1));
   if (boundary) continue;
   std::printf("{\"mode\":\"%s\",\"ip\":%u,\"count\":%llu,\"address\":%u,\"width\":%u,\"value\":%u}\n",PROBE_BLOCK?"block":"exact",v.ip,(unsigned long long)v.count,v.addr,v.width,v.data);
 }
 if (boundary) {
   constexpr bool late_hook = PROBE_HOOK == 2;
   constexpr unsigned expected_calls = !PROBE_BLOCK || PROBE_HOOK ? 1 : 0;
   CHECK(callback.calls == expected_calls);
   if (expected_calls) {
     // Block-mode replay can report the scheduled count after a store ran.
     CHECK(callback.count == 1);
     CHECK(callback.writes == (late_hook ? 1u : 0u));
     CHECK(callback.ip == (late_hook ? 0x100cu : 0x1008u));
   }
   CHECK(bus.hook.calls == (PROBE_HOOK ? 1u : 0u));
   if (PROBE_HOOK) {
     CHECK(bus.hook.count == (late_hook ? 2u : 1u));
     CHECK(bus.hook.writes == (late_hook ? 1u : 0u));
     CHECK(bus.hook.ip == (late_hook ? 0x100cu : 0x1008u));
   }
   return 0;
 }
 const auto parked=cpu.m_IP;bus.write_dword(0x800000,0x33333333);
 CHECK(bus.rows.back().ip==parked && bus.rows.back().count==4 && ls.count==4);
 std::printf("{\"host_poke_ip\":%u,\"host_poke_count\":%llu}\n",parked,(unsigned long long)ls.count);
}
