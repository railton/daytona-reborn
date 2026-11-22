#include "runtime/race_request.h"
#include <cstdio>
#include <memory>
#include <string>
struct Launcher { bool valid; bool rom_ok() const{return valid;} std::string error()const{return "load failed";} };
struct Audio {int closed=0; void close(){++closed;}};
struct Game {int id;rt::RaceStartResult result;rt::StartRaceRequest received{};
 rt::RaceStartResult request_race(rt::StartRaceRequest r){received=r;return result;}};
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"case %d line %d: %s\n",scenario,__LINE__,#x);return false;}}while(0)
bool run(int scenario){
 Launcher launcher{scenario!=0};Audio native_audio,audio;
 auto game=std::make_unique<Game>(Game{1,rt::RaceStartResult::Accepted});
 auto link=std::make_unique<int>(1);bool have_audio=true;
 bool solo=false,pc_race=true,have_frame=true,in_launcher=true;std::string home_message;
 int attempts=0;
 auto start_game=[&](bool solo_mode){++attempts;if(!solo_mode)return false;game.reset();link.reset();
   if(scenario==1)return false;
   game=std::make_unique<Game>(Game{2,scenario==2?rt::RaceStartResult::UnsupportedRevision:rt::RaceStartResult::Accepted});
   return true;};
 #include "begin_solo.inc"
 auto request=rt::StartRaceRequest{rt::RaceCourse::Expert,rt::RaceTransmission::Manual};
 if(scenario==4)request.course=static_cast<rt::RaceCourse>(99);
 if(scenario==5)request.transmission=static_cast<rt::RaceTransmission>(99);
 const bool accepted=begin_solo(request);
 if(scenario==0||scenario>=4){CHECK(!accepted&&attempts==0&&game&&game->id==1&&pc_race&&have_frame&&in_launcher);
   CHECK(native_audio.closed==0&&audio.closed==0&&have_audio&&link&&!home_message.empty());}
 else if(scenario==1){CHECK(!accepted&&attempts==1&&!game&&!solo&&!pc_race&&!have_frame&&in_launcher);}
 else if(scenario==2){CHECK(!accepted&&attempts==1&&!game&&!solo&&!pc_race&&!have_frame&&in_launcher);
   CHECK(native_audio.closed==1&&audio.closed==1&&!link&&!have_audio);
   CHECK(home_message=="La demande de course a été refusée.");}
 else{CHECK(accepted&&game&&game->id==2&&solo&&!pc_race&&!have_frame&&!in_launcher);
   CHECK(game->received.course==request.course&&game->received.transmission==request.transmission);
   CHECK(native_audio.closed==0&&audio.closed==0&&have_audio&&home_message.empty());}
 return true;
}
int main(){for(int i=0;i<6;++i)if(!run(i))return 1;std::puts("actual begin_solo callback: preflight/load-fail/reject/accept passed");}
