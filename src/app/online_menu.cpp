#include "app/online_menu.h"
#include "app/i18n.h"
#include "app/i18n_mark.h"
#include "app/neon_ui.h"
#include "app/online_overlay.h"
#include "app/relay_link.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace app {

namespace {
void copy_to(char *dst, size_t size, const std::string &src) {
    std::snprintf(dst, size, "%s", src.c_str());
}

// The room's race settings: the server's keys and values (its package
// settings), in the language of the moment.
struct RoomValue {
    const char *name, *label;
};
struct RoomOption {
    const char *key, *label;
    std::vector<RoomValue> values;
};
std::vector<RoomOption> room_options() {
    return {
        {"cabinet", TXR("Cabinet"), {{"twin", TXR("Twin")}, {"uplight", TXR("Uplight")}, {"deluxe", TXR("Deluxe")}}},
        {"country", TXR("Country"),
         {{"usa", TXR("USA (English)")}, {"jpn", TXR("Japan (Japanese)")}, {"export", TXR("Export")}}},
        {"difficulty", TXR("Difficulty"),
         {{"easy", TXR("Easy")}, {"normal", TXR("Normal")}, {"hard", TXR("Hard")}, {"hardest", TXR("Hardest")}}},
        {"game_mode", TXR("Game mode"),
         {{"normal", TXR("Normal")}, {"grandprix", TXR("Grand Prix")}, {"endurance", TXR("Endurance")}}},
        {"rival_arrow", TXR("Rival arrow"), {{"on", TXR("On")}, {"off", TXR("Off")}}},
        {"advertise_sound", TXR("Advertise sound"), {{"on", TXR("On")}, {"off", TXR("Off")}}},
        {"free_play", TXR("Free play"), {{"on", TXR("On (Start only)")}, {"off", TXR("Off (coins)")}}},
        {"start", TXR("Who may start the race"), {{"master", TXR("The MASTER")}, {"anyone", TXR("Anyone")}}},
    };
}
} // namespace

OnlineMenu::OnlineMenu() = default;
OnlineMenu::~OnlineMenu() = default;

// The server's defaults, then what this player chose for their last room.
void OnlineMenu::connected(const Config &cfg, const std::vector<std::pair<std::string, std::string>> &defaults) {
    const auto options = room_options();
    choice_.assign(options.size(), 0);
    auto pick = [&](const std::string &key, const std::string &value) {
        for (size_t i = 0; i < options.size(); ++i) {
            if (key != options[i].key) continue;
            for (size_t v = 0; v < options[i].values.size(); ++v)
                if (value == options[i].values[v].name) choice_[i] = int(v);
        }
    };
    for (const auto &[key, value] : defaults) pick(key, value);
    std::string pair;
    for (size_t start = 0; start <= cfg.online_room.size();) {
        auto comma = cfg.online_room.find(',', start);
        if (comma == std::string::npos) comma = cfg.online_room.size();
        pair = cfg.online_room.substr(start, comma - start);
        if (const auto colon = pair.find(':'); colon != std::string::npos) pick(pair.substr(0, colon), pair.substr(colon + 1));
        start = comma + 1;
    }
}

std::string OnlineMenu::room_settings() const {
    const auto options = room_options();
    std::string out;
    for (size_t i = 0; i < options.size() && i < choice_.size(); ++i) {
        if (!out.empty()) out += ',';
        out += std::string(options[i].key) + ':' + options[i].values[size_t(choice_[i])].name;
    }
    return out;
}

void OnlineMenu::load(const Config &cfg) {
    copy_to(name_, sizeof name_, cfg.online_name);
    copy_to(server_buf_, sizeof server_buf_, cfg.online_server);
    copy_to(password_, sizeof password_, cfg.online_password);
    ask_password_ = !cfg.online_password.empty();
    init_ = true;
}

