// Test the wheel haptics lifecycle against a deterministic SDL backend.

#include "app/ffb.h"
#include <array>
#include <cstdio>
#include <stdexcept>

namespace {
int joystick_token;
int haptic_token;
auto *const wheel=reinterpret_cast<SDL_Joystick*>(&joystick_token);
auto *const haptic=reinterpret_cast<SDL_Haptic*>(&haptic_token);
constexpr SDL_JoystickID wheel_id=23;
std::array<SDL_HapticEffect,3> effects{};
std::array<bool,3> running{};
int opened=0,closed=0,created=0,stopped=0,rumble=0,updates=0,starts=0;
Uint64 now_ms=0;
bool fail_open=false,fail_update=false,fail_start=false,fail_stop=false;
int reject_update_id=-1;
void require(bool condition,const char *message) {
    if (!condition) throw std::runtime_error(message);
}
bool quiet() {
    return !running[0] && !running[1] && !running[2];
}
void update(app::ForceFeedback &ffb,const app::Devices &d,const app::Controls &c,
            const std::vector<uint8_t> &cmd,bool invert=false) {
    ffb.update(cmd,d,c,1.f,invert);
}
}
SDL_Joystick *app::Devices::find(const std::string &guid) const {
    return guid=="private-mock-wheel" ? wheel : nullptr;
}

extern "C" {
SDL_Joystick *SDLCALL SDL_GetGamepadJoystick(SDL_Gamepad *) { return nullptr; }
SDL_JoystickID SDLCALL SDL_GetJoystickID(SDL_Joystick *) { return wheel_id; }
bool SDLCALL SDL_IsJoystickHaptic(SDL_Joystick *) { return true; }
bool SDLCALL SDL_GetBooleanProperty(SDL_PropertiesID,const char *,bool fallback) { return fallback; }
SDL_PropertiesID SDLCALL SDL_GetJoystickProperties(SDL_Joystick *) { return 1; }
SDL_Haptic *SDLCALL SDL_OpenHapticFromJoystick(SDL_Joystick *) {
    ++opened;
    if (fail_open) return nullptr;
    created=0;
    running.fill(false);
    return haptic;
}
const char *SDLCALL SDL_GetJoystickName(SDL_Joystick *) { return "private mock wheel"; }
const char *SDLCALL SDL_GetHapticName(SDL_Haptic *) { return "private mock haptic"; }
const char *SDLCALL SDL_GetError(void) { return "injected failure"; }
Uint64 SDLCALL SDL_GetTicks(void) { return now_ms; }
Uint32 SDLCALL SDL_GetHapticFeatures(SDL_Haptic *) {
    return SDL_HAPTIC_AUTOCENTER|SDL_HAPTIC_GAIN|SDL_HAPTIC_CONSTANT|SDL_HAPTIC_SPRING|SDL_HAPTIC_FRICTION;
}
bool SDLCALL SDL_SetHapticAutocenter(SDL_Haptic *,int value) { return value==0; }
bool SDLCALL SDL_SetHapticGain(SDL_Haptic *,int value) { return value==100; }
SDL_HapticEffectID SDLCALL SDL_CreateHapticEffect(SDL_Haptic *,const SDL_HapticEffect *effect) {
    effects.at(created)=*effect;
    return created++;
}
bool SDLCALL SDL_StopHapticEffects(SDL_Haptic *) {
    ++stopped;
    if (fail_stop) return false;
    running.fill(false);
    return true;
}
void SDLCALL SDL_CloseHaptic(SDL_Haptic *) { ++closed; running.fill(false); }
bool SDLCALL SDL_RumbleJoystick(SDL_Joystick *,Uint16,Uint16,Uint32) { ++rumble; return true; }
bool SDLCALL SDL_UpdateHapticEffect(SDL_Haptic *,SDL_HapticEffectID id,const SDL_HapticEffect *effect) {
    ++updates;
    if ((fail_update && id==2) || reject_update_id==id) return false;
    effects.at(id)=*effect;
    return true;
}
bool SDLCALL SDL_RunHapticEffect(SDL_Haptic *,SDL_HapticEffectID id,Uint32 iterations) {
    require(iterations==1,"unexpected haptic iterations");
    ++starts;
    if (fail_start && id==2) return false;
    running.at(id)=true;
    return true;
}
}

