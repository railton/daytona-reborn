// Online play: the comm board's wire frames, compressed for the relay.
//
// Measured on captured Daytona sessions (2, 4 and 8 cabinets): a frame is
// 3585 bytes (an id and 8 slots of 448 bytes, one per cabinet of the ring),
// two a game frame (the block and the MASTER's 0xFC token), so 3.3 Mbit/s a
// player each way; but only 1 to 6% of a block changes from one game frame
// to the next. Each frame becomes one message against the sender's last two
// blocks (the race: 135 / 251 / 463 bytes a game frame with 2 / 4 / 8
// cabinets instead of 7170):
//
//   0x00 raw:   the frame as it is (link handshake FF/FE, anything unknown)
//   0x01 token: u16 length, byte 1 (the board reads only bytes 0 and 1 of a
//               0xFC token; the rest decodes as zeros)
//   0x02 block: id, then per 448-byte slot: a reference (0 zeros, 1..8 that
//               slot of the previous block, 9..16 of the one before) and the
//               XOR against it as (varint zero run, varint literal count,
//               literals)*
//
// One codec a direction a ring: TCP keeps the order, and the state starts
// again with each new ring (RelayLink's epoch). The relay only passes bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app::online {

class LinkCodec {
public:
    void reset();
    // Appends the message for one wire frame.
    void encode(const uint8_t *frame, size_t n, std::string &out);
    // Appends the frame of one message; false: malformed (out unchanged).
    bool decode(const uint8_t *message, size_t n, std::string &out);

    static constexpr size_t kSlot = 0x1c0, kMaxFrame = 0x4000;

private:
    std::vector<uint8_t> hist_[2]; // the sender's previous block (no id), the one before
    const uint8_t *ref(uint8_t r) const;
    void push(const uint8_t *block, size_t n);
};

} // namespace app::online
