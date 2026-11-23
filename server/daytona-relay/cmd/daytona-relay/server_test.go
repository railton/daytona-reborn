package main

import (
	"bufio"
	"errors"
	"net"
	"reflect"
	"testing"
	"time"

	"daytona-relay/proto"
	"daytona-relay/settings"
)

func envRace(t *testing.T, env map[string]string) settings.Race {
	t.Helper()
	r, err := settings.FromEnv(func(k string) string { return env[k] })
	if err != nil {
		t.Fatal(err)
	}
	return r
}

// fakeClient: a member without a connection, its frames queued in out.
func fakeClient(name string) *client {
	return &client{name: name, addr: "test", out: make(chan proto.Frame, 64), done: make(chan struct{})}
}

type clock struct{ t time.Time }

func (c *clock) now() time.Time          { return c.t }
func (c *clock) add(d time.Duration)     { c.t = c.t.Add(d) }
func newTestServer(t *testing.T) *server { return newServer(envRace(t, nil), "", 100, 1000, 16) }

// dial joins a room over TCP as a game does and returns its first status.
type player struct {
	net.Conn
	br *bufio.Reader
}

func dial(t *testing.T, addr string, h proto.Hello) (*player, proto.Status) {
	t.Helper()
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	if _, err := conn.Write([]byte(h.Line())); err != nil {
		t.Fatal(err)
	}
	p := &player{conn, bufio.NewReader(conn)}
	return p, nextStatus(t, p)
}

func nextStatus(t *testing.T, p *player) proto.Status {
	t.Helper()
	p.SetReadDeadline(time.Now().Add(5 * time.Second))
	for {
		f, err := proto.ReadFrame(p.br)
		if err != nil {
			t.Fatal(err)
		}
		switch f.Type {
		case proto.TypeStatus:
			st, err := proto.DecodeStatus(f.Payload)
			if err != nil {
				t.Fatal(err)
			}
			return st
		case proto.TypeError:
			t.Fatalf("refused: %s", f.Payload)
		}
	}
}

// A web room's own settings reach its players; a game's room on the same
// server keeps the environment's.
func TestRoomSettingsInStatus(t *testing.T) {
	env := envRace(t, map[string]string{"DAYTONA_DIFFICULTY": "easy"})
	s := newServer(env, "", 100, 1000, 16)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	go s.serve(ln)

	web := env
	web.Difficulty, web.GameMode, web.FreePlay, web.Start, web.Country = "hardest", "endurance", "off", "anyone", "jpn"
	code, err := s.createWebRoom(web, true, "127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	a, st := dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: code, Name: "A", Game: "3"})
	if st.Size != 0 || st.Joined != 1 || st.Position != 0 || st.Ready || !st.Owner || st.Racing {
		t.Errorf("first status %+v", st)
	}
	if !reflect.DeepEqual(st.EEPROM, web.EEPROM()) || !st.AnyStart {
		t.Errorf("web room: eeprom %v anyStart %v, want %v true", st.EEPROM, st.AnyStart, web.EEPROM())
	}
	b, st := dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: code, Name: "B", Game: "3"})
	if st.Ready || st.Owner || !reflect.DeepEqual(st.Names, []string{"A", "B"}) || !reflect.DeepEqual(st.EEPROM, web.EEPROM()) {
		t.Errorf("second status %+v", st)
	}
	if st := nextStatus(t, a); st.Ready || st.Joined != 2 || !st.AnyStart {
		t.Errorf("first player's update %+v", st)
	}
	// only the owner starts: B's Begin is ignored, A's forms the ring
	proto.WriteFrame(b, proto.Frame{Type: proto.TypeBegin})
	proto.WriteFrame(a, proto.Frame{Type: proto.TypeBegin})
	if st := nextStatus(t, a); !st.Ready || st.Position != 1 || st.Size != 2 || !st.Racing {
		t.Errorf("owner after Begin %+v", st)
	}
	if st := nextStatus(t, b); !st.Ready || st.Position != 2 || !reflect.DeepEqual(st.EEPROM, web.EEPROM()) {
		t.Errorf("second after Begin %+v", st)
	}

	_, st = dial(t, ln.Addr().String(), proto.Hello{Op: "create", Room: "game1", Players: 2, Name: "C", Game: "3"})
	if !reflect.DeepEqual(st.EEPROM, env.EEPROM()) || st.AnyStart {
		t.Errorf("game room: eeprom %v anyStart %v, want %v false", st.EEPROM, st.AnyStart, env.EEPROM())
	}
}

