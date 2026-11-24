// license:BSD-3-Clause
// copyright-holders:Ariane Fugmann
//
// MAME's m2comm_device (M2COMM_SIMULATION), transplanted: see comm_board.h.
// Changes: the sockets are a non-blocking LinkTransport, so read_frame keeps
// partial frames until whole instead of waiting; the frame-sync wait gives up
// after kSyncTimeout (MAME waits forever); messages through osd_printf_verbose
// are dropped.

#include "runtime/comm_board.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace rt {

namespace {
constexpr auto kSyncTimeout = std::chrono::seconds(3);
}

CommBoard::Link CommBoard::link() const {
    if (!transport_ || !linkenable_) return Link::Off;
    if (linkalive_ == 1) return Link::Up;
    if (linkalive_ == 2) return Link::Lost;
    return Link::Waiting;
}

void CommBoard::cn_w(uint8_t data) {
    cn_ = data & 0x01;
    if (!cn_) {
        // reset command
        linkenable_ = 0x00;
        zfg_ = 0;
        pending_.clear();
        primed_ = false;
        cn_ = 0;
        fg_ = 0;
    } else {
        // init command
        linkenable_ = 0x01;
        linkid_ = 0x00;
        linkalive_ = 0x00;
        linkcount_ = 0x00;
        linktimer_ = 0x00e8; // 58 fps * 4s
        rx_.clear();

        // zero memory
        std::memset(shared_, 0, 0x4000);

        shared_[0x01] = 0x02;

        // frameSize - 0x0e00
        shared_[0x12] = 0x00;
        shared_[0x13] = 0x0e;

        // frameOffset - 0x01c0 in most games
        shared_[0x14] = uint8_t(frame_offset_ & 0xff);
        shared_[0x15] = uint8_t(frame_offset_ >> 8);

        if (preset_count_ > 0) { // the ring is known: up at once, as the numbering would leave it
            linkid_ = preset_id_;
            linkcount_ = preset_count_;
            linkalive_ = 0x01;
            linktimer_ = 0x00;
            shared_[0] = 0x01;
            shared_[2] = linkid_;
            shared_[3] = linkcount_;
        }

        comm_tick();
    }
}

uint8_t CommBoard::fg_r() {
    read_fg();
    // paced: a block's flip shows at the game's own wait, after the frame's
    // reference read (the vblank interrupt reads fg first), once a frame
    if (paced_ && ++fg_reads_ >= 2 && !zfg_shown_ && pending_.size() > size_t(primed_ ? 0 : reserve_)) {
        consume(pending_.front()); // the block and its flip together, as the board delivers them
        pending_.pop_front();
        min_depth_ = std::min<uint32_t>(min_depth_, uint32_t(pending_.size()));
        zfg_ ^= 0x01;
        zfg_shown_ = true;
        primed_ = true;
    }
    return uint8_t(fg_ | (~zfg_ << 7) | 0x7e);
}

// A block of the previous cabinet's (in buffer0_) has landed: into the shared
// RAM with a zfg flip (MAME), or, paced, queued: the game gets it with its
// flip at its read (fg_r), one a frame, so the data it reads is always the
// block that flip announces (the other cars' data steps once a frame).
void CommBoard::land_block(int frame_offset, int frame_size, bool reply) {
    Block b;
    b.offset = uint16_t(frame_offset);
    b.reply = reply;
    b.size = uint16_t(frame_size);
    b.data.assign(buffer0_ + 1, buffer0_ + 1 + std::min<int>(frame_size, int(sizeof buffer0_) - 1));
    if (!paced_) {
        write_block(b);
        zfg_ ^= 0x01;
        return;
    }
    if (flag_only_) { // 0.11.0's pacing (comparisons): the data at once, only its flip held
        write_block(b);
        b.data.clear();
    }
    pending_.push_back(std::move(b));
    while (pending_.size() > size_t(2 + reserve_)) { // further behind: the oldest goes in without a flip
        consume(pending_.front());
        pending_.pop_front();
    }
}

