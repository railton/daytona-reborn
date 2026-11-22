#include "runtime/port_observation.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using rt::PortObservation;
using rt::PortObservationWriter;

int g_failures = 0;

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
        return false;
    }
    return true;
}

std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(c);
            }
        } else if (c == ',') {
            fields.push_back(field);
            field.clear();
        } else if (c == '"') {
            in_quotes = true;
        } else {
            field.push_back(c);
        }
    }
    fields.push_back(field);
    return fields;
}

bool read_lines(std::FILE* file, std::vector<std::string>& lines) {
    if (std::fflush(file) != 0) {
        return false;
    }
    std::rewind(file);
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), file) !=
           nullptr) {
        std::string line(buffer);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return std::ferror(file) == 0;
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed =
        std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        return false;
    }
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool parse_u32(const std::string& text, std::uint32_t& value) {
    std::uint64_t parsed = 0;
    if (!parse_u64(text, parsed) ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_bool_field(const std::string& text, bool& value) {
    if (text == "0") {
        value = false;
        return true;
    }
    if (text == "1") {
        value = true;
        return true;
    }
    return false;
}

bool expect_u64(const std::vector<std::string>& fields, std::size_t index,
                std::uint64_t expected, const char* message) {
    if (!check(index < fields.size(), message)) {
        return false;
    }
    std::uint64_t actual = 0;
    if (!check(parse_u64(fields[index], actual), message)) {
        return false;
    }
    return check(actual == expected, message);
}

bool expect_u32(const std::vector<std::string>& fields, std::size_t index,
                std::uint32_t expected, const char* message) {
    if (!check(index < fields.size(), message)) {
        return false;
    }
    std::uint32_t actual = 0;
    if (!check(parse_u32(fields[index], actual), message)) {
        return false;
    }
    return check(actual == expected, message);
}

bool expect_bool(const std::vector<std::string>& fields, std::size_t index,
                 bool expected, const char* message) {
    if (!check(index < fields.size(), message)) {
        return false;
    }
    bool actual = false;
    if (!check(parse_bool_field(fields[index], actual), message)) {
        return false;
    }
    return check(actual == expected, message);
}

void test_two_record_csv() {
    std::FILE* file = std::tmpfile();
    if (!check(file != nullptr, "create tmpfile")) {
        return;
    }

    PortObservation first;
    first.frame = 0x1000000000000001ULL;
    first.count = 0x1000000000000042ULL;
    first.address = 0x80001234u;
    first.lane_mask = 0x00FF00FFu;
    first.data = 0xDEADBEEFu;
    first.buffer_cursor = 7u;
    first.observed_ip = 0x40012340u;
    first.cpu_state_present = true;
    for (std::size_t i = 0; i < first.registers.size(); ++i) {
        first.registers[i] =
            0xA0000000u + static_cast<std::uint32_t>(i);
    }

    PortObservation second;
    second.frame = 0x2000000000000002ULL;
    second.count = 0xFEDCBA9876543210ULL;
    second.address = 0x80005678u;
    second.lane_mask = 0xFFFF0000u;
    second.data = 0x12345678u;
    second.buffer_cursor = 0xFFFFFFF0u;
    second.observed_ip = 0xDEADBEEFu;
    second.cpu_state_present = false;
    for (std::size_t i = 0; i < second.registers.size(); ++i) {
        second.registers[i] =
            0xDEAD0000u + static_cast<std::uint32_t>(i);
    }

    first.buffer_provenance_available = true;
    first.buffer_write_observed = true;
    first.buffer_word_written = 32767;
    first.buffer_write_count = std::numeric_limits<std::uint64_t>::max();
    // A non-write must erase stale event coordinates in the serialized record.
    second.buffer_word_written = 99;
    second.buffer_write_count = 42;
    PortObservationWriter writer(file);
    check(writer.write(first), "write first record");
    check(writer.write(second), "write second record");
    check(!writer.failed(), "writer not failed after valid records");
    check(writer.records_written() == 2, "two records written");
    check(writer.flush(), "explicit flush succeeds");

    std::string expected_header =
        "sequence,frame,address,lane_mask,data_masked,buffer_cursor,"
        "cpu_state_present,observed_ip,observed_count";
    for (std::size_t i = 0; i < 32; ++i) {
        expected_header += ",r";
        expected_header += std::to_string(i);
    }

    expected_header += ",buffer_provenance_available,buffer_write_observed,buffer_word_written,buffer_write_count";
    std::vector<std::string> lines;
    check(read_lines(file, lines), "read CSV back from tmpfile");
    check(lines.size() == 3, "one header and two data rows");
    if (lines.size() == 3) {
        check(lines[0] == expected_header, "header is exact");
        check(PortObservationWriter::kColumnCount == std::size_t{45},
              "documented column count is 45");
        check(PortObservationWriter::kHeader == expected_header,
              "public header constant matches schema");

        const std::vector<std::string> header = parse_csv_line(lines[0]);
        const std::vector<std::string> row0 = parse_csv_line(lines[1]);
        const std::vector<std::string> row1 = parse_csv_line(lines[2]);
        check(header.size() == std::size_t{45}, "header has 45 fields");
        check(row0.size() == header.size(),
              "first row field count matches header");
        check(row1.size() == header.size(),
              "second row field count matches header");

        if (row0.size() == header.size()) {
            expect_bool(row0, 41, true, "provenance present");
            expect_bool(row0, 42, true, "completed write present");
            expect_u32(row0, 43, 32767, "last word retained");
            expect_u64(row0, 44, std::numeric_limits<std::uint64_t>::max(), "64bit event count retained");
            expect_u64(row0, 0, 0, "first sequence starts at zero");
            expect_u64(row0, 1, first.frame, "first frame");
            expect_u32(row0, 2, first.address, "first address");
            expect_u32(row0, 3, first.lane_mask, "first lane_mask");
            expect_u32(row0, 4, first.data & first.lane_mask,
                       "first data_masked");
            expect_u32(row0, 5, first.buffer_cursor,
                       "first buffer_cursor");
            expect_bool(row0, 6, true, "first cpu_state_present");
            expect_u32(row0, 7, first.observed_ip,
                       "first observed_ip");
            expect_u64(row0, 8, first.count, "first observed_count");
            for (std::size_t i = 0; i < first.registers.size(); ++i) {
                expect_u32(row0, 9 + i, first.registers[i],
                           "first register value");
            }
        }

        if (row1.size() == header.size()) {
            expect_u64(row1, 43, 0, "nonwrite clears word");
            expect_u64(row1, 44, 0, "nonwrite clears count");
            expect_u64(row1, 0, 1, "second sequence is one");
            expect_u64(row1, 1, second.frame, "second frame");
            expect_u32(row1, 2, second.address, "second address");
            expect_u32(row1, 3, second.lane_mask, "second lane_mask");
            expect_u32(row1, 4, second.data & second.lane_mask,
                       "second data_masked");
            expect_u32(row1, 5, second.buffer_cursor,
                       "second buffer_cursor");
            expect_bool(row1, 6, false, "second cpu_state_present");
            expect_u32(row1, 7, 0,
                       "missing CPU state normalizes observed_ip");
            expect_u64(row1, 8, 0,
                       "missing CPU state normalizes observed_count");
            for (std::size_t i = 0; i < second.registers.size(); ++i) {
                expect_u32(row1, 9 + i, 0,
                           "missing CPU state normalizes registers");
            }
        }

        check((first.data & first.lane_mask) !=
                  (second.data & second.lane_mask),
              "records use different effective masks");
        check(first.count != second.count,
              "records use different 64-bit counts");
        check(first.count > std::numeric_limits<std::uint32_t>::max(),
              "first count exceeds 32 bits");
        check(second.count > std::numeric_limits<std::uint32_t>::max(),
              "second count exceeds 32 bits");
    }

    std::fclose(file);
}

void test_null_file() {
    PortObservation observation;
    PortObservationWriter writer(nullptr);
    check(writer.failed(), "null FILE is failed immediately");
    check(!writer.write(observation), "null FILE write fails");
    check(!writer.write(observation), "null FILE write stays failed");
    check(!writer.flush(), "null FILE flush fails");
    check(writer.records_written() == 0, "null FILE writes no records");
}

void test_nonowning_file() {
    std::FILE* file = std::tmpfile();
    if (!check(file != nullptr, "create tmpfile for nonowning test")) {
        return;
    }

    PortObservation observation;
    observation.frame = 3;
    observation.cpu_state_present = false;

    {
        PortObservationWriter writer(file);
        check(writer.write(observation), "write before destruction");
        check(writer.flush(), "flush before destruction");
    }

    check(std::fputs("post-destruction\n", file) >= 0,
          "borrowed FILE remains usable after writer destruction");
    check(std::fflush(file) == 0,
          "borrowed FILE flushes after destruction");

    std::vector<std::string> lines;
    check(read_lines(file, lines), "read nonowning file");
    check(lines.size() >= 3,
          "header, row, and post-destruction write exist");
    if (!lines.empty()) {
        check(lines.back() == "post-destruction",
              "post-destruction write is present");
    }

    std::fclose(file);
}

}  // namespace

void test_completed_write_events() {
    rt::BufferWriteProvenance provenance(0x8000);
    rt::PortObservation observation;
    auto record = [&](unsigned word) { check(provenance.record(word, 0xffffffffu, rt::WriteOrigin::GeometryPort), "write accepted"); };
    rt::observe_port_write(observation, &provenance, 12, [&] { record(3); });
    check(observation.buffer_write_observed && observation.buffer_word_written == 3 && observation.buffer_write_count == 1, "first event captured");
    rt::observe_port_write(observation, &provenance, 12, [] {});
    check(!observation.buffer_write_observed && observation.buffer_word_written == 0 && observation.buffer_write_count == 0, "no-op does not reuse previous event");
    rt::observe_port_write(observation, &provenance, 12, [&] { record(3); });
    check(observation.buffer_write_count == 2, "same slot next write distinct");
    rt::observe_port_write(observation, &provenance, 4, [&] { record(1); });
    check(observation.buffer_word_written == 1 && observation.buffer_write_count == 1, "reordered slots retain independent counts");
    bool called = false;
    rt::observe_port_write(observation, nullptr, 12, [&] { called = true; });
    check(called && !observation.buffer_provenance_available && !observation.buffer_write_observed, "disabled tracking still executes handler");
    rt::observe_port_write(observation, &provenance, 0xffffffffu, [] {});
    check(!observation.buffer_write_observed, "out-of-range cursor on nonwrite safe");
    bool threw = false;
    try { rt::observe_port_write(observation, &provenance, 12, [] { throw std::runtime_error("handler failed"); }); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw && !observation.buffer_write_observed, "handler exception preserved without event");
    threw = false;
    try { rt::observe_port_write(observation, &provenance, 12, [&] { record(3); record(3); }); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "multiple writes are not silently joined");
}

int main() {
    test_completed_write_events();
    test_two_record_csv();
    test_null_file();
    test_nonowning_file();

    if (g_failures != 0) {
        std::cerr << g_failures << " test failure(s)\n";
        return 1;
    }
    std::cout << "port_observation tests passed\n";
    return 0;
}
