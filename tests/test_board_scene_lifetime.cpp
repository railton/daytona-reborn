#include "runtime/m2_board.h"
#include "runtime/msc2_frame_context.h"
#include "runtime/msc2_layer_memory.h"
#include "runtime/msc2_material_memory.h"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace rt {

const std::uint32_t tgpgen::program_crc32 = 0;
const std::uint32_t tgpgen::program_words = 0;

void tgpgen::run(Tgp&, std::uint64_t) {
    throw std::runtime_error("TGP execution is forbidden in the M2Board scene lifetime test");
}

}  // namespace rt

struct CheckError {
    std::string message;
};

[[noreturn]] static void fail(const std::string& message) {
    throw CheckError{message};
}

#define CHECK(cond) do { if (!(cond)) { std::ostringstream _check; _check << "CHECK failed: " #cond << " at " << __FILE__ << ":" << __LINE__; fail(_check.str()); } } while (0)
#define CHECK_MSG(cond, msg) do { if (!(cond)) { std::ostringstream _check; _check << msg << " [CHECK: " #cond " at " << __FILE__ << ":" << __LINE__ << "]"; fail(_check.str()); } } while (0)

static bool set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

static void clear_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

static void set_env_or_fail(const char* name, const std::string& value) {
    CHECK_MSG(set_env(name, value), "could not set environment variable " << name << " to " << value);
}

static void configure_scene(const fs::path& cache_log, const fs::path& render, const fs::path& export_path, int target_frame, bool with_context = false) {
    set_env_or_fail("M2_SCENE_CACHE_LOG", cache_log.string());
    set_env_or_fail("M2_SCENE_RENDER", render.string());
    set_env_or_fail("M2_SCENE_EXPORT", export_path.string());
    set_env_or_fail("M2_SCENE_EXPORT_CONTEXT", with_context ? export_path.string() + ".context" : "");
    set_env_or_fail("M2_SCENE_EXPORT_MATERIAL", with_context ? export_path.string() + ".m2mat" : "");
    set_env_or_fail("M2_SCENE_EXPORT_LAYERS", with_context ? export_path.string() + ".m2lay" : "");
    set_env_or_fail("M2_SCENE_RENDER_FRAME", std::to_string(target_frame));
    set_env_or_fail("M2_SCENE_EXPORT_FRAME", std::to_string(target_frame));
}

static void clear_debug_envs() {
    const char* names[] = {
        "M2_WRITE_TRACE",
        "M2_OBJECT_TRACE",
        "M2_PORT_OBSERVATION_TRACE",
        "M2_MESH_DUMP",
        "M2_SCENE_TIMING_TRACE",
        "M2_SCENE_EXPORT_CONTEXT",
        "M2_SCENE_EXPORT_MATERIAL",
        "M2_SCENE_EXPORT_LAYERS"
    };
    for (const char* name : names) {
        clear_env(name);
    }
}

static rt::M2Board::Images make_images() {
    rt::M2Board::Images images{};
    images.program.assign(0x200000u, 0u);
    images.main_data.assign(0x2000000u, 0u);
    images.copro_tables.assign(0x40000u, 0u);
    images.copro_data.assign(0x800000u, 0u);
    images.polygons.resize(4);
    images.textures.resize(2);
    return images;
}

