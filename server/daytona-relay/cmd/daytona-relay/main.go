// daytona-relay: the server. Players connect here (the game's Online mode,
// or the daytona-link helper) and join a room by its code, up to 8. The
// room's owner starts a race: the server forms the ring of everyone in the
// room (the owner first: the MASTER) and passes each player's game bytes to
// the next player. Who joins during a race waits for the next one.
//
// Configuration: flags, or the environment (Docker):
//
//	DAYTONA_PORT (7777), DAYTONA_PASSWORD (none), DAYTONA_MAX_ROOMS (100),
//	DAYTONA_MAX_CONNECTIONS (1000), DAYTONA_MAX_PER_IP (16),
//	DAYTONA_MAX_ROOMS_PER_IP (8), DAYTONA_ROOM_ALONE_MIN (30),
//	DAYTONA_ROOM_IDLE_MIN (120), DAYTONA_JOIN_TRIES_PER_MIN (20), and the
//	race settings' defaults (see package settings). The abuse limits are in
//	guard.go.
//
// The game asks the defaults (op=info) and creates a room with the owner's
// choices over them (op=create set=...). A room is gone when its last
// player leaves.
package main

import (
	"bufio"
	"bytes"
	"crypto/rand"
	"crypto/subtle"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"daytona-relay/linkcodec"
	"daytona-relay/proto"
	"daytona-relay/settings"
)

const (
	handshakeTimeout = 10 * time.Second
	idleTimeout      = 30 * time.Second // no frame (not even a ping) for this long: gone
	pingEvery        = 2 * time.Second  // also measures each player's latency (TypePong)
	sendQueue        = 512              // frames waiting for a slow client before it is dropped
	// resumeGrace: a racer whose connection drops keeps its place this long
	// (star mode, where the relay restarts the codecs itself)
	resumeGrace = 10 * time.Second
)

func newKey() string {
	b := make([]byte, 12)
	rand.Read(b)
	return hex.EncodeToString(b)
}

type client struct {
	conn net.Conn
	addr string
	ip   string // addr's address, without the port
	name string
	room *room
	out  chan proto.Frame
	once sync.Once
	done chan struct{}
	rtt  atomic.Int64 // latency to this server, round trip, smoothed, in µs; 0: not measured yet

	// star mode (server.star), under server.mu: this player's stream decoded,
	// the stream composed for it encoded, its latest own slot
	dec, enc linkcodec.Codec
	own      []byte

	key  string // to come back into the race after a drop (op=rejoin)
	away bool   // dropped from a race, its place kept until resumeGrace (star mode)

	udpState // numbered game data, by TCP and UDP (udp.go); under server.mu

	race raceStats // what this server saw of it in the current race, for the log (racelog.go)

	lim   limits         // its frames' rate (guard.go), its reading goroutine only
	dgram datagramLimits // its datagrams' rate (guard.go), under server.mu
}

// latencyMs: the client's round trip in ms, -1 before the first measure.
func (c *client) latencyMs() int {
	us := c.rtt.Load()
	if us == 0 {
		return -1
	}
	return int((us + 500) / 1000)
}

func (c *client) close() {
	c.once.Do(func() {
		close(c.done)
		c.conn.Close()
	})
}

// send queues a frame; a client too slow to keep up is disconnected rather
// than slowing the whole ring down.
func (c *client) send(f proto.Frame) {
	select {
	case c.out <- f:
	case <-c.done:
	default:
		log.Printf("%s: send queue full, disconnecting", c.addr)
		c.close()
	}
}

type room struct {
	code    string
	game    string    // the online version of the games in it (Hello.Game)
	members []*client // everyone in the room, in join order
	owner   *client   // starts races (Begin); nil in the helper's rooms
	ring    []*client // the race's cabinets in ring order (the owner first); nil: no race formed
	shown   [2][]int  // the latencies the members were last told, names and waiting (refreshLatency)
	epoch   uint32    // changes with every new ring (and when one breaks)
	auto    int       // the daytona-link helper's rooms: a ring forms on its own at this many; 0: the owner starts

	race    settings.Race   // the room's settings: the environment's, with its creator's choices over them
	eeprom  []proto.Setting // race.EEPROM()
	created time.Time

	creatorIP  string    // rooms per address (guard.go)
	aloneSince time.Time // when one member was left (or the room was created)
	active     time.Time // the last join, race or game data: a room idle too long is closed (guard.go)

	ringSince, ringLogged time.Time // the ring's start, its last lines in the log (racelog.go)
}

