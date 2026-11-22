// The optional context file describes the display state of one MSC2 export.
#pragma once

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rt::msc2 {

struct FrameContext {
    uint64_t frame;
    int32_t width, height;
    int32_t crtc_x, crtc_y;
    int32_t render_x, render_y;
};

namespace detail {
template<class T>
T context_number(std::istream &input) {
    std::string token;
    if (!(input >> token)) throw std::runtime_error("missing MSC2 context field");
    T value{};
    const auto end = token.data() + token.size();
    const auto result = std::from_chars(token.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::runtime_error("invalid MSC2 context number");
    return value;
}
} // namespace detail

inline FrameContext read_context(const std::string &text) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    std::string magic;
    if (!(input >> magic) || magic != "MSC2_CONTEXT" ||
        detail::context_number<uint32_t>(input) != 1)
        throw std::runtime_error("expected MSC2_CONTEXT version 1");
    const FrameContext context{
        detail::context_number<uint64_t>(input),
        detail::context_number<int32_t>(input), detail::context_number<int32_t>(input),
        detail::context_number<int32_t>(input), detail::context_number<int32_t>(input),
        detail::context_number<int32_t>(input), detail::context_number<int32_t>(input)};
    if (context.width <= 0 || context.height <= 0)
        throw std::runtime_error("invalid MSC2 reference dimensions");
    std::string extra;
    if (input >> extra) throw std::runtime_error("unexpected MSC2 context field");
    return context;
}

inline void write_context(std::FILE *file, const FrameContext &context) {
    if (std::fprintf(file, "MSC2_CONTEXT 1 %llu %d %d %d %d %d %d\n",
                     static_cast<unsigned long long>(context.frame),
                     context.width, context.height, context.crtc_x, context.crtc_y,
                     context.render_x, context.render_y) < 0)
        throw std::runtime_error("scene context write failed");
}

} // namespace rt::msc2
