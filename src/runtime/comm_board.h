// license:BSD-3-Clause
// copyright-holders:Ariane Fugmann
//
// The Model 2 communication board (837-10537 on Daytona USA Revision A):
// link play between cabinets. Transplanted from MAME's m2comm device in its
// M2COMM_SIMULATION mode (src/mame/sega/m2comm.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81; BSD-3-Clause, notice above kept
// as the licence requires; see THIRD_PARTY.md): the board's Z80 program
// (EPR-16726) is not run; the protocol it carries is.
//
// Cabinets form a ring: each sends to the next and receives from the one
// before. At start the master (set in the game's test mode: LINK ID) sends a
// numbering token round the ring, so every cabinet learns its id and the
// count; then every frame each one passes its block of race data on, and the
// master sends a vsync token. The shared RAM is the game's (M2Board maps it
// at 0x01a00000); the bytes travel through a LinkTransport the host provides
// (TCP in the app), which never blocks. Without a transport the board is not
// here at all: M2Board keeps its plain registers and the game sees no link.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace rt {

// The two cables: to the next cabinet and from the one before. Non-blocking.
class LinkTransport {
public:
    virtual ~LinkTransport() = default;
    virtual void poll() = 0;        // accept / connect as needed
    virtual bool rx_open() const = 0; // a cabinet is sending to us
    virtual bool tx_open() const = 0; // we are connected to the next cabinet
    // Bytes received: > 0 how many, 0 none yet, < 0 the connection closed.
    virtual int read(uint8_t *buf, int max) = 0;
    // All of buf, or false: the connection is lost.
    virtual bool write(const uint8_t *buf, int n) = 0;
};

class CommBoard {
public:
    // shared: the board's 16 KB shared RAM. frame_offset: where in the
    // received block a frame lands (0x1c0 for most games).
    explicit CommBoard(uint8_t *shared, uint16_t frame_offset = 0x1c0) : shared_(shared), frame_offset_(frame_offset) {}
    void set_transport(LinkTransport *t) { transport_ = t; }
    // Hold each cabinet to the master's frame (MAME's -comm_framesync); off
    // by default, as in MAME: cabinets run free and exchange data as it comes.
    void set_framesync(bool on) { framesync_ = on ? 1 : 0; }
    // The ring known beforehand (the Online relay tells each game its place):
    // each start of the board (cn) brings the link up at once with this id and
    // count instead of the FF/FE numbering round the ring, which waits 232
    // frames and then one round trip per cabinet (too slow for the game's own
    // boot check with long round trips). id: 1 the MASTER, else as FF/FE would
    // number it (rt::online::link_id). 0: the numbering, as MAME.
    // Over the Internet blocks arrive out of step: two in one frame flip zfg
    // back (the game reads one per frame: an even count, zero included, made
    // it miss that frame's logic tick, measured 24-56% of the ticks with 4
    // cabinets at 100 +- 15 ms), and one landing in the vblank tick flips it
    // before the game's vblank interrupt takes its reference. Paced, each
    // block's flip is held and shown at the game's next fg read after that
    // reference, at most one a frame. Off (MAME) by default; Online turns it on.
    // reserve: blocks held back once at the start (a cushion against a frame
    // with no arrival, one frame of extra delay each).
    void set_paced(bool on, int reserve = 0) {
        paced_ = on;
        reserve_ = uint8_t(reserve);
    }
    void preset_ring(int id, int count) {
        preset_id_ = uint8_t(id);
        preset_count_ = uint8_t(count);
    }

    uint8_t cn_r() const { return uint8_t(cn_ | 0xfe); }
    void cn_w(uint8_t data);
    uint8_t fg_r();
    void fg_w(uint8_t data) { fg_ = data & 1; }
    void vblank(); // MAME check_vint_irq: once per frame

    // For the host's status line.
    enum class Link { Off, Waiting, Up, Lost };
    Link link() const;
    int id() const { return linkid_; }
    int count() const { return linkcount_; }

private:
    uint8_t *shared_;
    uint16_t frame_offset_;
    LinkTransport *transport_ = nullptr;
    uint8_t zfg_ = 0, cn_ = 0, fg_ = 0, framesync_ = 0;
    uint8_t linkenable_ = 0, linkalive_ = 0, linkid_ = 0, linkcount_ = 0, zfg_delay_ = 0;
    uint16_t linktimer_ = 0;
    uint8_t preset_id_ = 0, preset_count_ = 0; // preset_ring
    bool paced_ = false, zfg_shown_ = false;   // set_paced: zfg flips held for the game's read
    uint8_t reserve_ = 0;
    struct Block {
        uint16_t offset = 0, size = 0; // size: the frame's (a SLAVE's answer)
        bool reply = false;            // a SLAVE answers it when the game takes it (consume)
        std::vector<uint8_t> data;
    };
    std::deque<Block> pending_; // paced: received blocks waiting for the game's read
  public:
    bool flag_only_ = false; // diagnostics: 0.11.0's pacing, the data written at once (M2_TEST_RELAY_FLAG_ONLY)
  private:
    bool primed_ = false; // the reserve has been built once
    uint32_t fg_reads_ = 0; // since the vblank tick
    void land_block(int frame_offset, int frame_size, bool reply);
    void consume(const Block &b);
    void write_block(const Block &b);
    std::chrono::steady_clock::time_point last_fg_poll_{}; // read_fg's transport poll, at most once a ms
    uint8_t buffer0_[0x1000] = {};
    std::vector<uint8_t> rx_; // bytes received toward the next whole frame
    void comm_tick();
    void read_fg();
    int read_frame(int data_size);
    void send_data(uint8_t frame_type, int frame_start, int frame_size, int data_size);
    void send_frame(int data_size);
    void lost();
};

} // namespace rt
