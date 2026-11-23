// daytona-relay: the server. Players connect here (the game's Online mode,
// or the daytona-link helper) and join a room by its code; once the room is
// full, the server forms the ring (in join order: the first is the MASTER)
// and passes each player's game bytes to the next player.
//
// Configuration: flags, or the environment (Docker):
//
//	DAYTONA_PORT (7777), DAYTONA_PASSWORD (none), DAYTONA_MAX_ROOMS (100),
//	DAYTONA_MAX_CONNECTIONS (1000), DAYTONA_MAX_PER_IP (16), the race
//	settings (see package settings), and the web interface (web.go):
//	DAYTONA_WEB_PORT (8080, 0: off), DAYTONA_PUBLIC_ADDRESS (none),
//	DAYTONA_WEB_PASSWORD (none: DAYTONA_PASSWORD).
//
// A room created by a game takes the environment's race settings and is
// gone when its last player leaves. A room created on the web interface
// keeps its own settings, exists before anyone joins (op=join), and is
// removed once it has stayed empty for webRoomIdle.
package main

import (
	"bufio"
	"crypto/subtle"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"daytona-relay/invite"
	"daytona-relay/proto"
	"daytona-relay/settings"
)

const (
	handshakeTimeout = 10 * time.Second
	idleTimeout      = 30 * time.Second // no frame (not even a ping) for this long: gone
	pingEvery        = 10 * time.Second
	sendQueue        = 512 // frames waiting for a slow client before it is dropped

	// webRoomIdle: a room created on the web interface is removed when it
	// has stayed empty this long, counted from its creation or from when
	// its last player left.
	webRoomIdle = 10 * time.Minute
	expireEvery = 30 * time.Second
)

type client struct {
	conn net.Conn
	addr string
	name string
	room *room
	out  chan proto.Frame
	once sync.Once
	done chan struct{}
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
	size    int
	game    string    // the online version of the games in it (Hello.Game)
	members []*client // ring order = join order
	epoch   uint32

	race    settings.Race   // the room's settings: the environment's, for a room a game created
	eeprom  []proto.Setting // race.EEPROM()
	created time.Time

	// Rooms created on the web interface:
	web       bool      // stays while empty, until webRoomIdle
	public    bool      // listed with its code
	idleSince time.Time // when it was created or its last player left
	creator   string    // the creating address (maxIdleWebRoomsPerIP)
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

	race settings.Race    // the environment's race settings, for rooms a game creates
	now  func() time.Time // time.Now; a parameter for tests

	// web interface
	publicAddr  string // host:port for invite codes; empty: no invite code
	webPassword string // what creating a room on the web asks; empty: nothing
}

func newServer(race settings.Race, password string, maxRooms, maxConns, maxPerIP int) *server {
	return &server{rooms: map[string]*room{}, password: password, maxRooms: maxRooms, race: race, now: time.Now,
		perIP: map[string]int{}, maxConns: maxConns, maxPerIP: maxPerIP}
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
		r = &room{code: h.Room, size: h.Players, game: h.Game, race: s.race, eeprom: s.race.EEPROM(), created: s.now()}
		s.rooms[h.Room] = r
	case h.Op == "auto" && r.size != h.Players:
		return proto.Errf("size", "room %q is for %d players, not %d", h.Room, r.size, h.Players)
	}
	if r.game == "" {
		r.game = h.Game
	}
	if h.Game != "" && h.Game != r.game {
		return proto.Errf("game_version", "room %q is for game online version %s, not %s", h.Room, r.game, h.Game)
	}
	if len(r.members) >= r.size {
		return proto.Errf("full", "room %q is full (%d players)", h.Room, r.size)
	}
	r.members = append(r.members, c)
	c.room = r
	s.changed(r)
	log.Printf("room %s: %q (%s) joined (%d/%d)", r.code, c.name, c.addr, len(r.members), r.size)
	return nil
}

func (s *server) leave(c *client) {
	s.mu.Lock()
	defer s.mu.Unlock()
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
	log.Printf("room %s: %q (%s) left (%d/%d)", r.code, c.name, c.addr, len(r.members), r.size)
	if len(r.members) == 0 {
		if r.web {
			// it waits for players again, of any game version
			r.idleSince = s.now()
			r.game = ""
		} else {
			delete(s.rooms, r.code)
		}
		return
	}
	s.changed(r)
}

// expire removes the web rooms that have stayed empty for webRoomIdle.
func (s *server) expire() {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	for code, r := range s.rooms {
		if r.web && len(r.members) == 0 && now.Sub(r.idleSince) >= webRoomIdle {
			delete(s.rooms, code)
			log.Printf("room %s: removed, empty for %v", code, webRoomIdle)
		}
	}
}

// changed starts a new ring (epoch) and tells every member. Caller holds s.mu.
func (s *server) changed(r *room) {
	r.epoch++
	ready := len(r.members) == r.size
	names := make([]string, len(r.members))
	for i, m := range r.members {
		names[i] = m.name
	}
	for i, m := range r.members {
		st := proto.Status{Joined: len(r.members), Size: r.size, Position: i + 1, Ready: ready, EEPROM: r.eeprom, Names: names, AnyStart: r.race.AnyStart()}
		m.send(proto.Frame{Type: proto.TypeStatus, Epoch: r.epoch, Payload: st.Encode()})
	}
}