type server struct {
	mu       sync.Mutex
	rooms    map[string]*room
	password string
	maxRooms int

	// connections: at most maxConns in all and maxPerIP from one address
	connMu             sync.Mutex
	perIP              map[string]int
	conns              int
	maxConns, maxPerIP int

	race settings.Race // the environment's race settings: every room's defaults
	// the newest game online version (Hello.Game) a race has started with
	// since this server started, under mu: op=info tells it (game_online=),
	// so that an older game can say it needs updating before it tries a room
	newestGame int
	now        func() time.Time // time.Now; a parameter for tests
	// star: the game's rooms get every other car's latest data in one hop
	// (forward); the helper's rooms stay a plain ring
	star bool

	udpConn    *net.UDPConn       // the UDP side of the port (udp.go); nil: TCP only
	udpByAddr  map[string]*client // under mu
	byKey      map[string]*client // every member by its key (Status.Key), under mu
	udpPending []*client          // players with messages for their next datagram (udp.go), under mu
	udpRepeat  int                // earlier messages repeated in each datagram

	// abuse limits (guard.go), under mu
	failures                       map[string]*tries
	maxRoomsPerIP, joinTriesPerMin int
	roomAlone, roomIdle            time.Duration
}

func newServer(race settings.Race, password string, maxRooms, maxConns, maxPerIP int) *server {
	return &server{rooms: map[string]*room{}, password: password, maxRooms: maxRooms, race: race, now: time.Now,
		perIP: map[string]int{}, maxConns: maxConns, maxPerIP: maxPerIP,
		udpByAddr: map[string]*client{}, byKey: map[string]*client{}, failures: map[string]*tries{},
		udpRepeat: defaultRepeat, maxRoomsPerIP: defaultRoomsPerIP, joinTriesPerMin: defaultJoinTries, roomAlone: defaultRoomAlone, roomIdle: defaultRoomIdle}
}

func (s *server) join(c *client, h proto.Hello) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.rooms[h.Room]
	switch {
	case r == nil && h.Op == "join":
		return proto.Errf("not_found", "room %q does not exist (is the code right? is its creator still connected?)", h.Room)
	case r != nil && h.Op == "create":
		return proto.Errf("exists", "room %q already exists", h.Room)
	case r == nil:
		if len(s.rooms) >= s.maxRooms {
			return proto.Errf("server_full", "server full: %d rooms at most", s.maxRooms)
		}
		if c.ip != "" && s.roomsOf(c.ip) >= s.maxRoomsPerIP {
			return proto.Errf("too_many_rooms", "%d rooms already open from %s (%d at most)", s.roomsOf(c.ip), c.ip, s.maxRoomsPerIP)
		}
		race := s.race
		if h.Op == "create" {
			if err := race.Apply(h.Settings); err != nil {
				return proto.Errf("settings", "%v", err)
			}
		}
		now := s.now()
		r = &room{code: h.Room, game: h.Game, race: race, eeprom: race.EEPROM(), created: now,
			creatorIP: c.ip, aloneSince: now, active: now}
		log.Printf("room %s: created (%s)", h.Room, race)
		if h.Op == "auto" {
			r.auto = h.Players
		}
		s.rooms[h.Room] = r
	case (r.auto > 0) != (h.Op == "auto"):
		// the helper's rooms pass raw link bytes, the game's compressed ones (app/link_codec.h)
		return proto.Errf("room_kind", "room %q belongs to the other kind of client (game or daytona-link helper)", h.Room)
	case h.Op == "auto" && r.auto != h.Players:
		return proto.Errf("size", "room %q is not for %d players", h.Room, h.Players)
	}
	if r.game == "" {
		r.game = h.Game
	}
	if h.Game != "" && h.Game != r.game {
		return proto.ErrDetail("game_version", []string{"room_online=" + r.game},
			"room %q is for game online version %s, not %s", h.Room, r.game, h.Game)
	}
	if limit := r.capacity(); len(r.members) >= limit {
		return proto.Errf("full", "room %q is full (%d players)", h.Room, limit)
	}
	r.members = append(r.members, c)
	c.room = r
	c.key = newKey()
	s.byKey[c.key] = c
	r.active = s.now()
	if r.owner == nil && r.auto == 0 {
		r.owner = c
	}
	log.Printf("room %s: %q (%s) joined (%d in the room)", r.code, c.name, c.addr, len(r.members))
	if r.auto > 0 && r.ring == nil && len(r.members) == r.auto {
		s.formRing(r)
	} else {
		s.notify(r)
	}
	return nil
}

