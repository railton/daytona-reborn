// Serialiseur CSV des entrees de conduite, independant de SDL.
//
// Format (ASCII, fins de ligne '\n'):
//   tick,steer,accel,brake,in0,in1,in2      <- en-tete, ecrit par begin()
//   <tick>,<steer>,<accel>,<brake>,<in0>,<in1>,<in2>   <- une ligne par tick
//   #end,<n>                                 <- ligne de fin, ecrite par finish()
// Toutes les valeurs sont decimales (0..255 pour les octets, tick sur 64 bits).
// Les ticks commencent a 0 et sont strictement consecutifs. <n> est le nombre de
// lignes de donnees. Un journal sans ligne "#end" n'est PAS complet (is_complete()
// reste faux et un lecteur doit rejeter le fichier).
// Les octets correspondent a rt::Inputs (m2_board.h), sans en dependre.
//
// Sortie non formatee: les chiffres viennent de std::to_chars puis ostream::write,
// donc ni flags (hex, width, fill) ni locale (groupement) du flux ne comptent, et le
// flux de l'appelant n'est jamais modifie (hors flush()).
// ENTREE BINAIRE: pour un vrai fichier, l'appelant DOIT ouvrir avec std::ios::binary.
// Sous Windows un flux texte traduirait '\n' en CRLF et le format ne serait plus LF.
// Echec: toute erreur de flux ou exception (y compris avec exceptions() active)
// donne l'etat Failed sans jamais ecrire de fin valide. Un finish() reussi est
// definitif: les appels suivants sont refuses mais l'etat reste Finished.
// Le tick UINT64_MAX est refuse (next_tick_ ne pourrait pas etre incremente).
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ostream>

namespace app {

struct PlayerInputSample {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20;
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;
};

struct PlayerInputTraceWriterTestAccess;

class PlayerInputTraceWriter {
public:
    enum class State { Idle, Open, Finished, Failed };

    // out doit survivre au writer. L'appelant gere l'ouverture du fichier (binaire).
    explicit PlayerInputTraceWriter(std::ostream& out) : out_(out) {}

    bool begin() {
        if (state_ != State::Idle) return refuse();
        static const char kHeader[] = "tick,steer,accel,brake,in0,in1,in2\n";
        if (!put(kHeader, sizeof(kHeader) - 1, false)) return fail();
        state_ = State::Open;
        return true;
    }

    // tick doit valoir exactement next_tick(). Tout ecart passe en Failed.
    bool write(uint64_t tick, const PlayerInputSample& s) {
        if (state_ != State::Open) return refuse();
        if (tick != next_tick_ || next_tick_ == UINT64_MAX) return fail();
        char buf[96];
        char* p = buf;
        char* const e = buf + sizeof(buf);
        p = number(p, e, tick);
        const unsigned v[6] = {s.steer, s.accel, s.brake, s.in0, s.in1, s.in2};
        for (unsigned x : v) {
            *p++ = ',';
            p = number(p, e, x);
        }
        *p++ = '\n';
        if (!put(buf, static_cast<std::size_t>(p - buf), false)) return fail();
        ++next_tick_;
        return true;
    }

    // Ecrit la ligne de fin et vide le tampon. Seul un finish reussi rend le
    // journal complet; il est ensuite definitif.
    bool finish() {
        if (state_ != State::Open) return refuse();
        char buf[40] = {'#', 'e', 'n', 'd', ','};
        char* p = number(buf + 5, buf + sizeof(buf) - 1, next_tick_);
        *p++ = '\n';
        if (!put(buf, static_cast<std::size_t>(p - buf), true)) return fail();
        state_ = State::Finished;
        return true;
    }

    State state() const { return state_; }
    bool failed() const { return state_ == State::Failed; }
    bool is_complete() const { return state_ == State::Finished; }
    uint64_t next_tick() const { return next_tick_; }

private:
    friend struct PlayerInputTraceWriterTestAccess;

    template <class T>
    static char* number(char* p, char* e, T v) {
        return std::to_chars(p, e, v).ptr;  // buffers dimensionnes: jamais d'erreur
    }
    bool put(const char* data, std::size_t n, bool flush) {
        try {
            out_.write(data, static_cast<std::streamsize>(n));
            if (flush) out_.flush();
            return static_cast<bool>(out_);
        } catch (...) {
            return false;
        }
    }
    // Appel hors etat: refuse, sans toucher un etat Finished (sticky).
    bool refuse() {
        if (state_ != State::Finished) state_ = State::Failed;
        return false;
    }
    bool fail() {
        state_ = State::Failed;
        return false;
    }

    std::ostream& out_;
    State state_ = State::Idle;
    uint64_t next_tick_ = 0;
};

}  // namespace app