func TestWebRoomExpiry(t *testing.T) {
	s := newTestServer(t)
	c := &clock{time.Date(2026, 1, 1, 12, 0, 0, 0, time.UTC)}
	s.now = c.now
	code, err := s.createWebRoom(s.race, false, "1.2.3.4")
	if err != nil {
		t.Fatal(err)
	}
	c.add(webRoomIdle - time.Second)
	s.expire()
	if s.rooms[code] == nil {
		t.Fatal("removed before webRoomIdle")
	}
	c.add(time.Second)
	s.expire()
	if s.rooms[code] != nil {
		t.Fatal("still there after webRoomIdle empty")
	}

	// a player joins and leaves: counted again from when it left
	code, _ = s.createWebRoom(s.race, false, "1.2.3.4")
	c.add(5 * time.Minute)
	p := fakeClient("A")
	if err := s.join(p, proto.Hello{Op: "join", Room: code, Game: "3"}); err != nil {
		t.Fatal(err)
	}
	c.add(time.Hour) // occupied: never expires
	s.expire()
	if s.rooms[code] == nil {
		t.Fatal("an occupied room expired")
	}
	s.leave(p)
	if s.rooms[code] == nil {
		t.Fatal("a web room was removed when its last player left")
	}
	c.add(webRoomIdle - time.Second)
	s.expire()
	if s.rooms[code] == nil {
		t.Fatal("removed before webRoomIdle after the last player left")
	}
	// emptied, it takes a game of another version
	q := fakeClient("B")
	if err := s.join(q, proto.Hello{Op: "join", Room: code, Game: "4"}); err != nil {
		t.Fatalf("other game version after the room emptied: %v", err)
	}
	s.leave(q)
	c.add(webRoomIdle)
	s.expire()
	if s.rooms[code] != nil {
		t.Fatal("still there after webRoomIdle")
	}

	// a game's room: gone with its last player, as before
	g := fakeClient("G")
	if err := s.join(g, proto.Hello{Op: "create", Room: "g", Players: 2}); err != nil {
		t.Fatal(err)
	}
	s.leave(g)
	if s.rooms["g"] != nil {
		t.Fatal("a game's empty room stayed")
	}
}

func TestWebRoomJoinRules(t *testing.T) {
	s := newTestServer(t)
	code, _ := s.createWebRoom(s.race, false, "x")
	var e *proto.Error
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: code, Players: 2}); !errors.As(err, &e) || e.Code != "exists" {
		t.Errorf("create over a web room: %v", err)
	}
	if err := s.join(fakeClient("A"), proto.Hello{Op: "auto", Room: code, Players: 3}); !errors.As(err, &e) || e.Code != "size" {
		t.Errorf("auto with another size: %v", err)
	}
	s.join(fakeClient("A"), proto.Hello{Op: "join", Room: code, Game: "3"})
	if err := s.join(fakeClient("B"), proto.Hello{Op: "join", Room: code, Game: "2"}); !errors.As(err, &e) || e.Code != "game_version" {
		t.Errorf("another game version: %v", err)
	}
	for i := 1; i < proto.MaxPlayers; i++ {
		if err := s.join(fakeClient("B"), proto.Hello{Op: "join", Room: code, Game: "3"}); err != nil {
			t.Fatalf("player %d: %v", i+1, err)
		}
	}
	if err := s.join(fakeClient("I"), proto.Hello{Op: "join", Room: code, Game: "3"}); !errors.As(err, &e) || e.Code != "full" {
		t.Errorf("ninth in a room: %v", err)
	}
}

// lastStatus: the newest status queued for a fake client.
func lastStatus(t *testing.T, c *client) (proto.Status, uint32) {
	t.Helper()
	var st proto.Status
	var epoch uint32
	found := false
	for {
		select {
		case f := <-c.out:
			if f.Type == proto.TypeStatus {
				st, _ = proto.DecodeStatus(f.Payload)
				epoch, found = f.Epoch, true
			}
		default:
			if !found {
				t.Fatalf("%s: no status", c.name)
			}
			return st, epoch
		}
	}
}