// capacity: the most players a room takes.
func (r *room) capacity() int {
	if r.auto > 0 {
		return r.auto
	}
	return proto.MaxPlayers
}

func (r *room) inRing(c *client) int { // c's place in the ring, 1-based; 0: not in it
	for i, m := range r.ring {
		if m == c {
			return i + 1
		}
	}
	return 0
}

// info: op=info's answer, the race settings' defaults and, once a race has
// started here, game_online= (the newest game online version raced with).
func (s *server) info() string {
	s.mu.Lock()
	newest := s.newestGame
	s.mu.Unlock()
	out := s.race.Info()
	if newest > 0 {
		out += "game_online=" + strconv.Itoa(newest) + "\n"
	}
	return out
}

// begin: the owner starts a race with everyone in the room.
func (s *server) begin(c *client) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := c.room
	if r == nil || r.owner != c || len(r.members) < proto.MinPlayers {
		return
	}
	s.formRing(r)
	if v, err := strconv.Atoi(r.game); err == nil && v > s.newestGame {
		s.newestGame = v
	}
	log.Printf("room %s: race started by %q with %d players", r.code, c.name, len(r.ring))
}

// formRing: a new ring of everyone in the room, the owner first. Caller holds s.mu.
func (s *server) formRing(r *room) {
	s.logRing(r, "ended (a new race)")
	r.ring = r.ring[:0]
	if r.owner != nil {
		r.ring = append(r.ring, r.owner)
	}
	for _, m := range r.members {
		if m != r.owner {
			r.ring = append(r.ring, m)
		}
	}
	r.epoch++                  // a new ring: data of the old one is dropped
	for _, m := range r.ring { // the games' codecs start again with the ring (app/relay_link.cpp)
		m.dec.Reset()
		m.enc.Reset()
		m.own = nil
		m.race.reset()
	}
	r.ringSince, r.ringLogged = time.Now(), time.Now()
	r.active = s.now()
	s.notify(r)
}

func (s *server) leave(c *client) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := c.room
	if r == nil {
		return
	}
	if s.star && r.auto == 0 && r.inRing(c) > 0 && !c.away {
		// a racer dropped: its place stays for resumeGrace (op=rejoin), the race goes on
		c.away = true
		c.race.drops.Add(1)
		log.Printf("room %s: %q (%s) dropped, its place kept for %v", r.code, c.name, c.addr, resumeGrace)
		time.AfterFunc(resumeGrace, func() {
			s.mu.Lock()
			defer s.mu.Unlock()
			if c.away && c.room == r {
				c.away = false
				s.remove(c)
			}
		})
		return
	}
	s.remove(c)
}

// rejoin: a dropped racer back with its key takes its place again, the race
// going on (the same epoch; its codecs start again on both sides).
func (s *server) rejoin(c *client, h proto.Hello) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.rooms[h.Room]
	if r == nil {
		return proto.Errf("not_found", "room %q does not exist any more", h.Room)
	}
	var old *client
	for _, m := range r.members {
		if h.Key != "" && subtle.ConstantTimeCompare([]byte(m.key), []byte(h.Key)) == 1 && s.star && r.inRing(m) > 0 {
			old = m
		}
	}
	if old == nil {
		return proto.Errf("not_found", "no place kept in room %q for this player", h.Room)
	}
	c.name, c.key, c.room = old.name, old.key, r
	s.byKey[c.key] = c
	s.forgetAddr(old)
	c.rtt.Store(old.rtt.Load())
	c.race.copyFrom(&old.race)
	c.race.backs.Add(1)
	for i, m := range r.members {
		if m == old {
			r.members[i] = c
		}
	}
	for i, m := range r.ring {
		if m == old {
			r.ring[i] = c
		}
	}
	if r.owner == old {
		r.owner = c
	}
	wasAway := old.away
	old.away, old.room = false, nil
	if !wasAway { // back before this server saw the old connection drop: close that one
		old.close()
	}
	log.Printf("room %s: %q (%s) back in its race", r.code, c.name, c.addr)
	s.notify(r)
	return nil
}