static std::string read_text(const fs::path& path, const std::string& what) {
    std::ifstream in(path, std::ios::binary);
    CHECK_MSG(in.good(), "cannot open " << what << " for reading: " << path.string());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

static void check_cache_log(const fs::path& path, std::initializer_list<int> frames, const std::string& label) {
    std::error_code ec;
    CHECK_MSG(fs::is_regular_file(path, ec), label << ": missing cache CSV " << path.string());
    const std::vector<std::string> lines = split_lines(read_text(path, label + " cache CSV"));
    const std::size_t expected_lines = frames.size() + 1u;
    CHECK_MSG(lines.size() == expected_lines, label << ": expected " << expected_lines << " lines in " << path.string() << ", got " << lines.size());
    const std::string header = "frame,polys,unique";
    CHECK_MSG(lines[0].compare(0, header.size(), header) == 0, label << ": expected header starting with '" << header << "' in " << path.string() << ", got '" << lines[0] << "'");
    std::size_t row = 1;
    for (int expected_frame : frames) {
        const std::string& line = lines[row++];
        const std::size_t comma = line.find(',');
        CHECK_MSG(comma != std::string::npos, label << ": cache row has no frame field: " << line);
        const std::string actual_frame = line.substr(0, comma);
        CHECK_MSG(actual_frame == std::to_string(expected_frame), label << ": expected frame " << expected_frame << " in " << path.string() << ", got '" << actual_frame << "'");
    }
}

static void check_p6(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    CHECK_MSG(!ec && size >= 3, label << ": missing or too-short P6 render " << path.string());
    std::ifstream in(path, std::ios::binary);
    CHECK_MSG(in.good(), label << ": cannot open P6 render " << path.string());
    char magic[3] = {0, 0, 0};
    in.read(magic, 3);
    CHECK_MSG(in.gcount() == 3, label << ": truncated P6 header in " << path.string());
    CHECK_MSG(magic[0] == 'P' && magic[1] == '6', label << ": bad P6 magic in " << path.string());
    CHECK_MSG(std::isspace(static_cast<unsigned char>(magic[2])) != 0, label << ": P6 magic is not followed by whitespace in " << path.string());
}

static void check_export(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    CHECK_MSG(!ec && size == 16, label << ": expected 16-byte MSC2 export " << path.string());
    std::ifstream in(path, std::ios::binary);
    CHECK_MSG(in.good(), label << ": cannot open MSC2 export " << path.string());
    std::uint8_t bytes[16] = {};
    in.read(reinterpret_cast<char*>(bytes), 16);
    CHECK_MSG(in.gcount() == 16, label << ": truncated MSC2 export " << path.string());
    const auto word = [&](std::size_t i) -> std::uint32_t {
        return static_cast<std::uint32_t>(bytes[i]) |
               (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[i + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[i + 3]) << 24);
    };
    CHECK_MSG(word(0) == 0x3243534dU, label << ": bad MSC2 magic in " << path.string());
    CHECK_MSG(word(4) == 1U, label << ": bad MSC2 version in " << path.string());
    CHECK_MSG(word(8) == 0U, label << ": expected zero mesh count in " << path.string());
    CHECK_MSG(word(12) == 0U, label << ": expected zero instance count in " << path.string());
}

static void seed_material(rt::M2Board& board, uint16_t value) {
    board.write_word(0x01802000, value + 1);
    board.write_word(0x01810000, value + 2);
    board.write_dword(0x12800000, value + 3);
    board.write_dword(0x12000000, value + 4);
    board.write_dword(0x12400000, value + 5);
}

static void check_companions(const fs::path& path, uint16_t seed) {
    const std::string material = read_text(path.string() + ".m2mat", "scene material");
    std::string snapshot = read_text(path, "scene snapshot");
    const auto bytes = [](const std::string& value) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    };
    const auto view = rt::msc2::read_material(bytes(material));
    CHECK(view.frame == 0);
    rt::msc2::verify_material_snapshot(view, bytes(snapshot));
    const std::string layers = read_text(path.string() + ".m2lay", "scene layers");
    const auto layer_view = rt::msc2::read_layers(bytes(layers));
    CHECK(layer_view.header.frame == 0);
    CHECK(layer_view.background.size() == rt::msc2::layer_image_size);
    CHECK(layer_view.foreground.size() == rt::msc2::layer_image_size);
    rt::msc2::verify_companion_snapshot(layer_view.header.snapshot, bytes(snapshot));
    unsigned index = 1;
    for (const auto block : {view.palette, view.xlat, view.luma, view.tex0, view.tex1}) {
        CHECK(block.size() >= 2);
        CHECK(block[0] == (seed + index) % 256 && block[1] == (seed + index) / 256);
        ++index;
    }
    snapshot[8] ^= 1;
    bool rejected = false;
    try {
        rt::msc2::verify_material_snapshot(view, bytes(snapshot));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

static void require_absent(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    CHECK_MSG(!ec, label << ": could not test existence of " << path.string() << ": " << ec.message());
    CHECK_MSG(!exists, label << ": unexpected output was created: " << path.string());
}

static fs::path make_workspace(const fs::path& parent) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::ostringstream name;
        name << "m2_board_scene_lifetime_" << stamp << "_" << attempt;
        const fs::path candidate = parent / name.str();
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
            return candidate;
        }
        if (ec && ec != std::errc::file_exists) {
            fail("could not create " + candidate.string() + ": " + ec.message());
        }
    }
    fail("could not create a unique test directory under " + parent.string());
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "usage: " << argv[0] << " <existing-parent-test-directory>" << std::endl;
            return 2;
        }
        clear_debug_envs();
        const fs::path parent(argv[1]);
        std::error_code parent_ec;
        CHECK_MSG(fs::is_directory(parent, parent_ec), "argv[1] is not an existing directory: " << parent.string());
        const fs::path root = make_workspace(parent);
        std::cout << "scene lifetime test directory: " << root.string() << std::endl;

        const fs::path a_cache = root / "a_cache.csv";
        const fs::path a_render = root / "a_render.ppm";
        const fs::path a_export = root / "a_export.msc2";
        const fs::path a_render2 = root / "a_render_frame1.ppm";
        const fs::path a_export2 = root / "a_export_frame1.msc2";
        const fs::path b_cache = root / "b_cache.csv";
        const fs::path b_render = root / "b_render.ppm";
        const fs::path b_export = root / "b_export.msc2";
        const fs::path c_cache = root / "c_cache.csv";
        const fs::path c_render = root / "c_render.ppm";
        const fs::path c_export = root / "c_export.msc2";

        auto a = std::make_unique<rt::M2Board>(make_images());
        auto b = std::make_unique<rt::M2Board>(make_images());
        CHECK(a != nullptr);
        CHECK(b != nullptr);
        CHECK(!a->last_drawn_tick() && !b->last_drawn_tick());
        CHECK(!a->copy_drawn_scene() && !b->copy_drawn_scene());
        seed_material(*a, 0x1250);
        seed_material(*b, 0x2390);

        configure_scene(a_cache, a_render, a_export, 0, true);
        a->vblank_end();
        CHECK(a->last_drawn_tick() == 0 && !b->last_drawn_tick());
        configure_scene(b_cache, b_render, b_export, 0, true);
        b->vblank_end();
        CHECK(b->last_drawn_tick() == 0);
        const auto first = a->copy_drawn_scene();
        CHECK(first && first->context.frame == 0 && first->context.width == rt::Video::W);
        const auto first_bytes = read_text(a_export, "A first snapshot");
        CHECK(std::string_view(reinterpret_cast<const char *>(first->bytes.data()), first->bytes.size()) == first_bytes);
        a->set_wide_margin(8);
        const auto after_resize = a->copy_drawn_scene();
        CHECK(after_resize && after_resize->context.width == first->context.width);
        CHECK(after_resize->bytes == first->bytes);
        a->set_wide_margin(0);

        check_cache_log(a_cache, {0}, "A first frame");
        check_p6(a_render, "A first frame");
        check_export(a_export, "A first frame");
        check_cache_log(b_cache, {0}, "B first frame");
        check_p6(b_render, "B first frame");
        check_export(b_export, "B first frame");
        check_companions(a_export, 0x1250);
        check_companions(b_export, 0x2390);
        for (const auto &path : {a_export, b_export}) {
            const auto context = rt::msc2::read_context(read_text(path.string() + ".context", "scene context"));
            CHECK(context.frame == 0 && context.width == rt::Video::W && context.height == rt::Video::H);
            CHECK(context.crtc_x == a->video().crtc_x() && context.crtc_y == a->video().crtc_y());
            CHECK(context.render_x == a->video().render_x() && context.render_y == a->video().render_y());
        }

        configure_scene(a_cache, a_render2, a_export2, 1, true);
        a->vblank_end();
        CHECK(a->last_drawn_tick() == 1 && b->last_drawn_tick() == 0);
        const auto second = a->copy_drawn_scene();
        CHECK(second && second->context.frame == 1 && first->context.frame == 0);
        CHECK(std::string_view(reinterpret_cast<const char *>(first->bytes.data()), first->bytes.size()) == first_bytes);
        require_absent(a_render2, "A changed-target render");
        require_absent(a_export2, "A changed-target export");
        require_absent(a_export2.string() + ".context", "A changed-target context");
        require_absent(a_export2.string() + ".m2mat", "A changed-target material");
        require_absent(a_export2.string() + ".m2lay", "A changed-target layers");
        check_cache_log(a_cache, {0, 1}, "A original cache after second frame");
        check_cache_log(b_cache, {0}, "B cache after A second frame");

        a.reset();
        b.reset();

        auto c = std::make_unique<rt::M2Board>(make_images());
        CHECK(c != nullptr);
        CHECK(!c->last_drawn_tick());
        configure_scene(c_cache, c_render, c_export, 0);
        c->vblank_end();
        check_cache_log(c_cache, {0}, "C first frame after destruction");
        check_p6(c_render, "C first frame after destruction");
        check_export(c_export, "C first frame after destruction");
        require_absent(c_export.string() + ".context", "legacy export context");
        require_absent(c_export.string() + ".m2mat", "legacy material export");
        require_absent(c_export.string() + ".m2lay", "legacy layer export");
        c->set_frame_skip(1);
        c->vblank_end();
        CHECK(c->frame() == 2 && c->last_drawn_tick() == 0);
        CHECK(!c->copy_drawn_scene());
        c->vblank_end();
        CHECK(c->frame() == 3 && c->last_drawn_tick() == 2);
        const auto resumed = c->copy_drawn_scene();
        CHECK(resumed && resumed->context.frame == 2);
        check_cache_log(c_cache, {0, 2}, "C skips tick 1");

        std::cout << "PASS: M2Board scene cache/render/export state is isolated per board" << std::endl;
        return 0;
    } catch (const CheckError& error) {
        std::cerr << "FAIL: " << error.message << std::endl;
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << std::endl;
        return 1;
    }
}