// The owner starts races; who joins during one waits for the next; a new
// Begin takes everyone; a racer leaving breaks the ring; the owner's role
// passes on.
func TestBeginQueueAndOwner(t *testing.T) {
	s := newTestServer(t)
	a, b, c := fakeClient("A"), fakeClient("B"), fakeClient("C")
	if err := s.join(a, proto.Hello{Op: "create", Room: "r"}); err != nil {
		t.Fatal(err)
	}
	s.begin(a) // alone: nothing
	if st, _ := lastStatus(t, a); st.Racing || !st.Owner {
		t.Fatalf("alone %+v", st)
	}
	s.join(b, proto.Hello{Op: "join", Room: "r"})
	s.begin(b) // not the owner: nothing
	if st, _ := lastStatus(t, b); st.Racing {
		t.Fatal("a non-owner started a race")
	}
	s.begin(a)
	sa, ea := lastStatus(t, a)
	sb, eb := lastStatus(t, b)
	if !sa.Ready || sa.Position != 1 || !sb.Ready || sb.Position != 2 || ea != eb {
		t.Fatalf("race of 2: %+v %+v", sa, sb)
	}
	// C joins during the race: waits; the racers' ring (epoch) is unchanged
	s.join(c, proto.Hello{Op: "join", Room: "r"})
	sc, _ := lastStatus(t, c)
	sa, ea2 := lastStatus(t, a)
	if sc.Ready || sc.Position != 0 || !sc.Racing || ea2 != ea || !sa.Ready ||
		!reflect.DeepEqual(sa.Waiting, []string{"C"}) || !reflect.DeepEqual(sa.Names, []string{"A", "B"}) {
		t.Fatalf("joining during a race: C %+v, A %+v (epoch %d -> %d)", sc, sa, ea, ea2)
	}
	// a new Begin takes everyone, in a new ring
	s.begin(a)
	sc, ec := lastStatus(t, c)
	if !sc.Ready || sc.Position != 3 || sc.Size != 3 || ec == ea {
		t.Fatalf("second race: C %+v epoch %d", sc, ec)
	}
	lastStatus(t, a)
	lastStatus(t, b)
	// B leaves mid-race: the ring breaks, back to the room
	s.leave(b)
	sa, ea3 := lastStatus(t, a)
	if sa.Ready || sa.Racing || ea3 == ec || !reflect.DeepEqual(sa.Names, []string{"A", "C"}) {
		t.Fatalf("after a racer left: %+v", sa)
	}
	// the owner leaves: C owns the room now
	s.leave(a)
	if sc, _ := lastStatus(t, c); !sc.Owner || sc.Joined != 1 {
		t.Fatalf("owner left: %+v", sc)
	}
}

// The daytona-link helper's rooms keep a fixed size and start on their own.
func TestAutoRoomStartsAtSize(t *testing.T) {
	s := newTestServer(t)
	a, b := fakeClient("A"), fakeClient("B")
	s.join(a, proto.Hello{Op: "auto", Room: "h", Players: 2})
	if st, _ := lastStatus(t, a); st.Racing || st.Owner {
		t.Fatalf("helper room, alone: %+v", st)
	}
	s.join(b, proto.Hello{Op: "auto", Room: "h", Players: 2})
	if st, _ := lastStatus(t, b); !st.Ready || st.Position != 2 {
		t.Fatalf("helper room, full: %+v", st)
	}
	var e *proto.Error
	if err := s.join(fakeClient("C"), proto.Hello{Op: "auto", Room: "h", Players: 2}); !errors.As(err, &e) || e.Code != "full" {
		t.Errorf("third in a helper room of 2: %v", err)
	}
}

// DAYTONA_MAX_ROOMS counts the games' rooms and the web's.
func TestMaxRoomsCountsBoth(t *testing.T) {
	s := newServer(envRace(t, nil), "", 2, 1000, 16)
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: "game", Players: 2}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.createWebRoom(s.race, true, "x"); err != nil {
		t.Fatal(err)
	}
	var e *proto.Error
	if _, err := s.createWebRoom(s.race, true, "x"); !errors.As(err, &e) || e.Code != "server_full" {
		t.Errorf("third room on the web: %v", err)
	}
	if err := s.join(fakeClient("B"), proto.Hello{Op: "create", Room: "game2", Players: 2}); !errors.As(err, &e) || e.Code != "server_full" {
		t.Errorf("third room from a game: %v", err)
	}
}

func TestRandomRoom(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 200; i++ {
		r, err := randomRoom()
		if err != nil {
			t.Fatal(err)
		}
		if n, err := proto.NormalizeRoom(r); err != nil || n != r || len(r) != 8 {
			t.Fatalf("room %q", r)
		}
		seen[r] = true
	}
	if len(seen) < 199 {
		t.Errorf("%d distinct codes of 200", len(seen))
	}
}