// broadcast passes a control frame from c to every other member of its room.
func (s *server) broadcast(c *client, f proto.Frame) {
	s.mu.Lock()
	r := c.room
	if r == nil || f.Epoch != r.epoch || len(r.members) != r.size {
		s.mu.Unlock()
		return
	}
	others := make([]*client, 0, len(r.members)-1)
	for _, m := range r.members {
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
	r := c.room
	if r == nil || f.Epoch != r.epoch || len(r.members) != r.size {
		s.mu.Unlock()
		return // an old ring's data, or the ring is not complete
	}
	var next *client
	for i, m := range r.members {
		if m == c {
			next = r.members[(i+1)%len(r.members)]
			break
		}
	}
	s.mu.Unlock()
	if next != nil {
		next.send(f)
	}
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
	c := &client{conn: conn, addr: conn.RemoteAddr().String(), out: make(chan proto.Frame, sendQueue), done: make(chan struct{})}
	defer c.close()

	br := bufio.NewReaderSize(conn, 32*1024)
	conn.SetReadDeadline(time.Now().Add(handshakeTimeout))
	line, err := proto.ReadLine(br, proto.MaxHello)
	if err != nil {
		return
	}
	h, err := proto.ParseHello(strings.TrimSpace(line))
	if err == nil && s.password != "" && subtle.ConstantTimeCompare([]byte(h.Password), []byte(s.password)) != 1 {
		err = proto.Errf("password", "wrong server password")
	}
	if err != nil {
		proto.WriteFrame(conn, proto.Frame{Type: proto.TypeError, Payload: proto.ErrorPayload(err)})
		log.Printf("%s: refused: %v", c.addr, err)
		return
	}
	c.name = h.Name

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
			case <-ping.C:
				if !write(proto.Frame{Type: proto.TypePing}) || bw.Flush() != nil {
					c.close()
					return
				}
			case <-c.done:
				return
			}
		}
	}()

	if err := s.join(c, h); err != nil {
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
		switch f.Type {
		case proto.TypeData:
			s.forward(c, f)
		case proto.TypeControl:
			if len(f.Payload) <= 256 {
				s.broadcast(c, f)
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
	port := flag.Int("port", envInt("DAYTONA_PORT", 7777), "TCP port to listen on (DAYTONA_PORT)")
	password := flag.String("password", os.Getenv("DAYTONA_PASSWORD"), "password the players must give, optional (DAYTONA_PASSWORD)")
	maxRooms := flag.Int("max-rooms", envInt("DAYTONA_MAX_ROOMS", 100), "rooms open at the same time, at most (DAYTONA_MAX_ROOMS)")
	maxConns := flag.Int("max-connections", envInt("DAYTONA_MAX_CONNECTIONS", 1000), "connections at the same time, at most (DAYTONA_MAX_CONNECTIONS)")
	maxPerIP := flag.Int("max-per-ip", envInt("DAYTONA_MAX_PER_IP", 16), "connections from one address, at most (DAYTONA_MAX_PER_IP)")
	webPort := flag.Int("web-port", envInt("DAYTONA_WEB_PORT", 8080), "web interface's TCP port, 0 to turn it off (DAYTONA_WEB_PORT)")
	publicAddr := flag.String("public-address", os.Getenv("DAYTONA_PUBLIC_ADDRESS"), "host:port the players reach this server at, for invite codes (DAYTONA_PUBLIC_ADDRESS)")
	webPassword := flag.String("web-password", os.Getenv("DAYTONA_WEB_PASSWORD"), "password to create rooms on the web interface; default: the server's password (DAYTONA_WEB_PASSWORD)")
	flag.Parse()

	race, err := settings.FromEnv(os.Getenv)
	if err != nil {
		log.Fatal(err)
	}
	s := newServer(race, *password, *maxRooms, *maxConns, *maxPerIP)
	if strings.TrimSpace(*publicAddr) != "" {
		addr, ok := invite.NormalizeServer(*publicAddr)
		if !ok {
			log.Fatalf("invalid DAYTONA_PUBLIC_ADDRESS=%q (use host or host:port)", *publicAddr)
		}
		s.publicAddr = addr
	}
	s.webPassword = *webPassword
	if s.webPassword == "" {
		s.webPassword = *password
	}

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("cannot listen on port %d: %v", *port, err)
	}
	log.Printf("daytona-relay listening on port %d (rooms of %d to %d players, password: %v)", *port, proto.MinPlayers, proto.MaxPlayers, *password != "")
	log.Printf("race settings: %s", race)
	if *webPort > 0 {
		wl, err := net.Listen("tcp", fmt.Sprintf(":%d", *webPort))
		if err != nil {
			log.Fatalf("cannot listen on web port %d: %v", *webPort, err)
		}
		hint := "invite codes for " + s.publicAddr
		if s.publicAddr == "" {
			hint = "no invite codes: set DAYTONA_PUBLIC_ADDRESS"
		}
		log.Printf("web interface on port %d (%s; creating a room asks a password: %v)", *webPort, hint, s.webPassword != "")
		go func() { log.Fatal(s.webServer().Serve(wl)) }()
	}
	go func() {
		for range time.Tick(expireEvery) {
			s.expire()
		}
	}()
	s.serve(ln)
}