// Paced: a queued block reaches the game, and a SLAVE answers it now, with
// its state of this frame. Answered on arrival instead (MAME), a burst of
// blocks got the same state twice and a gap then: the MASTER saw this
// cabinet's data stall and skip (39% of its frames moved by one step,
// measured at 100 +- 15 ms); answered here, once a frame.
void CommBoard::consume(const Block &b) {
    write_block(b);
    if (b.reply && linkalive_ == 0x01) send_data(linkid_, 0x2000, b.size, b.size + 1);
    if (tokens_owed_ && linkalive_ == 0x01) { // the token, with the answer (one send for both)
        const int n = std::min<int>(b.size + 1, int(sizeof buffer0_));
        std::memset(buffer0_, 0, size_t(n));
        buffer0_[0] = 0xfc;
        buffer0_[1] = token_byte_;
        send_frame(n);
        tokens_owed_ = 0;
    }
    if (transport_) transport_->flush_now(); // the answer and the token: one send, now
}

// A SLAVE passes the MASTER's 0xFC token on (MAME: at once). Paced with
// deferred tokens, it goes with the SLAVE's next answer instead: nothing
// waits for it without frame sync, and the two then share one send.
void CommBoard::forward_token(bool is_slave, int data_size) {
    if (paced_ && defer_tokens_ && is_slave) {
        token_byte_ = buffer0_[1];
        tokens_owed_ = 1;
        return;
    }
    send_frame(data_size);
}

void CommBoard::write_block(const Block &b) {
    for (size_t j = 0; j < b.data.size(); j++) shared_[(b.offset + j) & 0x3fff] = b.data[j];
}

void CommBoard::vblank() {
    // adaptive reserve: every 300 frames, more blocks held back if frames went
    // without one, fewer (one block dropped) if there was always one to spare
    if (paced_ && adaptive_ && primed_) {
        if (!zfg_shown_) ++starved_;
        if (++window_ >= 300) {
            if (starved_ >= 2 && reserve_ < 6) {
                ++reserve_;
                primed_ = false; // wait for the larger reserve
                ++reserve_changes_;
            } else if (starved_ == 0 && min_depth_ >= 1 && reserve_ > 0) {
                --reserve_;
                if (!pending_.empty()) {
                    consume(pending_.front());
                    pending_.pop_front();
                }
                ++reserve_changes_;
            }
            window_ = starved_ = 0;
            min_depth_ = 255;
        }
    }
    fg_reads_ = 0;
    zfg_shown_ = false;
    comm_tick();
}

void CommBoard::lost() {
    linkalive_ = 0x02;
    linktimer_ = 0x00;
}

