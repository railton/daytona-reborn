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
	code, err := s.createWebRoom(2, web, true, "127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	a, st := dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: code, Name: "A", Game: "3"})
	if st.Size != 2 || st.Joined != 1 || st.Position != 1 || st.Ready {
		t.Errorf("first status %+v", st)
	}
	if !reflect.DeepEqual(st.EEPROM, web.EEPROM()) || !st.AnyStart {
		t.Errorf("web room: eeprom %v anyStart %v, want %v true", st.EEPROM, st.AnyStart, web.EEPROM())
	}
	_, st = dial(t, ln.Addr().String(), proto.Hello{Op: "join", Room: code, Name: "B", Game: "3"})
	if st.Position != 2 || !st.Ready || !reflect.DeepEqual(st.Names, []string{"A", "B"}) || !reflect.DeepEqual(st.EEPROM, web.EEPROM()) {
		t.Errorf("second status %+v", st)
	}
	if st := nextStatus(t, a); !st.Ready || !st.AnyStart {
		t.Errorf("first player's update %+v", st)
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
	code, err := s.createWebRoom(2, s.race, false, "1.2.3.4")
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
	code, _ = s.createWebRoom(2, s.race, false, "1.2.3.4")
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
	code, _ := s.createWebRoom(2, s.race, false, "x")
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
	s.join(fakeClient("B"), proto.Hello{Op: "join", Room: code, Game: "3"})
	if err := s.join(fakeClient("C"), proto.Hello{Op: "join", Room: code, Game: "3"}); !errors.As(err, &e) || e.Code != "full" {
		t.Errorf("third in a room of 2: %v", err)
	}
}

// DAYTONA_MAX_ROOMS counts the games' rooms and the web's.
func TestMaxRoomsCountsBoth(t *testing.T) {
	s := newServer(envRace(t, nil), "", 2, 1000, 16)
	if err := s.join(fakeClient("A"), proto.Hello{Op: "create", Room: "game", Players: 2}); err != nil {
		t.Fatal(err)
	}
	if _, err := s.createWebRoom(2, s.race, true, "x"); err != nil {
		t.Fatal(err)
	}
	var e *proto.Error
	if _, err := s.createWebRoom(2, s.race, true, "x"); !errors.As(err, &e) || e.Code != "server_full" {
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
