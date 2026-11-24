#include "app/online_menu.h"
#include "app/i18n.h"
#include "app/i18n_mark.h"
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

// A label above its field, the field across the whole width.
bool text_field(const char *label, const char *id, char *buf, size_t size, ImGuiInputTextFlags flags = 0) {
    ImGui::TextWrapped("%s", label); // a long translation wraps instead of being cut
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputText(id, buf, size, flags);
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
    Action action = Stay;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.7f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const float width = std::min(view->Size.x - 32.f, 900.f * scale);
    const bool two_columns = width >= 640.f; // the room settings in two columns; one on a narrow window
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
    using State = RelayLink::State;
    const bool in_room = link && link->state() != State::Failed;

    if (!in_room) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", TX("Race over the Internet with up to 8 players, through a daytona-relay server."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 4.f * scale});
        if (text_field(TX("Your name"), "##name", name_, sizeof name_)) {
            cfg.online_name = online::clean_name(name_);
            cfg.save();
        }

        section(TX("Join a room"));
        text_field(TX("Invite code"), "##invite", invite_, sizeof invite_);
        const float half_w = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button(TX("Paste"), {half_w, button_h})) {
            if (const char *clip = ImGui::GetClipboardText()) copy_to(invite_, sizeof invite_, clip);
        }
        ImGui::SameLine();
        if (ImGui::Button(TX("Join"), {half_w, button_h})) {
            if (const auto invite = online::decode_invite(invite_)) {
                server_ = *online::normalize_server(invite->server);
                hello_ = {};
                hello_.op = online::Hello::Op::Join;
                hello_.room = invite->room;
                hello_.name = name_;
                hello_.password = invite->password;
                room_password_ = invite->password;
                error_.clear();
                copied_ = false;
                action = Connect;
            } else {
                error_ = TX("Invalid invite code: copy all of it, it starts with DTN1.");
            }
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", TX("The code already holds the server, the room and the password."));
        ImGui::PopTextWrapPos();

        // Create: connect to the server first (it answers its defaults), then
        // the room's settings, then the room
        section(TX("Create a room"));
        // the answer to Connect, if one is on its way
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
        const bool connected_now = !probed_.empty();

        // server, password and Connect on one row on a wide window (a table:
        // a row of labels, then a row of fields, so they line up); one under
        // the other otherwise. The password only once the server asks for one
        // (most servers have none: a room is private by its code).
        bool edited = false;
        auto server_input = [&](float w) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputText("##server", server_buf_, sizeof server_buf_)) {
                cfg.online_server = server_buf_;
                cfg.save();
                edited = true;
            }
        };
        auto password_input = [&](float w) {
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputText("##password", password_, sizeof password_, ImGuiInputTextFlags_Password)) {
                cfg.online_password = password_;
                cfg.save();
                edited = true;
            }
        };
        auto connect_button = [&](float w, float h) {
            ImGui::BeginDisabled(probe_ != nullptr);
            if (ImGui::Button(probe_ ? TX("Connecting...") : connected_now ? TX("Connect again") : TX("Connect"), {w, h})) {
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
            }
            ImGui::EndDisabled();
        };
        if (two_columns && ImGui::BeginTable("##server_row", ask_password_ ? 3 : 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##server", ImGuiTableColumnFlags_WidthStretch, ask_password_ ? 0.38f : 0.76f);
            if (ask_password_) ImGui::TableSetupColumn("##password", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("##connect", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(TX("Server (IP or DNS)"));
            if (ask_password_) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(TX("Server password"));
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            server_input(-FLT_MIN);
            if (ask_password_) {
                ImGui::TableNextColumn();
                password_input(-FLT_MIN);
            }
            ImGui::TableNextColumn();
            connect_button(-FLT_MIN, ImGui::GetFrameHeight());
            ImGui::EndTable();
        } else {
            ImGui::TextUnformatted(TX("Server (IP or DNS)"));
            server_input(-FLT_MIN);
            if (ask_password_) {
                ImGui::TextUnformatted(TX("Server password"));
                password_input(-FLT_MIN);
            }
            connect_button(full, button_h);
        }
        if (edited) { // another server: connect again
            probed_.clear();
            probe_.reset();
        }
        if (connected_now) ImGui::TextColored(good, TX("Connected to %s: choose the room's settings."), server_.c_str());
        else ImGui::TextDisabled("%s", TX("Connect to the server to choose the room's settings."));

        // the room's race settings, as every player's game will apply them
        const auto options = room_options();
        if (choice_.size() != options.size()) choice_.assign(options.size(), 0);
        ImGui::BeginDisabled(!connected_now);
        // a grid of "name [choice]": two settings a row on a wide window, one on a narrow one
        const int pairs = two_columns ? 2 : 1;
        if (ImGui::BeginTable("##room_settings", pairs * 2, ImGuiTableFlags_SizingFixedFit)) {
            for (int p = 0; p < pairs; ++p) {
                ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            }
            for (size_t i = 0; i < options.size(); ++i) {
                const auto &o = options[i];
                std::vector<const char *> labels;
                for (const auto &v : o.values) labels.push_back(v.label);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(o.label);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::PushID(o.key);
                ImGui::Combo("##value", &choice_[i], labels.data(), int(labels.size()));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", TX("Up to 8 players. You start the race once your friends are in; who joins "
                                     "later races the next one."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(TX("Create the room"), {full, button_h})) {
            hello_ = {};
            hello_.op = online::Hello::Op::Create;
            hello_.room = online::random_room();
            hello_.name = name_;
            hello_.password = password_;
            hello_.settings = room_settings();
            cfg.online_room = hello_.settings;
            cfg.save();
            room_password_ = password_;
            error_.clear();
            copied_ = false;
            action = Connect;
        }
        ImGui::EndDisabled();

        const std::string &problem = !error_.empty() ? error_ : link ? link->error() : message;
        if (!problem.empty()) {
            ImGui::Dummy({0, 4.f * scale});
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(warn, "%s", problem.c_str());
            ImGui::PopTextWrapPos();
        }
    } else if (link->state() == State::Connecting) {
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
        for (int i = 0; i < int(st.names.size()); ++i) {
            const bool me = i + 1 == st.me;
            char unnamed[64];
            std::snprintf(unnamed, sizeof unnamed, TX("Player %d"), i + 1);
            ImGui::TextColored(me ? good : ImVec4(0.91f, 0.97f, 1, 1), "%d. %s%s%s", i + 1,
                               st.names[i].empty() ? unnamed : st.names[i].c_str(), i == 0 ? TX(" (starts the race)") : "",
                               me ? TX(" (you)") : "");
        }
        if (!st.waiting.empty()) {
            ImGui::TextDisabled("%s", TX("Next race:"));
            for (const auto &name : st.waiting) ImGui::TextDisabled("   %s", name.empty() ? "-" : name.c_str());
        }
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