// remove takes c out of its room for good. Caller holds s.mu.
func (s *server) remove(c *client) {
	r := c.room
	if r == nil {
		return
	}
	for i, m := range r.members {
		if m == c {
			r.members = append(r.members[:i], r.members[i+1:]...)
			break
		}
	}
	c.room = nil
	if s.byKey[c.key] == c {
		delete(s.byKey, c.key)
	}
	s.forgetAddr(c)
	if len(r.members) == 1 {
		r.aloneSince = s.now()
	}
	if r.owner == c { // the next in line owns the room
		r.owner = nil
		if len(r.members) > 0 && r.auto == 0 {
			r.owner = r.members[0]
		}
	}
	if r.inRing(c) > 0 { // the race's ring is broken: back to the room until the owner starts again
		s.logRing(r, fmt.Sprintf("ended (%q left)", c.name))
		r.ring = nil
		r.epoch++
		r.active = s.now()
	}
	log.Printf("room %s: %q (%s) left (%d in the room)", r.code, c.name, c.addr, len(r.members))
	if len(r.members) == 0 {
		delete(s.rooms, r.code)
		return
	}
	s.notify(r)
}

// notify tells every member the room's state, in the current epoch. Caller holds s.mu.
func (s *server) notify(r *room) {
	var names, waiting []string
	line := map[*client]int{} // each member's line in names
	add := func(m *client) {
		names = append(names, m.name)
		line[m] = len(names)
	}
	if r.ring != nil {
		for _, m := range r.ring {
			add(m)
		}
		for _, m := range r.members {
			if r.inRing(m) == 0 {
				waiting = append(waiting, m.name)
			}
		}
	} else { // the order a race would take: the owner first
		if r.owner != nil {
			add(r.owner)
		}
		for _, m := range r.members {
			if m != r.owner {
				add(m)
			}
		}
	}
	latency, waitingLatency := r.latencies()
	r.shown = [2][]int{latency, waitingLatency}
	for _, m := range r.members {
		pos := r.inRing(m)
		st := proto.Status{Joined: len(r.members), Size: len(r.ring), Position: pos, Ready: pos > 0,
			EEPROM: r.eeprom, Names: names, AnyStart: r.race.AnyStart(), Owner: m == r.owner,
			Racing: r.ring != nil, Waiting: waiting, Me: line[m], Latency: latency, WaitingLatency: waitingLatency,
			Key: m.key, Resume: s.star && r.auto == 0}
		m.send(proto.Frame{Type: proto.TypeStatus, Epoch: r.epoch, Payload: st.Encode()})
	}
}

// order: the members as the status lists them (names, then waiting).
func (r *room) order() (names, waiting []*client) {
	if r.ring != nil {
		names = append(names, r.ring...)
		for _, m := range r.members {
			if r.inRing(m) == 0 {
				waiting = append(waiting, m)
			}
		}
		return names, waiting
	}
	if r.owner != nil {
		names = append(names, r.owner)
	}
	for _, m := range r.members {
		if m != r.owner {
			names = append(names, m)
		}
	}
	return names, nil
}

func (r *room) latencies() (names, waiting []int) {
	n, w := r.order()
	for _, m := range n {
		names = append(names, m.latencyMs())
	}
	for _, m := range w {
		waiting = append(waiting, m.latencyMs())
	}
	return names, waiting
}

// refreshLatency tells each room's members their latencies when they have
// changed by a few ms since the last status (every pingEvery).
func (s *server) refreshLatency() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range s.rooms {
		if r.auto > 0 || len(r.members) == 0 {
			continue
		}
		names, waiting := r.latencies()
		if latencyMoved(r.shown, names, waiting) {
			s.notify(r)
		}
	}
}

// latencyMoved: a player measured for the first time, or one moved by 3 ms
// or more (jitter alone sends no status).
func latencyMoved(prev [2][]int, names, waiting []int) bool {
	for i, now := range [2][]int{names, waiting} {
		if len(now) != len(prev[i]) {
			return true
		}
		for k := range now {
			if d := now[k] - prev[i][k]; (now[k] < 0) != (prev[i][k] < 0) || d >= 3 || d <= -3 {
				return true
			}
		}
	}
	return false
}

// masterOnly: the control messages only the race's MASTER sends, the
// course and the gearbox for the whole room (the game's CourseSync and
// GearboxSync, src/runtime/online_settings.h): "cursor=", "course=",
// "gearcursor=", "gearbox=".
func masterOnly(payload []byte) bool {
	for _, key := range []string{"cursor=", "course=", "gearcursor=", "gearbox="} {
		if bytes.HasPrefix(payload, []byte(key)) {
			return true
		}
	}
	return false
}

