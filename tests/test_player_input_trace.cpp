#include "../src/app/player_input_trace.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>
#include <sstream>
#include <string>

using app::PlayerInputSample;
using app::PlayerInputTraceWriter;
using State = PlayerInputTraceWriter::State;

namespace app {
struct PlayerInputTraceWriterTestAccess {
    static void set_next_tick(PlayerInputTraceWriter& w, uint64_t t) { w.next_tick_ = t; }
};
}  // namespace app

struct Grouping : std::numpunct<char> {
    char do_thousands_sep() const override { return '\''; }
    std::string do_grouping() const override { return "\3"; }
};
struct ThrowBuf : std::streambuf {
    int allowed;
    explicit ThrowBuf(int n) : allowed(n) {}
    int_type overflow(int_type c) override {
        if (allowed-- <= 0) throw std::runtime_error("boom");
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char*, std::streamsize n) override {
        if (allowed < n) throw std::runtime_error("boom");
        allowed -= int(n);
        return n;
    }
};

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const char* kHeader = "tick,steer,accel,brake,in0,in1,in2\n";

struct BadBuf : std::streambuf {  // sans tampon: echoue apres `allowed` caracteres
    int allowed;
    explicit BadBuf(int n) : allowed(n) {}
    int_type overflow(int_type c) override { return allowed-- > 0 ? traits_type::not_eof(c) : traits_type::eof(); }
    std::streamsize xsputn(const char*, std::streamsize n) override {
        if (allowed < n) { allowed = 0; return 0; }
        allowed -= int(n);
        return n;
    }
};
int main() {
    {  // deux etats, ticks exacts, fin
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        CHECK(w.begin());
        CHECK(w.write(0, PlayerInputSample{}));
        CHECK(w.write(1, PlayerInputSample{1, 2, 3, 4, 5, 6}));
        CHECK(!w.is_complete());
        CHECK(w.finish());
        CHECK(w.is_complete() && w.state() == State::Finished);
        CHECK(os.str() == std::string(kHeader) + "0,128,32,32,255,143,255\n1,1,2,3,4,5,6\n#end,2\n");
    }
    {  // extremes 0/255
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        CHECK(w.write(0, PlayerInputSample{0, 0, 0, 0, 0, 0}));
        CHECK(w.write(1, PlayerInputSample{255, 255, 255, 255, 255, 255}));
        CHECK(w.finish());
        CHECK(os.str() == std::string(kHeader) + "0,0,0,0,0,0,0\n1,255,255,255,255,255,255\n#end,2\n");
    }
    {  // journal vide mais fini
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        CHECK(w.finish());
        CHECK(os.str() == std::string(kHeader) + "#end,0\n");
    }
    {  // doit commencer a zero
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        CHECK(!w.write(1, PlayerInputSample{}));
        CHECK(w.failed() && !w.finish() && !w.is_complete());
    }
    {  // duplication
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        CHECK(w.write(0, PlayerInputSample{}));
        CHECK(!w.write(0, PlayerInputSample{}));
        CHECK(w.failed() && !w.write(1, PlayerInputSample{}) && !w.finish() && !w.is_complete());
        CHECK(os.str().find("#end") == std::string::npos);
    }
    {  // saut
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        w.write(0, PlayerInputSample{});
        CHECK(!w.write(2, PlayerInputSample{}));
        CHECK(w.failed() && !w.is_complete());
    }
    {  // appels hors ordre
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        CHECK(!w.write(0, PlayerInputSample{}));
        PlayerInputTraceWriter w2(os);
        w2.begin();
        CHECK(!w2.begin());
        PlayerInputTraceWriter w3(os);
        w3.begin();
        w3.finish();
        CHECK(!w3.write(0, PlayerInputSample{}) && w3.is_complete());  // fin sticky
    }
    {  // flux deja invalide
        std::ostringstream os;
        os.setstate(std::ios::badbit);
        PlayerInputTraceWriter w(os);
        CHECK(!w.begin());
        CHECK(w.failed() && !w.is_complete());
    }
    {  // echec d'ecriture en cours de route
        BadBuf b(int(std::string(kHeader).size()) + 5);
        std::ostream os(&b);
        PlayerInputTraceWriter w(os);
        CHECK(w.begin());
        CHECK(!w.write(0, PlayerInputSample{}));
        CHECK(w.failed() && !w.finish() && !w.is_complete());
    }
    {  // echec a la ligne de fin
        BadBuf b(int(std::string(kHeader).size()));
        std::ostream os(&b);
        PlayerInputTraceWriter w(os);
        CHECK(w.begin());
        CHECK(!w.finish());
        CHECK(w.failed() && !w.is_complete());
    }
    {  // groupement de locale, hex, width, fill: sortie inchangee, flux non mute
        std::ostringstream os;
        os.imbue(std::locale(std::locale::classic(), new Grouping));
        os << std::hex << std::showbase << std::setw(7) << std::setfill('*');
        const std::locale loc = os.getloc();
        PlayerInputTraceWriter w(os);
        CHECK(w.begin());
        CHECK(w.write(0, PlayerInputSample{}));
        CHECK(w.write(1, PlayerInputSample{}));
        CHECK(w.finish());
        CHECK(os.str() == std::string(kHeader) + "0,128,32,32,255,143,255\n1,128,32,32,255,143,255\n#end,2\n");
        CHECK(os.getloc() == loc && (os.flags() & std::ios::hex) && (os.flags() & std::ios::showbase));
        CHECK(os.width() == 7 && os.fill() == '*');
    }
    {  // tick >= 1000 avec groupement: pas de separateur
        std::ostringstream os;
        os.imbue(std::locale(std::locale::classic(), new Grouping));
        PlayerInputTraceWriter w(os);
        w.begin();
        for (uint64_t t = 0; t < 1234; ++t) w.write(t, PlayerInputSample{});
        CHECK(w.finish());
        const std::string s = os.str();
        CHECK(s.find("\n1233,128,32,32,255,143,255\n#end,1234\n") != std::string::npos);
        CHECK(s.find('\'') == std::string::npos);
    }
    {  // exceptions de flux actives: echec d'etat, aucune exception, pas de fin
        BadBuf b(int(std::string(kHeader).size()) + 5);
        std::ostream os(&b);
        os.exceptions(std::ios::badbit | std::ios::failbit);
        PlayerInputTraceWriter w(os);
        bool threw = false;
        try {
            CHECK(w.begin());
            CHECK(!w.write(0, PlayerInputSample{}));
        } catch (...) { threw = true; }
        CHECK(!threw && w.failed() && !w.finish() && !w.is_complete());
    }
    {  // exception du streambuf (sans masque) puis avec masque
        for (int mask = 0; mask < 2; ++mask) {
            ThrowBuf b(int(std::string(kHeader).size()));
            std::ostream os(&b);
            if (mask) os.exceptions(std::ios::badbit);
            PlayerInputTraceWriter w(os);
            bool threw = false;
            try {
                CHECK(w.begin());
                CHECK(!w.write(0, PlayerInputSample{}));
            } catch (...) { threw = true; }
            CHECK(!threw && w.failed() && !w.is_complete());
        }
    }
    {  // exception pendant begin et pendant finish
        ThrowBuf b(0);
        std::ostream os(&b);
        PlayerInputTraceWriter w(os);
        CHECK(!w.begin() && w.failed());
        ThrowBuf b2(int(std::string(kHeader).size()));
        std::ostream os2(&b2);
        PlayerInputTraceWriter w2(os2);
        CHECK(w2.begin());
        CHECK(!w2.finish() && w2.failed() && !w2.is_complete());
    }
    {  // fin sticky: tout appel ulterieur est refuse mais le journal reste complet
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        w.write(0, PlayerInputSample{});
        CHECK(w.finish());
        const std::string before = os.str();
        CHECK(!w.write(1, PlayerInputSample{}));
        CHECK(!w.finish());
        CHECK(!w.begin());
        CHECK(w.is_complete() && w.state() == State::Finished && !w.failed());
        CHECK(os.str() == before);
        os.setstate(std::ios::badbit);  // flux casse apres coup: toujours complet
        CHECK(!w.write(1, PlayerInputSample{}) && w.is_complete());
    }
    {  // UINT64_MAX: increment impossible refuse, aucune ligne ecrite
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        app::PlayerInputTraceWriterTestAccess::set_next_tick(w, UINT64_MAX - 1);
        CHECK(w.write(UINT64_MAX - 1, PlayerInputSample{}));
        CHECK(w.next_tick() == UINT64_MAX);
        const std::string before = os.str();
        CHECK(!w.write(UINT64_MAX, PlayerInputSample{}));
        CHECK(w.failed() && w.next_tick() == UINT64_MAX && !w.finish() && !w.is_complete());
        CHECK(os.str() == before && before.find("#end") == std::string::npos);
    }
    {  // grand tick sur 64 bits, decimal exact
        std::ostringstream os;
        PlayerInputTraceWriter w(os);
        w.begin();
        app::PlayerInputTraceWriterTestAccess::set_next_tick(w, 18446744073709551614ull);
        CHECK(w.write(18446744073709551614ull, PlayerInputSample{}));
        CHECK(os.str().find("\n18446744073709551614,128,") != std::string::npos);
    }
    {  // vrai fichier ouvert en binaire: LF exact, aucun CR
        const char* path = "test_player_input_trace.tmp.csv";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            PlayerInputTraceWriter w(f);
            CHECK(w.begin() && w.write(0, PlayerInputSample{}) && w.finish());
        }
        std::ifstream in(path, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        CHECK(got == std::string(kHeader) + "0,128,32,32,255,143,255\n#end,1\n");
        CHECK(got.find('\r') == std::string::npos);
        std::remove(path);
    }    std::printf(g_fail ? "FAILED %d\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