void CommBoard::comm_tick() {
    if (linkenable_ != 0x01 || !transport_) return;
    transport_->poll();
    const int frame_start = 0x2000;
    const int frame_size = shared_[0x13] << 8 | shared_[0x12];
    const int frame_offset = frame_start | shared_[0x15] << 8 | shared_[0x14];
    const int data_size = frame_size + 1;
    int recv = 0;
    int idx = 0;

    // EPR-16726 uses fg for master/slave
    const bool is_master = (fg_ == 0x01 || shared_[1] == 0x01);
    const bool is_slave = (fg_ == 0x00 && shared_[1] == 0x02);
    const bool is_relay = (fg_ == 0x00 && shared_[1] == 0x00);

    if (linkalive_ == 0x02) {
        // link failed...
        shared_[0] = 0xff;
        return;
    } else if (linkalive_ == 0x00) {
        // link not yet established...
        shared_[0] = 0x00;
        shared_[2] = 0xff;
        shared_[3] = 0xff;

        // if both lines are there check ring
        if (transport_->rx_open() && transport_->tx_open()) {
            zfg_ ^= 0x01;

            // try to read one message
            recv = read_frame(data_size);
            while (recv > 0) {
                // check if message id
                idx = buffer0_[0];

                // 0xFF - link id
                if (idx == 0xff) {
                    if (is_master) {
                        // master gets first id and starts next state
                        linkid_ = 0x01;
                        linkcount_ = buffer0_[1];
                        linktimer_ = 0x00;
                    } else {
                        // slave get own id, relay does nothing
                        if (is_slave) buffer0_[1]++;

                        // forward message to other nodes
                        send_frame(data_size);
                    }
                }

                // 0xFE - link size
                else if (idx == 0xfe) {
                    if (is_slave) {
                        // fetch linkid and linkcount, then decrease linkid
                        linkid_ = buffer0_[1];
                        linkcount_ = buffer0_[2];
                        buffer0_[1]--;

                        // forward message to other nodes
                        send_frame(data_size);
                    } else if (is_relay) {
                        // fetch linkid and linkcount, then decrease linkid
                        linkid_ = 0x00;
                        linkcount_ = buffer0_[2];

                        // forward message to other nodes
                        send_frame(data_size);
                    }

                    // consider it done
                    linkalive_ = 0x01;

                    // write to shared mem
                    shared_[0] = 0x01;
                    shared_[2] = linkid_;
                    shared_[3] = linkcount_;
                }

                if (linkalive_ == 0x00) recv = read_frame(data_size);
                else recv = 0;
            }

            // if we are master and link is not yet established
            if (is_master && (linkalive_ == 0x00)) {
                if (linktimer_ == 0x01) {
                    // send first packet
                    buffer0_[0] = 0xff;
                    buffer0_[1] = 0x01;
                    buffer0_[2] = 0x00;
                    send_frame(data_size);
                } else if (linktimer_ == 0x00) {
                    // send second packet
                    buffer0_[0] = 0xfe;
                    buffer0_[1] = linkcount_;
                    buffer0_[2] = linkcount_;
                    send_frame(data_size);

                    // consider it done
                    linkalive_ = 0x01;

                    // write to shared mem
                    shared_[0] = 0x01;
                    shared_[2] = linkid_;
                    shared_[3] = linkcount_;
                } else if (linktimer_ > 0x01) {
                    // decrease delay timer
                    linktimer_--;
                }
            }
        }
    }

    // if link established
    if (linkalive_ == 0x01) {
        const auto deadline = std::chrono::steady_clock::now() + kSyncTimeout;
        do {
            // try to read a message
            recv = read_frame(data_size);
            while (recv > 0) {
                // check if valid id
                idx = buffer0_[0];
                if (idx >= 0 && idx <= linkcount_) {
                    // save message to "ring buffer"
                    land_block(frame_offset, frame_size, is_slave);
                    if (is_slave && !paced_) send_data(linkid_, frame_start, frame_size, data_size);
                    else if (is_relay) send_frame(data_size);
                } else if (idx == 0xfc) {
                    // 0xFC - VSYNC
                    linktimer_ = 0x00;
                    // forward message to other nodes
                    if (!is_master) forward_token(is_slave, data_size);
                }

                // try to read another message
                recv = read_frame(data_size);
            }
            if (linktimer_ == 0x01) {
                if (std::chrono::steady_clock::now() > deadline) { lost(); break; } // MAME waits forever
                std::this_thread::yield();
                transport_->poll();
            }
        } while (linktimer_ == 0x01);

        if (linkalive_ != 0x01) return;

        // enable wait for vsync
        linktimer_ = framesync_;

        if (is_master) {
            // update "ring buffer" if link established
            send_data(linkid_, frame_start, frame_size, data_size);

            // send vsync
            buffer0_[0] = 0xfc;
            buffer0_[1] = 0x01;
            send_frame(data_size);
            transport_->flush_now(); // the block and its token: one send
        }

        zfg_delay_ = 0x02;
    }
}