// broadcast passes a control frame from c to every other member of its room.
// The course and the gearbox are the MASTER's (the ring's first) alone: from
// anyone else they are dropped, so that a modified game cannot choose for
// the room.
func (s *server) broadcast(c *client, f proto.Frame) {
	s.mu.Lock()
	r := c.room
	if r == nil || f.Epoch != r.epoch || r.inRing(c) == 0 || (masterOnly(f.Payload) && r.ring[0] != c) {
		s.mu.Unlock()
		return
	}
	others := make([]*client, 0, len(r.ring)-1)
	for _, m := range r.ring {
		if m != c {
			others = append(others, m)
		}
	}
	s.mu.Unlock()
	for _, m := range others {
		m.send(f)
	}
}

// forward passes game bytes from c to the next member of its ring.
func (s *server) forward(c *client, f proto.Frame) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if c.udp { // numbered: in order, once, from TCP or UDP (udp.go)
		if len(f.Payload) < 4 {
			return
		}
		s.receiveNumbered(c, f.Epoch, binary.BigEndian.Uint32(f.Payload), f.Payload[4:], false)
		return
	}
	s.forwardLocked(c, f)
}

// forwardLocked passes one game message of c's on. Caller holds s.mu.
func (s *server) forwardLocked(c *client, f proto.Frame) {
	c.race.in.Add(1)
	r := c.room
	pos := 0
	if r != nil {
		pos = r.inRing(c)
	}
	if pos == 0 || f.Epoch != r.epoch {
		return // an old ring's data, or c is not in the race
	}
	r.active = s.now() // games exchanging data: never idle
	next := r.ring[pos%len(r.ring)]
	if s.star && r.auto == 0 && next != nil {
		f = s.compose(r, pos-1, c, next, f)
	}
	if next != nil && f.Payload != nil {
		s.sendData(next, f)
	}
}

// compose (star mode): the block c sent its next player, with every other
// car's slot replaced by that car's latest own data. In the ring, slot j of a
// block holds the data of the cabinet j places behind its sender, passed on
// j times; here each comes from that cabinet's last block, one hop old.
// Slot 0 is c's own; the slots past the others are left as c sent them.
// Caller holds s.mu.
func (s *server) compose(r *room, at int, c, next *client, f proto.Frame) proto.Frame {
	frame, err := c.dec.Decode(f.Payload)
	if err != nil {
		f.Payload = nil // a stream out of step: drop it (the game sees the link stall)
		return f
	}
	if linkcodec.IsBlock(frame) && len(frame) >= 1+len(r.ring)*linkcodec.Slot {
		c.own = append(c.own[:0], frame[1:1+linkcodec.Slot]...)
		n := len(r.ring)
		for j := 1; j <= n-2; j++ {
			src := r.ring[((at-j)%n+n)%n]
			if len(src.own) == linkcodec.Slot {
				copy(frame[1+j*linkcodec.Slot:], src.own)
			}
		}
	}
	f.Payload = next.enc.Encode(nil, frame)
	return f
}

// admit counts a new connection in, or refuses it.
func (s *server) admit(ip string) error {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	if s.conns >= s.maxConns {
		return proto.Errf("server_full", "server full: %d connections at most", s.maxConns)
	}
	if s.perIP[ip] >= s.maxPerIP {
		return proto.Errf("too_many", "too many connections from %s (%d at most)", ip, s.maxPerIP)
	}
	s.conns++
	s.perIP[ip]++
	return nil
}

func (s *server) release(ip string) {
	s.connMu.Lock()
	defer s.connMu.Unlock()
	s.conns--
	if s.perIP[ip]--; s.perIP[ip] <= 0 {
		delete(s.perIP, ip)
	}
}

