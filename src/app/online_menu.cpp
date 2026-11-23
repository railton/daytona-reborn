#include "app/online_menu.h"
#include "app/i18n.h"
#include "app/relay_link.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace app {

namespace {
void copy_to(char *dst, size_t size, const std::string &src) {
    std::snprintf(dst, size, "%s", src.c_str());
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
    const float scale = std::max(0.6f, std::min(view->Size.y / 900.f, view->Size.x / 980.f));
    const ImVec2 size(std::min(view->Size.x - 32.f, 620.f * scale), std::min(view->Size.y - 32.f, 640.f * scale));
    ImGui::SetNextWindowPos({view->Pos.x + view->Size.x * 0.5f, view->Pos.y + view->Size.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size);
    ImGui::PushFont(nullptr, 18.f * scale);
    ImGui::Begin(TX("En ligne##online"), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const ImVec4 warn(1, 0.74f, 0.45f, 1), good(0.48f, 0.98f, 0.79f, 1);
    const float field = ImGui::GetContentRegionAvail().x * 0.6f;
    using State = RelayLink::State;
    const bool in_room = link && link->state() != State::Failed;

    if (!in_room) {
        ImGui::TextWrapped("%s", TX("Jouez en réseau par Internet avec jusqu'à 8 joueurs, à travers un serveur daytona-relay."));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputText(TX("Votre nom"), name_, sizeof name_)) {
            cfg.online_name = online::clean_name(name_);
            cfg.save();
        }
        ImGui::SeparatorText(TX("Créer une salle"));
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputText(TX("Serveur (IP ou DNS)"), server_buf_, sizeof server_buf_)) {
            cfg.online_server = server_buf_;
            cfg.save();
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputText(TX("Mot de passe du serveur (facultatif)"), password_, sizeof password_, ImGuiInputTextFlags_Password)) {
            cfg.online_password = password_;
            cfg.save();
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::SliderInt(TX("Joueurs"), &cfg.online_players, online::kMinPlayers, online::kMaxPlayers)) cfg.save();
        if (ImGui::Button(TX("Créer la salle"), {field, 0})) {
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
        ImGui::SeparatorText(TX("Rejoindre une salle"));
        ImGui::SetNextItemWidth(field);
        ImGui::InputText(TX("Code d'invitation"), invite_, sizeof invite_);
        if (ImGui::Button(TX("Coller"))) {
            if (const char *clip = ImGui::GetClipboardText()) copy_to(invite_, sizeof invite_, clip);
        }
        ImGui::SameLine();
        if (ImGui::Button(TX("Rejoindre"), {field - ImGui::GetItemRectSize().x - ImGui::GetStyle().ItemSpacing.x, 0})) {
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
        const std::string &problem = !error_.empty() ? error_ : link ? link->error() : message;
        if (!problem.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(warn, "%s", problem.c_str());
            ImGui::PopTextWrapPos();
        }
    } else if (link->state() == State::Connecting) {
        ImGui::Text(TX("Connexion à %s..."), link->server().c_str());
        if (ImGui::Button(TX("Annuler"), {field, 0})) action = Leave;
    } else {
        const auto &st = link->status();
        ImGui::Text(TX("Salle %s : %d / %d joueurs"), link->room().c_str(), st.joined, st.size);
        ImGui::SeparatorText(TX("Code d'invitation"));
        const std::string code = online::encode_invite({link->server(), link->room(), room_password_});
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(code.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button(copied_ ? TX("Copié !") : TX("Copier le code"))) {
            ImGui::SetClipboardText(code.c_str());
            copied_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TX("Envoyez-le à vos amis : ils le collent dans Rejoindre."));
        ImGui::SeparatorText(TX("Joueurs"));
        for (int i = 0; i < int(st.names.size()); ++i) {
            const bool me = i + 1 == st.position;
            char unnamed[64];
            std::snprintf(unnamed, sizeof unnamed, TX("Joueur %d"), i + 1);
            ImGui::TextColored(me ? good : ImVec4(0.91f, 0.97f, 1, 1), "%d. %s%s", i + 1,
                               st.names[i].empty() ? unnamed : st.names[i].c_str(), me ? TX(" (vous)") : "");
        }
        for (int i = int(st.names.size()); i < st.size; ++i) ImGui::TextDisabled(TX("%d. en attente..."), i + 1);
        ImGui::Spacing();
        if (link->state() == State::Waiting) {
            ImGui::TextColored(warn, "%s", TX("En attente des autres joueurs ; la course démarre quand la salle est complète."));
        } else {
            ImGui::TextColored(good, "%s", racing ? TX("Course en ligne en cours.") : TX("Salle complète : démarrage..."));
            ImGui::TextWrapped("%s", TX("Appuyez sur Start (Entrée) pour jouer. Échap revient à ce menu."));
        }
        if (!message.empty()) ImGui::TextColored(warn, "%s", message.c_str());
        ImGui::Spacing();
        if (racing && link->state() == State::Ready && ImGui::Button(TX("Reprendre la course"), {field, 0})) action = Resume;
        if (ImGui::Button(TX("Quitter la salle"), {field, 0})) action = Leave;
    }
    ImGui::Spacing();
    if (ImGui::Button(TX("Retour"), {field, 0})) action = Back;
    if (action == Stay && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        action = racing && link && link->state() == RelayLink::State::Ready ? Resume : Back;
    ImGui::End();
    ImGui::PopFont();
    return action;
}

} // namespace app
