// Diagnostic command provenance must survive jumps without becoming an ID.
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "../src/runtime/geo.cpp"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv) {
    CHECK(argc == 2);
#ifdef _WIN32
    CHECK(_putenv_s("M2_OBJECT_TRACE", argv[1]) == 0);
#else
    CHECK(setenv("M2_OBJECT_TRACE", argv[1], 1) == 0);
#endif
    std::vector<uint8_t> polygons(128), textures(128);
    std::array<uint32_t, 0x8000> buffer{};
    auto geo = std::make_unique<rt::Geo>(polygons, textures, buffer.data());
    // NOP then jump to a nonadjacent object opcode, not its first argument.
    buffer[0] = 0;
    buffer[1] = 0x80000000u | (20u * 4u);
    buffer[2] = 15u << 23; // Decoy end: ignoring the jump must emit no object.
    buffer[20] = 1u << 23;
    buffer[21] = 4; buffer[22] = 8; buffer[23] = 0x800000; buffer[24] = 1;
    buffer[25] = 15u << 23;

    rt::m2trace::frame_begin(10);
    geo->parse(0);
    rt::m2trace::frame_end();
    // Reuse the same buffer slot in another frame with different arguments.
    buffer[21] = 12; buffer[22] = 16;
    buffer[23] = 0x800001; // Different source address, not merely a texture change.
    rt::m2trace::frame_begin(11);
    geo->parse(0);
    rt::m2trace::frame_end();
    // The previous row must retain its metadata when a later command begins.
    rt::m2trace::frame_begin(12);
    rt::m2trace::command_begin(30, 2u << 23);
    rt::m2trace::begin_direct(24, 28);
    rt::m2trace::command_begin(40, 2u << 23);
    rt::m2trace::begin_direct(32, 36); // Forces delayed flush of A while beginning B.
    rt::m2trace::frame_begin(13); // Flush B before changing the frame number.
    rt::m2trace::begin_direct(); // Reset context must not leak B's offset.
    rt::m2trace::frame_end();
    rt::m2trace::finish();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file);
    char line[1024];
    CHECK(std::fgets(line, sizeof line, file));
    CHECK(std::string(line) == "frame,pool,oba,obc,mode,matrix0,matrix1,matrix2,matrix3,matrix4,"
          "matrix5,matrix6,matrix7,matrix8,matrix9,matrix10,matrix11,vertices,source,command_word,opcode,tpa,tha\n");
    const char *tails[] = {",ROM,20,8388608,4,8\n", ",ROM,20,8388608,12,16\n",
                          ",direct,30,16777216,24,28\n", ",direct,40,16777216,32,36\n",
                          ",direct,4294967295,0,0,0\n"};
    const char *heads[] = {"10,0,8388608,1,0,", "11,0,8388609,1,0,",
                           "12,0,0,0,0,", "12,0,0,0,0,", "13,0,0,0,0,"};
    unsigned index = 0;
    for (const auto *tail : tails) {
        CHECK(std::fgets(line, sizeof line, file));
        const std::string row(line), suffix(tail);
        CHECK(row.rfind(heads[index++], 0) == 0);
        CHECK(std::count(row.begin(), row.end(), ',') == 22);
        CHECK(row.size() >= suffix.size());
        CHECK(row.compare(row.size()-suffix.size(), suffix.size(), suffix) == 0);
    }
    CHECK(!std::fgets(line, sizeof line, file));
    std::fclose(file);
    std::puts("command provenance: jump offset, buffer reuse, deferred flush and frame reset passed");
}