func (s *server) handle(conn net.Conn) {
	if tc, ok := conn.(*net.TCPConn); ok {
		tc.SetNoDelay(true)
		tc.SetKeepAlive(true)
	}
	c := &client{conn: conn, addr: conn.RemoteAddr().String(), out: make(chan proto.Frame, sendQueue), done: make(chan struct{}),
		lim: newLimits(time.Now())}
	c.ip, _, _ = net.SplitHostPort(c.addr)
	defer c.close()

	br := bufio.NewReaderSize(conn, 32*1024)
	conn.SetReadDeadline(time.Now().Add(handshakeTimeout))
	line, err := proto.ReadLine(br, proto.MaxHello)
	if err != nil {
		return
	}
	h, err := proto.ParseHello(strings.TrimSpace(line))
	if err == nil && s.tooManyTries(c.ip) {
		err = proto.Errf("slow_down", "too many failed attempts from %s: wait a minute", c.ip)
	} else if err == nil && s.password != "" && subtle.ConstantTimeCompare([]byte(h.Password), []byte(s.password)) != 1 {
		err = proto.Errf("password", "wrong server password")
		s.failed(c.ip, err)
	}
	if err != nil {
		conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		proto.WriteFrame(conn, proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(err)})
		log.Printf("%s: refused: %v", c.addr, err)
		return
	}
	c.name = h.Name
	c.udp = h.UDP
	if h.Op == "info" { // the defaults the game shows before creating a room
		conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		proto.WriteFrame(conn, proto.Frame{Type: proto.TypeInfo, Payload: []byte(s.info())})
		return
	}

	// writer: the only goroutine writing to conn from here on
	go func() {
		bw := bufio.NewWriterSize(conn, 32*1024)
		ping := time.NewTicker(pingEvery)
		defer ping.Stop()
		write := func(f proto.Frame) bool {
			conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
			return proto.WriteFrame(bw, f) == nil && f.Type != proto.TypeError
		}
		for {
			select {
			case f := <-c.out:
				ok := write(f)
				for n := len(c.out); ok && n > 0; n-- { // what else is queued, then one flush
					ok = write(<-c.out)
				}
				if bw.Flush() != nil || !ok {
					c.close()
					return
				}
			case <-ping.C: // stamped: the game sends it back (TypePong), the round trip is its latency
				stamp := make([]byte, 8)
				binary.BigEndian.PutUint64(stamp, uint64(time.Now().UnixNano()))
				if !write(proto.Frame{Type: proto.TypePing, Payload: stamp}) || bw.Flush() != nil {
					c.close()
					return
				}
			case <-c.done:
				return
			}
		}
	}()

	join := s.join
	if h.Op == "rejoin" {
		join = s.rejoin
	}
	if err := join(c, h); err != nil {
		s.failed(c.ip, err)
		c.send(proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(err)})
		log.Printf("%s: refused: %v", c.addr, err)
		select {
		case <-c.done:
		case <-time.After(5 * time.Second):
		}
		return
	}
	defer s.leave(c)

	for {
		conn.SetReadDeadline(time.Now().Add(idleTimeout))
		f, err := proto.ReadFrame(br)
		if err != nil {
			return
		}
		if !c.lim.allow(f, time.Now()) { // a flood: this connection goes, not the others (guard.go)
			s.mu.Lock()
			where := "-"
			if c.room != nil {
				where = c.room.code
			}
			s.mu.Unlock()
			log.Printf("room %s: %q (%s) sends too much (frame type %q), disconnecting", where, c.name, c.addr, f.Type)
			c.send(proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(
				proto.Errf("flood", "disconnected: your game sent too much data"))})
			select {
			case <-c.done:
			case <-time.After(2 * time.Second):
			}
			return
		}
		switch f.Type {
		case proto.TypeData:
			s.forward(c, f)
			if br.Buffered() < 7 { // nothing more to read at once: this batch's datagrams go now
				s.flushDatagrams()
			}
		case proto.TypeBegin:
			s.begin(c)
		case proto.TypeControl:
			if len(f.Payload) <= 256 {
				s.broadcast(c, f)
			}
		case proto.TypePong:
			if len(f.Payload) == 8 {
				sent := int64(binary.BigEndian.Uint64(f.Payload))
				if us := (time.Now().UnixNano() - sent) / 1000; us > 0 && us < 60_000_000 {
					c.race.latency(us)
					if old := c.rtt.Load(); old == 0 {
						c.rtt.Store(us)
					} else {
						c.rtt.Store((3*old + us) / 4) // smoothed: a moment's jitter does not jump the number
					}
				}
			}
		case proto.TypePing:
		default:
			return
		}
	}
}

