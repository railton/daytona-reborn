#include "runtime/background_motion.h"

#include <cstdio>
#include <fstream>
#include <string>

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

using rt::BackgroundObject;
using rt::BackgroundTransform;

int compare_replay(const char *path) {
    std::ifstream file(path);
    CHECK(file);
    std::vector<BackgroundObject> previous, current;
    std::optional<rt::BackgroundMotion> fit;
    unsigned previous_tick = 0, pairs = 0, expected_pairs = 0, alerts = 0;
    double max_error = 0;
    bool known_cut = false;
    std::string kind;
    unsigned tick;
    while (file >> kind >> tick) {
        if (kind == "obj") {
            BackgroundObject object;
            for (auto &value : object.geometry) file >> value;
            for (auto &value : object.model_view) file >> value;
            CHECK(file);
            current.push_back(object);
        } else if (kind == "tick") {
            fit = tick == previous_tick + 1 ? rt::fit_background_motion(previous, current) : std::nullopt;
            if (fit) ++pairs;
            previous.swap(current);
            current.clear();
            previous_tick = tick;
        } else if (kind == "fit") {
            int expected_cut;
            BackgroundTransform expected;
            file >> expected_cut;
            for (auto &value : expected) file >> value;
            CHECK(file && fit && tick + 1 == previous_tick);
            for (unsigned i = 0; i < expected.size(); ++i) {
                const auto error = std::abs(fit->transform[i] - expected[i]);
                max_error = std::max(max_error, error);
                if (error > 1e-8) {
                    std::fprintf(stderr, "fit mismatch at %u, element %u: %.17g vs %.17g\n",
                                 tick, i, fit->transform[i], expected[i]);
                    return 1;
                }
            }
            CHECK(fit->cut_candidate() == bool(expected_cut));
            if (fit->cut_candidate()) ++alerts;
            if (tick == 767) known_cut = fit->cut_candidate();
            ++expected_pairs;
        } else {
            CHECK(false);
        }
    }
    CHECK(file.eof() && expected_pairs == 5517 && pairs == expected_pairs && known_cut);
    std::printf("%u fitted pairs, max matrix error %.3g, %u angular alerts including 767->768\n",
                pairs, max_error, alerts);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2) return compare_replay(argv[1]);
    CHECK(argc == 1);
    const BackgroundTransform delta{0, 1, 0, -1, 0, 0, 0, 0, 1, 2, 3, 0};
    const std::vector<BackgroundObject> left{
        {{1, 0, 1}, {2, 0, 0, 1, 1, 0, 0, 0.5, 1, 3, 4, 10}},
        {{2, 0, 1}, {0, -1, 0, 1, 0, 0, 0, 0, 1, -3, 11, 17}},
        {{3, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1, 20, 12, 14}},
        {{4, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 4, 15}},
    };
    // Explicit transformed inputs exercise rotation, translation, an affine
    // basis and an outlier which must not win the consensus.
    std::vector<BackgroundObject> right{
        {{1, 0, 1}, {0, 2, 0, -1, 1, 0, -0.5, 0, 1, -2, 6, 10}},
        {{2, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1, -9, 0, 17}},
        {{3, 0, 1}, {0, 1, 0, -1, 0, 0, 0, 0, 1, -10, 23, 14}},
        left[3],
    };
    const auto fit = rt::fit_background_motion(left, right);
    CHECK(fit && fit->transform == delta && fit->support == 3 && fit->common_objects == 4);
    CHECK(fit->cut_candidate() && !fit->cut_candidate(91));
    right.push_back(right[0]);
    CHECK(!rt::fit_background_motion(left, right));
    right.pop_back();
    right[0].model_view[11] = 0;
    CHECK(!rt::fit_background_motion(left, right));
}
