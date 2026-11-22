#include "runtime/msc2_snapshot.h"

#include <fstream>
#include <iostream>
#include <limits>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: m2scenecheck SNAPSHOT\n";
        return 2;
    }
    try {
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("cannot open snapshot");
        const std::streamoff length = file.tellg();
        if (length < 0 || uintmax_t(length) > std::numeric_limits<size_t>::max() ||
            uintmax_t(length) > uintmax_t(std::numeric_limits<std::streamsize>::max()))
            throw std::runtime_error("snapshot length cannot be represented");
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            file.peek() != std::char_traits<char>::eof() || file.bad())
            throw std::runtime_error("could not read complete snapshot");
        const auto scene = rt::msc2::read(bytes);
        uint64_t primitives = 0;
        uint64_t vertices = 0;
        for (const auto &mesh : scene.meshes) {
            primitives += mesh.primitives.size();
            for (const auto &p : mesh.primitives) vertices += p.vertices.size();
        }
        std::cout << "{\"meshes\":" << scene.meshes.size()
                  << ",\"instances\":" << scene.instances.size()
                  << ",\"primitives\":" << primitives
                  << ",\"vertices\":" << vertices << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