// serve accepts the players' connections until ln is closed.
func (s *server) serve(ln net.Listener) {
	for {
		conn, err := ln.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			log.Printf("accept: %v", err)
			time.Sleep(100 * time.Millisecond)
			continue
		}
		ip, _, _ := net.SplitHostPort(conn.RemoteAddr().String())
		if err := s.admit(ip); err != nil {
			conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
			proto.WriteFrame(conn, proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(err)})
			conn.Close()
			log.Printf("%s: refused: %v", conn.RemoteAddr(), err)
			continue
		}
		go func() {
			defer s.release(ip)
			s.handle(conn)
		}()
	}
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds | log.LUTC) // UTC: set against the players' logs
	port := flag.Int("port", envInt("DAYTONA_PORT", 7777), "TCP port to listen on (DAYTONA_PORT)")
	password := flag.String("password", os.Getenv("DAYTONA_PASSWORD"), "password the players must give, optional (DAYTONA_PASSWORD)")
	maxRooms := flag.Int("max-rooms", envInt("DAYTONA_MAX_ROOMS", 100), "rooms open at the same time, at most (DAYTONA_MAX_ROOMS)")
	maxConns := flag.Int("max-connections", envInt("DAYTONA_MAX_CONNECTIONS", 1000), "connections at the same time, at most (DAYTONA_MAX_CONNECTIONS)")
	maxPerIP := flag.Int("max-per-ip", envInt("DAYTONA_MAX_PER_IP", 16), "connections from one address, at most (DAYTONA_MAX_PER_IP)")
	roomsPerIP := flag.Int("max-rooms-per-ip", envInt("DAYTONA_MAX_ROOMS_PER_IP", defaultRoomsPerIP), "rooms open from one address, at most (DAYTONA_MAX_ROOMS_PER_IP)")
	aloneMin := flag.Int("room-alone-min", envInt("DAYTONA_ROOM_ALONE_MIN", int(defaultRoomAlone/time.Minute)), "minutes a room's creator may wait alone (DAYTONA_ROOM_ALONE_MIN)")
	idleMin := flag.Int("room-idle-min", envInt("DAYTONA_ROOM_IDLE_MIN", int(defaultRoomIdle/time.Minute)), "minutes a room may go without a race (DAYTONA_ROOM_IDLE_MIN)")
	udpRepeat := flag.Int("udp-repeat", envInt("DAYTONA_UDP_REPEAT", defaultRepeat), "earlier messages repeated in each datagram (DAYTONA_UDP_REPEAT)")
	joinTries := flag.Int("join-tries-per-min", envInt("DAYTONA_JOIN_TRIES_PER_MIN", defaultJoinTries), "failed attempts a minute from one address (DAYTONA_JOIN_TRIES_PER_MIN)")
	flag.Parse()

	race, err := settings.FromEnv(os.Getenv)
	if err != nil {
		log.Fatal(err)
	}
	s := newServer(race, *password, *maxRooms, *maxConns, *maxPerIP)
	s.maxRoomsPerIP, s.joinTriesPerMin, s.udpRepeat = *roomsPerIP, *joinTries, max(0, *udpRepeat)
	s.roomAlone, s.roomIdle = time.Duration(*aloneMin)*time.Minute, time.Duration(*idleMin)*time.Minute
	log.Printf("limits: %d rooms from one address, a room closed after %v alone or %v without a race, %d failed attempts a minute",
		s.maxRoomsPerIP, s.roomAlone, s.roomIdle, s.joinTriesPerMin)
	s.star = os.Getenv("DAYTONA_STAR") != "0"
	if s.star {
		log.Printf("star mode: each player gets every other car's latest data in one hop")
	} else {
		log.Printf("ring mode (DAYTONA_STAR=0): the cars' data passed from player to player; no rejoin")
	}

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("cannot listen on port %d: %v", *port, err)
	}
	log.Printf("daytona-relay listening on port %d (rooms of %d to %d players, password: %v)", *port, proto.MinPlayers, proto.MaxPlayers, *password != "")
	log.Printf("race settings (defaults): %s", race)
	if uc, err := net.ListenUDP("udp", &net.UDPAddr{Port: *port}); err == nil {
		s.udpConn = uc
		log.Printf("UDP on port %d too (games that ask for it)", *port)
		go s.serveUDP()
	} else {
		log.Printf("no UDP (%v): TCP only", err)
	}
	go func() {
		for range time.Tick(pingEvery) {
			s.refreshLatency()
			s.logRaces()
		}
	}()
	go func() {
		for range time.Tick(janitorEvery) {
			s.janitor()
		}
	}()
	s.serve(ln)
}