OnlineMenu::Action OnlineMenu::draw_settings(Config &cfg, const RelayLink *link, const std::string &message) {
    using namespace neon;
    using State = RelayLink::State;
    Action action = Stay;
    // the answer to Connect (the server's defaults), if one is on its way
    if (probe_) {
        probe_->poll();
        if (probe_->state() == State::Info) {
            probed_ = server_ + "\n" + password_;
            connected(cfg, probe_->info());
            probe_.reset();
        } else if (probe_->state() == State::Failed) {
            error_ = probe_->error();
            if (probe_->error_code() == "password") {
                if (!ask_password_) error_ = TX("This server asks for a password.");
                ask_password_ = true;
            }
            probe_.reset();
        }
    }
    auto connect = [&] {
        if (const auto server = online::normalize_server(server_buf_)) {
            server_ = *server;
            online::Hello info;
            info.op = online::Hello::Op::Info;
            info.name = name_;
            info.password = password_;
            probed_.clear();
            error_.clear();
            probe_ = std::make_unique<RelayLink>(server_, info);
        } else {
            error_ = TX("Invalid server address: an IP or name, with :port if it is not 7777.");
        }
    };
    // no server without a name: the room's players see it
    const bool named_before = !online::clean_name(name_).empty();
    if (!auto_connect_) { // opened with a name and a saved server: ask it at once
        auto_connect_ = true;
        if (named_before && probed_.empty() && !probe_ && *server_buf_) connect();
    }
    const bool connected_now = !probed_.empty();
    const auto options = room_options();
    if (choice_.size() != options.size()) choice_.assign(options.size(), 0);

    const auto *view = ImGui::GetMainViewport();
    const float w = view->Size.x, h = view->Size.y;
    const float scale = std::max(0.13f, std::min(h / 900.f, w / 980.f));
    ImFont *body = body_ ? body_ : ImGui::GetFont(), *display = display_ ? display_ : body;
    const ImU32 white = rgba(0.97f, 0.98f, 1), muted = rgba(0.70f, 0.78f, 0.90f), cyan = rgba(0.38f, 0.80f, 1);
    const ImU32 green = rgba(0.25f, 0.88f, 0.48f), amber = rgba(1, 0.80f, 0.28f), alarm = rgba(1, 0.42f, 0.38f);
    const float row = 44 * scale, step = 52 * scale, option_h = 38 * scale, option_step = 44 * scale;
    const float pw = std::min(w - 32.f, 720 * scale);
    const float ph = (748 + (ask_password_ ? 52 : 0)) * scale;
    const float x0 = view->Pos.x + (w - pw) * 0.5f, y0 = view->Pos.y + std::max(16.f, (h - ph) * 0.5f);

    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##room_settings_screen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(view->Pos, {view->Pos.x + w, view->Pos.y + h}, rgba(0.01f, 0.03f, 0.08f, 0.55f)); // the scene, dimmed
    const auto panel = rounded({x0, y0}, {x0 + pw, y0 + ph}, 0, 16 * scale);
    fill(draw, panel, rgba(0.05f, 0.11f, 0.25f, 0.97f), rgba(0.02f, 0.05f, 0.12f, 0.97f), y0, y0 + ph);
    glow(draw, panel, kBlue, 0.9f, scale);
    const float pad = 28 * scale, lx = x0 + pad, inner = pw - 2 * pad, label_w = 190 * scale;
    const float fx = lx + label_w, fw = inner - label_w;
    auto label = [&](const char *text, float y, float height) {
        const float s = fit(body, 20 * scale, text, label_w - 12 * scale);
        italic(draw, body, s, {lx, y + (height - s) * 0.5f}, white, text, 0.14f);
    };

    // title: a gear in a badge
    float y = y0 + 22 * scale;
    const ImVec2 badge = {lx + 22 * scale, y + 22 * scale};
    draw->AddCircleFilled(badge, 22 * scale, rgba(0.02f, 0.06f, 0.15f, 0.95f), 32);
    draw->AddCircle(badge, 22 * scale, cyan, 32, 2 * scale);
    gear(draw, badge, 26 * scale, white, rgba(0.02f, 0.06f, 0.15f));
    italic(draw, display, fit(display, 32 * scale, TX("Room settings"), inner - 60 * scale), {lx + 58 * scale, y + 4 * scale}, white,
           TX("Room settings"), 0.18f);
    y += 54 * scale;
    draw->AddText(body, 15 * scale, {lx, y}, muted,
                  TX("Every player of the room races with these settings. They are kept for your next room."), nullptr, inner);
    y += 36 * scale;

    // the player's name: the room's players see it (above the car, in the lists)
    label(TX("Your name"), y, row);
    if (field(draw, body, scale, "##name", name_, sizeof name_, {fx, y}, {fx + fw, y + row})) {
        cfg.online_name = online::clean_name(name_);
        cfg.save();
    }
    const bool named = !online::clean_name(name_).empty();
    if (!named) ImGui::SetItemDefaultFocus();
    y += step;

    // the server (and its password, once it asks for one), and Connect
    bool edited = false;
    const float connect_w = 150 * scale;
    label(TX("Server (IP or DNS)"), y, row);
    if (field(draw, body, scale, "##server", server_buf_, sizeof server_buf_, {fx, y}, {fx + fw - connect_w - 10 * scale, y + row})) {
        cfg.online_server = server_buf_;
        cfg.save();
        edited = true;
    }
    ImGui::BeginDisabled(probe_ != nullptr || !named);
    const char *connect_label = probe_ ? TX("Connecting...") : connected_now ? TX("Connect again") : TX("Connect");
    if (blue_button(draw, body, scale, "##connect", connect_label, {fx + fw - connect_w, y}, {fx + fw, y + row}, 0, 19)) connect();
    if (named && !connected_now) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    y += step;
    if (ask_password_) {
        label(TX("Server password"), y, row);
        if (field(draw, body, scale, "##password", password_, sizeof password_, {fx, y}, {fx + fw, y + row},
                  ImGuiInputTextFlags_Password)) {
            cfg.online_password = password_;
            cfg.save();
            edited = true;
        }
        y += step;
    }
    if (edited) { // another server: connect again
        probed_.clear();
        probe_.reset();
    }
    {
        char status[256];
        ImU32 col = muted;
        const std::string &problem = !error_.empty() ? error_ : link ? link->error() : message;
        if (probe_) std::snprintf(status, sizeof status, TX("Connecting to %s..."), server_.c_str()), col = amber;
        else if (!problem.empty()) std::snprintf(status, sizeof status, "%s", problem.c_str()), col = alarm;
        else if (!named) std::snprintf(status, sizeof status, "%s", TX("Type your name above to connect to the server.")), col = amber;
        else if (connected_now) std::snprintf(status, sizeof status, TX("Connected to %s: choose the room's settings."), server_.c_str()), col = green;
        else std::snprintf(status, sizeof status, "%s", TX("Connect to the server to choose the room's settings."));
        draw->AddText(body, 14.5f * scale, {fx + 2 * scale, y - 2 * scale}, col, status, nullptr, fw);
    }
    y += 34 * scale;

    // the race settings: each a row of choices that light up
    {
        const char *title = TX("Race settings");
        const float s = 18 * scale;
        italic(draw, body, s, {lx, y}, cyan, title, 0.14f);
        const float tw = measure(body, s, title).x;
        draw->AddLine({lx + tw + 16 * scale, y + s * 0.6f}, {lx + inner, y + s * 0.6f}, rgba(0.5f, 0.75f, 1, 0.3f), 1 * scale);
    }
    y += 34 * scale;
    ImGui::BeginDisabled(!connected_now);
    for (size_t i = 0; i < options.size(); ++i) {
        const auto &o = options[i];
        label(o.label, y, option_h);
        const int n = int(o.values.size());
        const float gap = 8 * scale, cw = (fw - (n - 1) * gap) / n;
        ImGui::PushID(o.key);
        for (int v = 0; v < n; ++v) {
            const float cx = fx + v * (cw + gap);
            if (choice(draw, body, scale, o.values[size_t(v)].name, o.values[size_t(v)].label, {cx, y}, {cx + cw, y + option_h},
                       choice_[i] == v, 19)) {
                choice_[i] = v;
                cfg.online_room = room_settings(); // the home screen's Create uses them
                cfg.save();
            }
        }
        ImGui::PopID();
        y += option_step;
    }
    ImGui::EndDisabled();

    // Create the room, and Back
    const float by = y0 + ph - pad - 64 * scale, back_w = 180 * scale;
    ImGui::BeginDisabled(!connected_now || !named);
    if (main_button(draw, body, scale, TX("Create the room"), {lx, by}, {lx + inner - back_w - 14 * scale, by + 64 * scale}, true, 36)) {
        hello_ = {};
        hello_.op = online::Hello::Op::Create;
        hello_.room = online::random_room();
        hello_.name = online::clean_name(name_);
        hello_.password = password_;
        hello_.settings = room_settings();
        cfg.online_room = hello_.settings;
        cfg.save();
        room_password_ = password_;
        error_.clear();
        copied_ = false;
        action = Connect;
    }
    if (connected_now) ImGui::SetItemDefaultFocus();
    ImGui::EndDisabled();
    if (main_button(draw, body, scale, TX("Back"), {lx + inner - back_w, by}, {lx + inner, by + 64 * scale}, false, 28)) action = Back;
    if (action == Stay && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) action = Back;
    ImGui::End();
    ImGui::PopStyleVar();
    return action;
}

