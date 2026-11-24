#include "app/online_menu.h"
#include "app/i18n.h"
#include "app/i18n_mark.h"
#include "app/neon_ui.h"
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
            server_ = online::normalize_server(server_buf_).value_or("127.0.0.1:7777");
            probed_ = server_;
            connected(cfg, {{"cabinet", "twin"}, {"difficulty", "hard"}});
        }
    }
    using State = RelayLink::State;
    if (!link || link->state() == State::Failed) return draw_settings(cfg, link, message);
    Action action = Stay;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.7f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const float width = std::min(view->Size.x - 32.f, 900.f * scale);
    // as tall as its content, up to the screen (then it scrolls)
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, view->Size.y - 32.f});
    ImGui::SetNextWindowPos({view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::PushFont(nullptr, 21.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.f * scale, 20.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 28.f * scale);
    ImGui::Begin(TX("Online##online"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const ImVec4 warn(1, 0.74f, 0.45f, 1), good(0.48f, 0.98f, 0.79f, 1);
    const float full = ImGui::GetContentRegionAvail().x, button_h = 48.f * scale;
    auto section = [&](const char *title) {
        ImGui::Dummy({0, 6.f * scale});
        ImGui::SeparatorText(title);
    };

    if (link->state() == State::Connecting) {
        ImGui::Text(TX("Connecting to %s..."), link->server().c_str());
        ImGui::Dummy({0, 4.f * scale});
        if (ImGui::Button(TX("Cancel"), {full, button_h})) action = Leave;
    } else {
        const auto &st = link->status();
        ImGui::Text(TX("Room %s: %d / %d players"), link->room().c_str(), st.joined, online::kMaxPlayers);

        section(TX("Invite code"));
        // read-only field: the whole code shows, and it can be selected as well as copied
        std::string code = online::encode_invite({link->server(), link->room(), room_password_});
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##code", code.data(), code.size() + 1, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(copied_ ? TX("Copied!") : TX("Copy the code"), {full, button_h})) {
            ImGui::SetClipboardText(code.c_str());
            copied_ = true;
        }
        ImGui::TextDisabled("%s", TX("Send it to your friends: they paste it under Join."));

        // the players: the race's (car 1, 2...) or, before one, the order a race
        // will take (the room's owner first); then who waits for the next race
        section(TX("Players"));
        const bool me_in_race = st.position > 0;
        // each player's latency to the server, at the right: green, yellow, red
        const float latency_x = ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("9999 ms").x;
        auto latency = [&](const std::vector<int> &list, size_t i) {
            const int ms = i < list.size() ? list[i] : -1;
            ImGui::SameLine(latency_x);
            if (ms < 0) {
                ImGui::TextDisabled("--");
                return;
            }
            const ImVec4 colour = ms <= 60 ? good : ms <= 120 ? ImVec4(1, 0.85f, 0.35f, 1) : ImVec4(1, 0.42f, 0.38f, 1);
            ImGui::TextColored(colour, TX("%d ms"), ms);
        };
        for (int i = 0; i < int(st.names.size()); ++i) {
            const bool me = i + 1 == st.me;
            char unnamed[64];
            std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), i + 1);
            ImGui::TextColored(me ? good : ImVec4(0.91f, 0.97f, 1, 1), "%d. %s%s%s", i + 1,
                               st.names[i].empty() ? unnamed : st.names[i].c_str(), i == 0 ? TX(" (starts the race)") : "",
                               me ? TX(" (you)") : "");
            latency(st.latency, size_t(i));
        }
        if (!st.waiting.empty()) {
            ImGui::TextDisabled("%s", TX("Next race:"));
            for (size_t i = 0; i < st.waiting.size(); ++i) {
                ImGui::TextDisabled("   %s", st.waiting[i].empty() ? "-" : st.waiting[i].c_str());
                latency(st.waiting_latency, i);
            }
        }
        if (!st.latency.empty())
            ImGui::TextDisabled("%s", TX("Latency to the server, round trip; between two players it is about the sum."));
        ImGui::Dummy({0, 4.f * scale});
        ImGui::PushTextWrapPos(0);
        if (!st.racing) {
            if (st.owner)
                ImGui::TextColored(warn, "%s", st.joined < online::kMinPlayers
                                                   ? TX("Waiting for at least one more player.")
                                                   : TX("Start the race when everyone is in."));
            else
                ImGui::TextColored(warn, TX("Waiting for %s to start the race."),
                                   st.names.empty() || st.names[0].empty() ? TX("the room's owner") : st.names[0].c_str());
        } else if (!me_in_race) {
            ImGui::TextColored(warn, "%s", TX("A race is on: you will join the next one."));
        } else {
            ImGui::TextColored(good, "%s", racing ? TX("Online race in progress.") : TX("Starting the race..."));
            ImGui::TextUnformatted(TX("Press Start (Enter) to play. Esc comes back to this menu."));
        }
        if (!message.empty()) ImGui::TextColored(warn, "%s", message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 4.f * scale});
        if (st.owner) { // a race with everyone in the room; during one, a new race with whoever is waiting too
            ImGui::BeginDisabled(st.joined < online::kMinPlayers);
            if (ImGui::Button(st.racing ? TX("New race with everyone") : TX("Start the race"), {full, button_h}))
                action = Begin;
            ImGui::EndDisabled();
            if (st.racing) ImGui::TextDisabled("%s", TX("Restarts every player's game with everyone in the room."));
        }
        if (racing && link->state() == State::Ready && ImGui::Button(TX("Resume the race"), {full, button_h}))
            action = Resume;
        if (ImGui::Button(TX("Leave the room"), {full, button_h})) action = Leave;
    }
    ImGui::Dummy({0, 6.f * scale});
    ImGui::Separator();
    if (ImGui::Button(TX("Back"), {full, button_h})) action = Back;
    if (action == Stay && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        action = racing && link && link->state() == RelayLink::State::Ready ? Resume : Back;
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
    return action;
}

} // namespace app
