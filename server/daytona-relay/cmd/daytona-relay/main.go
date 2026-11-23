// daytona-relay: the server. Players connect here (the game's Online mode,
// or the daytona-link helper) and join a room by its code; once the room is
// full, the server forms the ring (in join order: the first is the MASTER)
// and passes each player's game bytes to the next player.
//
// Configuration: flags, or the environment (Docker):
//
//	DAYTONA_PORT (7777), DAYTONA_PASSWORD (none), DAYTONA_MAX_ROOMS (100),
//	and the race settings (see package settings).
package main

import (
	"bufio"
	"crypto/subtle"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"daytona-relay/proto"
	"daytona-relay/settings"
)

const (
	handshakeTimeout = 10 * time.Second
	idleTimeout      = 30 * time.Second // no frame (not even a ping) for this long: gone
	pingEvery        = 10 * time.Second
	sendQueue        = 512 // frames waiting for a slow client before it is dropped
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
	eeprom             []proto.Setting
	anyStart           bool
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
		r = &room{code: h.Room, size: h.Players, game: h.Game}
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
		delete(s.rooms, r.code)
		return
	}
	s.changed(r)
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
		st := proto.Status{Joined: len(r.members), Size: r.size, Position: i + 1, Ready: ready, EEPROM: s.eeprom, Names: names, AnyStart: s.anyStart}
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
	flag.Parse()

	eeprom, desc, err := settings.FromEnv(os.Getenv)
	if err != nil {
		log.Fatal(err)
	}
	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		log.Fatalf("cannot listen on port %d: %v", *port, err)
	}
	start := strings.ToLower(strings.TrimSpace(os.Getenv("DAYTONA_START")))
	if start != "" && start != "master" && start != "anyone" {
		log.Fatalf("invalid DAYTONA_START=%q (use: master, anyone)", start)
	}
	if start == "" {
		start = "master"
	}
	s := &server{rooms: map[string]*room{}, password: *password, maxRooms: *maxRooms, eeprom: eeprom, anyStart: start == "anyone",
		perIP: map[string]int{}, maxConns: *maxConns, maxPerIP: *maxPerIP}
	log.Printf("daytona-relay listening on port %d (rooms of %d to %d players, password: %v)", *port, proto.MinPlayers, proto.MaxPlayers, *password != "")
	log.Printf("race settings: %s start=%s", desc, start)
	for {
		conn, err := ln.Accept()
		if err != nil {
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