OnlineMenu::Action OnlineMenu::draw(Config &cfg, const RelayLink *link, bool racing, const std::string &message) {
    if (!init_) {
        load(cfg);
        // diagnostic: M2_CAPTURE_ONLINE_CONNECTED=1 shows the room settings as after Connect, for a --capture-home image
        if (const char *v = std::getenv("M2_CAPTURE_ONLINE_CONNECTED"); v && *v == '1') {
            if (!*name_) copy_to(name_, sizeof name_, "Alex"); // shown only, not saved
            if (!*server_buf_) copy_to(server_buf_, sizeof server_buf_, "relay-eu.example");
            server_ = online::normalize_server(server_buf_).value_or("127.0.0.1:7777");
            probed_ = server_;
            connected(cfg, {{"cabinet", "twin"}, {"difficulty", "hard"}});
        }
    }
    using State = RelayLink::State;
    // diagnostic: M2_CAPTURE_ONLINE_ROOM=1 (before a race) or =race shows the room's details with
    // example players, for a --capture-home image
    if (const char *v = std::getenv("M2_CAPTURE_ONLINE_ROOM"); v && *v && !link) {
        online::Status st;
        const bool race = !std::strcmp(v, "race");
        st.joined = 4;
        st.names = {"Alex", "Ana", "Bruno"};
        st.latency = {32, 48, 110};
        st.waiting = {"Carla"};
        st.waiting_latency = {190};
        st.owner = true;
        st.me = 1;
        st.racing = race;
        st.position = race ? 1 : 0;
        st.size = race ? 3 : 0;
        static const std::string code = online::encode_invite({"relay-eu.example:7777", online::random_room(), ""});
        return draw_room(st, code, false, "relay-eu.example:7777", race, race, message);
    }
    if (!link || link->state() == State::Failed) return draw_settings(cfg, link, message);
    const std::string code = online::encode_invite({link->server(), link->room(), room_password_});
    return draw_room(link->status(), code, link->state() == State::Connecting, link->server(), racing,
                     link->state() == State::Ready, message);
}