void CommBoard::read_fg() {
    if (zfg_delay_ > 0x00) {
        zfg_delay_--;
        return;
    }
    if (linkalive_ != 0x01 || !transport_) return;
    // The guest polls fg while it waits for the other cabinets' data. MAME
    // reads its sockets right here; our non-blocking transport only fetches
    // what arrived when polled, and without frame sync nothing else polled it
    // until the next vblank: a guest waiting inside a frame waited out its own
    // timeout instead (6 to 9 s stalls entering the selection, measured
    // through the relay). At most once a millisecond: the guest reads fg in
    // tight loops.
    if (const auto now = std::chrono::steady_clock::now(); now - last_fg_poll_ >= std::chrono::milliseconds(1)) {
        last_fg_poll_ = now;
        transport_->poll();
    }
    const int frame_start = 0x2000;
    const int frame_size = shared_[0x13] << 8 | shared_[0x12];
    const int frame_offset = frame_start | shared_[0x15] << 8 | shared_[0x14];
    const int data_size = frame_size + 1;

    const bool is_master = (fg_ == 0x01 || shared_[1] == 0x01);
    const bool is_slave = (fg_ == 0x00 && shared_[1] == 0x02);
    const bool is_relay = (fg_ == 0x00 && shared_[1] == 0x00);

    const auto deadline = std::chrono::steady_clock::now() + kSyncTimeout;
    do {
        int recv = read_frame(data_size);
        while (recv > 0) {
            const int idx = buffer0_[0];
            if (idx >= 0 && idx <= linkcount_) {
                land_block(frame_offset, frame_size, is_slave);
                if (is_slave && !paced_) send_data(linkid_, frame_start, frame_size, data_size);
                else if (is_relay) send_frame(data_size);
            } else if (idx == 0xfc) {
                linktimer_ = 0x00;
                if (!is_master) forward_token(is_slave, data_size);
            }
            recv = read_frame(data_size);
        }
        if (linktimer_ == 0x01) {
            if (std::chrono::steady_clock::now() > deadline) { lost(); break; }
            std::this_thread::yield();
            transport_->poll();
        }
    } while (linktimer_ == 0x01);
}

// One whole frame into buffer0_: data_size, or 0 while it is not all here
// (what has come is kept). The previous cabinet closing the line loses the
// link.
int CommBoard::read_frame(int data_size) {
    if (!transport_) return 0;
    if (!transport_->rx_open()) {
        if (linkalive_ == 0x01) lost(); // rx connection lost
        return 0;
    }
    data_size = std::min<int>(data_size, int(sizeof buffer0_));
    uint8_t chunk[4096];
    while (int(rx_.size()) < data_size) {
        const int n = transport_->read(chunk, std::min<int>(int(sizeof chunk), data_size - int(rx_.size())));
        if (n < 0) {
            if (linkalive_ == 0x01) lost(); // rx connection lost
            return 0;
        }
        if (n == 0) return 0;
        rx_.insert(rx_.end(), chunk, chunk + n);
    }
    std::memcpy(buffer0_, rx_.data(), size_t(data_size));
    rx_.erase(rx_.begin(), rx_.begin() + data_size);
    return data_size;
}

void CommBoard::send_data(uint8_t frame_type, int frame_start, int frame_size, int data_size) {
    buffer0_[0] = frame_type;
    for (int i = 0x0000; i < frame_size && 1 + i < int(sizeof buffer0_); i++) buffer0_[1 + i] = shared_[(frame_start + i) & 0x3fff];
    send_frame(data_size);
}

void CommBoard::send_frame(int data_size) {
    if (!transport_) return;
    if (!transport_->tx_open()) {
        if (linkalive_ == 0x01) lost(); // tx connection lost
        return;
    }
    if (!transport_->write(buffer0_, std::min<int>(data_size, int(sizeof buffer0_)))) {
        if (linkalive_ == 0x01) lost(); // tx connection lost
    }
}

} // namespace rt
