#include "app/online_menu.h"
#include "app/i18n.h"
#include "app/relay_link.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace app {

namespace {
void copy_to(char *dst, size_t size, const std::string &src) {
    std::snprintf(dst, size, "%s", src.c_str());
}

// A label above its field, the field across the whole width.
bool text_field(const char *label, const char *id, char *buf, size_t size, ImGuiInputTextFlags flags = 0) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputText(id, buf, size, flags);
}
} // namespace

void OnlineMenu::load(const Config &cfg) {
    copy_to(name_, sizeof name_, cfg.online_name);
    copy_to(server_buf_, sizeof server_buf_, cfg.online_server);
    copy_to(password_, sizeof password_, cfg.online_password);
    init_ = true;
}

OnlineMenu::Action OnlineMenu::draw(Config &cfg, const RelayLink *link, bool racing, const std::string &message) {
    if (!init_) load(cfg);
    Action action = Stay;
    const auto *view = ImGui::GetMainViewport();
    const float scale = std::max(0.7f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const float width = std::min(view->Size.x - 32.f, 900.f * scale);
    const bool two_columns = width >= 640.f; // create and join side by side; one column on a narrow window
    // as tall as its content, up to the screen (then it scrolls)
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, view->Size.y - 32.f});
    ImGui::SetNextWindowPos({view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::PushFont(nullptr, 21.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.f * scale, 20.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.f * scale, 10.f * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 28.f * scale);
    ImGui::Begin(TX("En ligne##online"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
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
        ImGui::TextDisabled("%s", TX("Jouez en réseau par Internet avec jusqu'à 8 joueurs, à travers un serveur daytona-relay."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 4.f * scale});
        if (text_field(TX("Votre nom"), "##name", name_, sizeof name_)) {
            cfg.online_name = online::clean_name(name_);
            cfg.save();
        }
        auto create = [&] {
            ImGui::SeparatorText(TX("Créer une salle"));
            if (text_field(TX("Serveur (IP ou DNS)"), "##server", server_buf_, sizeof server_buf_)) {
                cfg.online_server = server_buf_;
                cfg.save();
            }
            if (text_field(TX("Mot de passe du serveur (facultatif)"), "##password", password_, sizeof password_,
                           ImGuiInputTextFlags_Password)) {
                cfg.online_password = password_;
                cfg.save();
            }
            ImGui::TextUnformatted(TX("Joueurs"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##players", &cfg.online_players, online::kMinPlayers, online::kMaxPlayers)) cfg.save();
            if (ImGui::Button(TX("Créer la salle"), {-FLT_MIN, button_h})) {
                if (const auto server = online::normalize_server(server_buf_)) {
                    server_ = *server;
                    hello_ = {};
                    hello_.op = online::Hello::Op::Create;
                    hello_.room = online::random_room();
                    hello_.players = cfg.online_players;
                    hello_.name = name_;
                    hello_.password = password_;
                    room_password_ = password_;
                    error_.clear();
                    copied_ = false;
                    action = Connect;
                } else {
                    error_ = TX("Adresse du serveur invalide : IP ou nom, avec :port si ce n'est pas 7777.");
                }
            }
        };
        auto join = [&] {
            ImGui::SeparatorText(TX("Rejoindre une salle"));
            text_field(TX("Code d'invitation"), "##invite", invite_, sizeof invite_);
            const float avail = ImGui::GetContentRegionAvail().x;
            const float half_w = (avail - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button(TX("Coller"), {half_w, button_h})) {
                if (const char *clip = ImGui::GetClipboardText()) copy_to(invite_, sizeof invite_, clip);
            }
            ImGui::SameLine();
            if (ImGui::Button(TX("Rejoindre"), {half_w, button_h})) {
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
                    error_ = TX("Code d'invitation invalide : copiez-le en entier, il commence par DTN1.");
                }
            }
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", TX("Le code contient déjà le serveur, la salle et le mot de passe."));
            ImGui::PopTextWrapPos();
        };
        ImGui::Dummy({0, 6.f * scale});
        if (two_columns && ImGui::BeginTable("##online_columns", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableNextColumn();
            create();
            ImGui::TableNextColumn();
            join();
            ImGui::EndTable();
        } else {
            create();
            ImGui::Dummy({0, 6.f * scale});
            join();
        }
        const std::string &problem = !error_.empty() ? error_ : link ? link->error() : message;
        if (!problem.empty()) {
            ImGui::Dummy({0, 4.f * scale});
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(warn, "%s", problem.c_str());
            ImGui::PopTextWrapPos();
        }
    } else if (link->state() == State::Connecting) {
        ImGui::Text(TX("Connexion à %s..."), link->server().c_str());
        ImGui::Dummy({0, 4.f * scale});
        if (ImGui::Button(TX("Annuler"), {full, button_h})) action = Leave;
    } else {
        const auto &st = link->status();
        ImGui::Text(TX("Salle %s : %d / %d joueurs"), link->room().c_str(), st.joined, st.size);

        section(TX("Code d'invitation"));
        // read-only field: the whole code shows, and it can be selected as well as copied
        std::string code = online::encode_invite({link->server(), link->room(), room_password_});
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##code", code.data(), code.size() + 1, ImGuiInputTextFlags_ReadOnly);
        if (ImGui::Button(copied_ ? TX("Copié !") : TX("Copier le code"), {full, button_h})) {
            ImGui::SetClipboardText(code.c_str());
            copied_ = true;
        }
        ImGui::TextDisabled("%s", TX("Envoyez-le à vos amis : ils le collent dans Rejoindre."));

        section(TX("Joueurs"));
        for (int i = 0; i < int(st.names.size()); ++i) {
            const bool me = i + 1 == st.position;
            char unnamed[64];
            std::snprintf(unnamed, sizeof unnamed, TX("Joueur %d"), i + 1);
            ImGui::TextColored(me ? good : ImVec4(0.91f, 0.97f, 1, 1), "%d. %s%s", i + 1,
                               st.names[i].empty() ? unnamed : st.names[i].c_str(), me ? TX(" (vous)") : "");
        }
        for (int i = int(st.names.size()); i < st.size; ++i) ImGui::TextDisabled(TX("%d. en attente..."), i + 1);
        ImGui::Dummy({0, 4.f * scale});
        ImGui::PushTextWrapPos(0);
        if (link->state() == State::Waiting) {
            ImGui::TextColored(warn, "%s", TX("En attente des autres joueurs ; la course démarre quand la salle est complète."));
        } else {
            ImGui::TextColored(good, "%s", racing ? TX("Course en ligne en cours.") : TX("Salle complète : démarrage..."));
            ImGui::TextUnformatted(TX("Appuyez sur Start (Entrée) pour jouer. Échap revient à ce menu."));
        }
        if (!message.empty()) ImGui::TextColored(warn, "%s", message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 4.f * scale});
        if (racing && link->state() == State::Ready && ImGui::Button(TX("Reprendre la course"), {full, button_h}))
            action = Resume;
        if (ImGui::Button(TX("Quitter la salle"), {full, button_h})) action = Leave;
    }
    ImGui::Dummy({0, 6.f * scale});
    ImGui::Separator();
    if (ImGui::Button(TX("Retour"), {full, button_h})) action = Back;
    if (action == Stay && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        action = racing && link && link->state() == RelayLink::State::Ready ? Resume : Back;
    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
    return action;
}

} // namespace app