int main() try {
    app::Devices devices;
    app::Controls controls;
    controls.bind[app::SteerRight].joy={app::JoyInput::Axis,"private-mock-wheel",0,0,32767};
    app::ForceFeedback ffb;

    fail_open=true;
    update(ffb,devices,controls,{0x07,0x57});
    require(opened==1 && quiet() && rumble==0,"failed wheel open generated output");
    now_ms=2999;
    update(ffb,devices,controls,{});
    require(opened==1,"early wheel reopen");
    fail_open=false;
    now_ms=3000;
    update(ffb,devices,controls,{});
    require(opened==2 && running[2] && effects[2].constant.level==32767,"wheel did not resume remembered pull");
    const auto accepted=updates;
    update(ffb,devices,controls,{});
    require(updates==accepted,"unchanged accepted effect was resent");

    fail_update=true;
    update(ffb,devices,controls,{0x01});
    require(quiet(),"failed update kept old wheel force active");
    const int starts_before_retry=starts;
    update(ffb,devices,controls,{});
    require(quiet() && starts==starts_before_retry,"persistent update failure restarted effects");
    fail_update=false;
    update(ffb,devices,controls,{});
    require(running[2] && effects[2].constant.level==0,"wheel did not recover neutral update");
    update(ffb,devices,controls,{0x07,0x57},true);
    require(effects[2].constant.level==-32767,"force inversion");

    ffb.stop();
    require(quiet(),"pause did not stop wheel");
    fail_start=true;
    update(ffb,devices,controls,{});
    require(quiet(),"partial wheel startup left effects active");
    fail_start=false;
    update(ffb,devices,controls,{});
    require(running[2] && effects[2].constant.level==32767,"start retry did not restore intended effect");

    // SDL retains the effect's parameters while the effect is stopped.
    ffb.stop();
    require(quiet(), "pause before signed effect update did not stop wheel");
    const float signed_unit = 1.0f / 32767.0f;
    ffb.update({}, devices, controls, signed_unit, true);
    require(running[2] && effects[2].constant.level == -1,
            "resume reused the stale positive force instead of sending -1");
    const int signed_update_count = updates;
    ffb.update({}, devices, controls, signed_unit, true);
    require(updates == signed_update_count, "unchanged signed effect was resent");
    update(ffb, devices, controls, {});

    // Constant force is running. A rejected spring update, then a rejected
    // friction update, must stop every effect; the next accepted frame
    // restarts all three at the board's current values.
    require(running[2] && effects[2].constant.level==32767, "constant force was not running");
    reject_update_id=0;
    update(ffb, devices, controls, {0x37});
    require(quiet(), "rejected spring update left an effect running");
    update(ffb, devices, controls, {});
    require(quiet(), "spring rejection restarted effects");
    reject_update_id=-1;
    update(ffb, devices, controls, {});
    require(running[0] && running[1] && running[2], "spring recovery did not restart all effects");
    require(effects[0].condition.left_sat[0]==44395 && effects[0].condition.right_sat[0]==44395
            && effects[0].condition.left_coeff[0]==32767 && effects[0].condition.deadband[0]==0x0500
            && effects[1].condition.left_coeff[0]==0 && effects[2].constant.level==0,
            "spring recovery applied the wrong effect values");
    update(ffb, devices, controls, {0x57});
    require(running[2] && effects[2].constant.level==32767, "constant force did not resume");
    reject_update_id=1;
    update(ffb, devices, controls, {0x27});
    require(quiet(), "rejected friction update left an effect running");
    update(ffb, devices, controls, {});
    require(quiet(), "friction rejection restarted effects");
    reject_update_id=-1;
    update(ffb, devices, controls, {});
    require(running[0] && running[1] && running[2], "friction recovery did not restart all effects");
    require(effects[0].condition.left_sat[0]==0 && effects[1].condition.left_coeff[0]==22197
            && effects[1].condition.right_coeff[0]==22197 && effects[2].constant.level==0,
            "friction recovery applied the wrong effect values");
    update(ffb, devices, controls, {0x57});
    require(running[2] && effects[2].constant.level==32767, "constant force did not resume after friction");

    // After stop(), a rejected update must not start any effect while it lasts.
    ffb.stop();
    require(quiet(), "stop before a rejected idle update left an effect running");
    reject_update_id=0;
    const int idle_starts=starts;
    update(ffb, devices, controls, {0x37});
    require(quiet() && starts==idle_starts, "rejected update started effects after stop");
    update(ffb, devices, controls, {});
    require(quiet() && starts==idle_starts, "rejected update kept starting effects after stop");
    reject_update_id=-1;
    update(ffb, devices, controls, {0x57});
    require(running[0] && running[1] && running[2] && effects[2].constant.level==32767,
            "effects did not resume after the idle rejection");

    fail_stop=true;
    const int prior_closes=closed;
    ffb.stop();
    require(quiet() && closed==prior_closes+1,"stop failure did not release haptic effects");
    fail_stop=false;
    now_ms=5999;
    update(ffb,devices,controls,{});
    require(opened==2 && quiet(),"wheel reopened before stop-failure backoff");
    now_ms=6000;
    update(ffb,devices,controls,{});
    require(opened==3 && running[2] && effects[2].constant.level==32767,
            "wheel did not recover after stop failure");
    require(rumble==0,"wheel received rumble");
    std::printf("wheel failure tests passed, %d updates, %d starts, %d stops\n",updates,starts,stopped);
    return 0;
} catch(const std::exception &error) {
    std::fprintf(stderr,"%s\n",error.what());
    return 1;
}