OnlineMenu::Action OnlineMenu::draw_room(const online::Status &st, const std::string &code, bool connecting,
                                         const std::string &server, bool racing, bool link_ready,
                                         const std::string &message) {
    using namespace neon;
    Action action = Stay;
    const auto *view = ImGui::GetMainViewport();
    const float w = view->Size.x, h = view->Size.y;
    const float scale = std::max(0.13f, std::min(h / 900.f, w / 980.f));
    ImFont *body = body_ ? body_ : ImGui::GetFont(), *display = display_ ? display_ : body;
    const ImU32 white = rgba(0.97f, 0.98f, 1), muted = rgba(0.70f, 0.78f, 0.90f), cyan = rgba(0.38f, 0.80f, 1);
    const ImU32 green = rgba(0.25f, 0.88f, 0.48f), amber = rgba(1, 0.80f, 0.28f), alarm = rgba(1, 0.42f, 0.38f);
    const float row = 40 * scale, step = 46 * scale, pad = 28 * scale;
    const int racers = int(st.names.size()), waiting = int(st.waiting.size());
    const bool me_in_race = st.position > 0;
    const bool can_resume = racing && link_ready && me_in_race;
    const bool new_race = st.owner && st.racing; // a new race with everyone (who waits too)

    // its height from what it lists
    float content = 54 + 34; // title, state
    if (connecting) content += 90;
    else content += racers * 46 + (waiting ? 34 + waiting * 46 : 0) + 30 + (new_race ? 56 : 0);
    if (!message.empty()) content += 28;
    const float pw = std::min(w - 32.f, 760 * scale);
    const float ph = std::min(h - 32.f, (content + 22 + 26 + 64 + 28) * scale);
    const float x0 = view->Pos.x + (w - pw) * 0.5f, y0 = view->Pos.y + std::max(16.f, (h - ph) * 0.5f);

    ImGui::SetNextWindowPos(view->Pos);
    ImGui::SetNextWindowSize(view->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##room_details_screen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(view->Pos, {view->Pos.x + w, view->Pos.y + h}, rgba(0.01f, 0.03f, 0.08f, racing ? 0.70f : 0.55f));
    const auto panel = rounded({x0, y0}, {x0 + pw, y0 + ph}, 0, 16 * scale);
    fill(draw, panel, rgba(0.05f, 0.11f, 0.25f, 0.97f), rgba(0.02f, 0.05f, 0.12f, 0.97f), y0, y0 + ph);
    glow(draw, panel, kBlue, 0.9f, scale);
    const float lx = x0 + pad, inner = pw - 2 * pad;

    // title: the players' badge, the room's count; its code and Copy at the right
    float y = y0 + 22 * scale;
    {
        const ImVec2 badge = {lx + 22 * scale, y + 22 * scale};
        draw->AddCircleFilled(badge, 22 * scale, rgba(0.02f, 0.06f, 0.15f, 0.95f), 32);
        draw->AddCircle(badge, 22 * scale, cyan, 32, 2 * scale);
        const ImU32 back = rgba(0.30f, 0.70f, 0.95f);
        draw->AddCircleFilled({badge.x + 6 * scale, badge.y - 6 * scale}, 5 * scale, back);
        draw->PathArcTo({badge.x + 6 * scale, badge.y + 8 * scale}, 8 * scale, IM_PI, 2 * IM_PI, 12);
        draw->PathFillConvex(back);
        draw->AddCircleFilled({badge.x - 4 * scale, badge.y - 4 * scale}, 6 * scale, cyan);
        draw->PathArcTo({badge.x - 4 * scale, badge.y + 12 * scale}, 10 * scale, IM_PI, 2 * IM_PI, 12);
        draw->PathFillConvex(cyan);
        char title[64];
        if (connecting) std::snprintf(title, sizeof title, "%s", TX("Room"));
        else std::snprintf(title, sizeof title, TX("Room  %d / %d"), st.joined, online::kMaxPlayers);
        italic(draw, display, fit(display, 32 * scale, title, inner * 0.45f), {lx + 58 * scale, y + 4 * scale}, white, title, 0.18f);
        if (!connecting && !code.empty()) {
            const float copy_w = 110 * scale;
            if (blue_button(draw, body, scale, "##copy_code", copied_ ? TX("Copied!") : TX("Copy"), {lx + inner - copy_w, y + 4 * scale},
                            {lx + inner, y + 42 * scale}, 0, 18)) {
                ImGui::SetClipboardText(code.c_str());
                copied_ = true;
            }
            // the code, its start and end when long
            const float room = inner * 0.5f - copy_w - 16 * scale, s = 15.5f * scale;
            std::string shown = code;
            if (measure(body, s, shown.c_str()).x > room && shown.size() > 16) {
                const auto dash = shown.rfind('-');
                const std::string tail = "..." + shown.substr(dash + 1);
                std::string head = shown.substr(0, dash);
                while (head.size() > 6 && measure(body, s, (head + tail).c_str()).x > room) {
                    const auto d = head.rfind('-');
                    head.resize(d != std::string::npos && d > 4 ? d : head.size() - 1);
                }
                shown = head + tail;
            }
            const ImVec2 cw = measure(body, s, shown.c_str());
            draw->AddText(body, s, {lx + inner - copy_w - 14 * scale - cw.x, y + 23 * scale - s * 0.5f}, cyan, shown.c_str());
        }
    }
    y += 54 * scale;

    // what the room waits for
    {
        char state[192];
        ImU32 col = amber;
        const std::string owner_name = st.names.empty() || st.names[0].empty() ? TX("the room's owner") : st.names[0];
        if (connecting) std::snprintf(state, sizeof state, TX("Connecting to %s..."), server.c_str());
        else if (!st.racing) {
            if (st.owner)
                std::snprintf(state, sizeof state, "%s", st.joined < online::kMinPlayers ? TX("Waiting for at least one more player.")
                                                                                       : TX("Start the race when everyone is in."));
            else std::snprintf(state, sizeof state, TX("Waiting for %s to start the race."), owner_name.c_str());
        } else if (!me_in_race) {
            std::snprintf(state, sizeof state, "%s", TX("A race is on: you will join the next one."));
        } else {
            std::snprintf(state, sizeof state, "%s", racing ? TX("Online race in progress.") : TX("Starting the race..."));
            col = green;
        }
        draw->AddText(body, 17 * scale, {lx, y}, col, state, nullptr, inner);
    }
    y += 34 * scale;

    if (connecting) {
        const int dots = int(ImGui::GetTime() * 3) % 4;
        for (int i = 0; i < dots; ++i) draw->AddCircleFilled({lx + (8 + i * 16) * scale, y + 20 * scale}, 5 * scale, cyan);
        y += 90 * scale;
    } else {
        // one row a player: the car's colour and number, the name, the latency
        auto player = [&](int index, const std::string &name, int ms, bool racer, bool you, bool crowned) {
            const ImVec2 a = {lx, y}, b = {lx + inner, y + row};
            const auto box = rounded(a, b, 0, 8 * scale);
            fill(draw, box, rgba(0.03f, 0.07f, 0.16f, 0.85f), rgba(0.02f, 0.05f, 0.12f, 0.85f), a.y, b.y);
            if (you) draw->AddPolyline(box.data(), int(box.size()), rgba(0.55f, 0.85f, 1, 0.8f), ImDrawFlags_Closed, 1.6f * scale);
            const uint32_t c = racer ? online_car_colour(index + 1) : 0x50607a;
            const ImU32 chip = rgba(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f);
            const auto tag = rounded({a.x + 6 * scale, a.y + 6 * scale}, {a.x + 58 * scale, b.y - 6 * scale}, 5 * scale, 4 * scale);
            fill(draw, tag, chip, chip, a.y, b.y);
            const float luma = (((c >> 16) & 255) * 0.3f + ((c >> 8) & 255) * 0.59f + (c & 255) * 0.11f) / 255.f;
            const ImU32 ink = luma > 0.6f ? rgba(0.06f, 0.05f, 0.03f) : white;
            char n[8];
            std::snprintf(n, sizeof n, racer ? "%d" : "-", index + 1);
            const float ns = 17 * scale;
            const ImVec2 nw = measure(body, ns, n);
            float nx = a.x + 32 * scale - nw.x * 0.5f;
            if (crowned) { // the owner's crown beside the number
                const float cx = nx - 9 * scale, cy = (a.y + b.y) * 0.5f, s = 11 * scale;
                const ImVec2 p[] = {{cx - s * 0.45f, cy + s * 0.3f}, {cx - s * 0.45f, cy - s * 0.25f}, {cx - s * 0.18f, cy + s * 0.02f},
                                    {cx, cy - s * 0.35f}, {cx + s * 0.18f, cy + s * 0.02f}, {cx + s * 0.45f, cy - s * 0.25f},
                                    {cx + s * 0.45f, cy + s * 0.3f}};
                draw->AddConcavePolyFilled(p, 7, ink);
                nx += 6 * scale;
            }
            draw->AddText(body, ns, {nx, (a.y + b.y - ns) * 0.5f}, ink, n);
            char unnamed[32];
            std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), index + 1);
            const char *shown = name.empty() ? unnamed : name.c_str();
            const float ts = 19 * scale;
            italic(draw, body, ts, {a.x + 74 * scale, (a.y + b.y - ts) * 0.5f}, racer ? white : muted, shown, 0.14f);
            if (you) {
                const float sx = a.x + 74 * scale + measure(body, ts, shown).x + 12 * scale;
                draw->AddText(body, 15 * scale, {sx, (a.y + b.y) * 0.5f - 8 * scale}, cyan, TX("(you)"));
            }
            char lat[24];
            if (ms >= 0) std::snprintf(lat, sizeof lat, TX("%d ms"), ms);
            else std::snprintf(lat, sizeof lat, "--");
            const ImU32 lc = ms < 0 ? muted : ms <= 60 ? green : ms <= 120 ? amber : alarm;
            const ImVec2 lw = measure(body, 17 * scale, lat);
            draw->AddText(body, 17 * scale, {b.x - 14 * scale - lw.x, (a.y + b.y) * 0.5f - 9 * scale}, lc, lat);
            signal(draw, {b.x - 30 * scale - 76 * scale, (a.y + b.y) * 0.5f + 8 * scale}, scale, ms);
            y += step;
        };
        for (int i = 0; i < racers; ++i)
            player(i, st.names[size_t(i)], i < int(st.latency.size()) ? st.latency[size_t(i)] : -1, true, i + 1 == st.me, i == 0);
        if (waiting) {
            italic(draw, body, 17 * scale, {lx, y + 4 * scale}, cyan, TX("Next race:"), 0.14f);
            y += 34 * scale;
            for (int i = 0; i < waiting; ++i)
                player(i, st.waiting[size_t(i)], i < int(st.waiting_latency.size()) ? st.waiting_latency[size_t(i)] : -1, false,
                       false, false); // which of them is this player: the server does not say
        }
        draw->AddText(body, 14 * scale, {lx, y}, muted, TX("Latency to the server, round trip; between two players it is about the sum."),
                      nullptr, inner);
        y += 30 * scale;
        if (new_race) {
            if (blue_button(draw, body, scale, TX("New race with everyone"), TX("New race with everyone"), {lx, y},
                            {lx + inner, y + 44 * scale}, 0, 19))
                action = Begin;
            y += 56 * scale;
        }
    }
    if (!message.empty()) draw->AddText(body, 15 * scale, {lx, y}, amber, message.c_str(), nullptr, inner);

    // the call to action, Leave, Back
    const float by = y0 + ph - pad - 64 * scale, back_w = 160 * scale, leave_w = 190 * scale, gap = 12 * scale;
    const ImVec2 ma = {lx, by}, mb = {lx + inner - back_w - leave_w - 2 * gap, by + 64 * scale};
    if (connecting) {
        main_button(draw, body, scale, TX("Connecting..."), ma, mb, false, 28);
    } else if (can_resume) {
        if (main_button(draw, body, scale, TX("Resume the race"), ma, mb, true, 32)) action = Resume;
    } else if (st.owner && !st.racing) {
        ImGui::BeginDisabled(st.joined < online::kMinPlayers);
        if (main_button(draw, body, scale, TX("Start the race"), ma, mb, true, 32) && st.joined >= online::kMinPlayers) action = Begin;
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled();
        main_button(draw, body, scale, TX("Waiting for the race"), ma, mb, false, 26);
        ImGui::EndDisabled();
    }
    ImGui::SetItemDefaultFocus();
    if (outline_button(draw, body, scale, connecting ? TX("Cancel") : TX("Leave the room"), {mb.x + gap, by},
                       {mb.x + gap + leave_w, by + 64 * scale}, ImVec4(1, 0.36f, 0.36f, 1), 20))
        action = Leave;
    if (main_button(draw, body, scale, TX("Back"), {lx + inner - back_w, by}, {lx + inner, by + 64 * scale}, false, 26)) action = Back;
    if (action == Stay && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) action = can_resume ? Resume : Back;
    ImGui::End();
    ImGui::PopStyleVar();
    return action;
}

} // namespace app
