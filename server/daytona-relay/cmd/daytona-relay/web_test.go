package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"daytona-relay/invite"
	"daytona-relay/proto"
)

func init() { badPasswordDelay = 0 }

type call struct {
	method, path, body, contentType string
	password                        *string
}

func (s *server) do(t *testing.T, c call) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(c.method, c.path, strings.NewReader(c.body))
	req.RemoteAddr = "192.0.2.7:51000"
	if c.contentType != "" {
		req.Header.Set("Content-Type", c.contentType)
	} else if c.body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	if c.password != nil {
		req.SetBasicAuth("web", *c.password)
	}
	w := httptest.NewRecorder()
	s.webHandler().ServeHTTP(w, req)
	return w
}

func decode[T any](t *testing.T, w *httptest.ResponseRecorder) T {
	t.Helper()
	var v T
	if err := json.Unmarshal(w.Body.Bytes(), &v); err != nil {
		t.Fatalf("%s: %v", w.Body, err)
	}
	return v
}

func TestWebPage(t *testing.T) {
	s := newTestServer(t)
	w := s.do(t, call{method: "GET", path: "/"})
	if w.Code != 200 || !strings.HasPrefix(w.Header().Get("Content-Type"), "text/html") || !strings.Contains(w.Body.String(), "DAYTONA") {
		t.Fatalf("GET /: %d %s", w.Code, w.Header())
	}
	if !strings.Contains(w.Header().Get("Content-Security-Policy"), "default-src 'none'") || w.Header().Get("X-Content-Type-Options") != "nosniff" {
		t.Errorf("security headers: %v", w.Header())
	}
	if w := s.do(t, call{method: "GET", path: "/nothing"}); w.Code != 404 {
		t.Errorf("GET /nothing: %d", w.Code)
	}
	if w := s.do(t, call{method: "PUT", path: "/api/rooms", body: "{}"}); w.Code != 405 {
		t.Errorf("PUT /api/rooms: %d", w.Code)
	}
	if w := s.do(t, call{method: "DELETE", path: "/"}); w.Code != 405 {
		t.Errorf("DELETE /: %d", w.Code)
	}
}

func TestWebConfig(t *testing.T) {
	s := newTestServer(t)
	s.race.Difficulty = "hard"
	s.publicAddr = "relay.example:7777"
	c := decode[configJSON](t, s.do(t, call{method: "GET", path: "/api/config"}))
	if len(c.Options) != 8 || c.Defaults.Difficulty != "hard" || c.MinPlayers != 2 || c.MaxPlayers != 8 ||
		c.PasswordRequired || c.Server != "relay.example:7777" || c.IdleMinutes != 10 {
		t.Errorf("config %+v", c)
	}
	var raw map[string]any
	json.Unmarshal(s.do(t, call{method: "GET", path: "/api/config"}).Body.Bytes(), &raw)
	o := raw["options"].([]any)[0].(map[string]any)
	if o["key"] != "cabinet" || len(o["values"].([]any)) != 3 {
		t.Errorf("first option %v", o)
	}
}

func TestWebCreate(t *testing.T) {
	s := newTestServer(t)
	s.publicAddr = "relay.example:17777"
	s.race.Cabinet = "deluxe"
	w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{"public":true,"difficulty":"hardest","start":"anyone"}`})
	if w.Code != http.StatusCreated {
		t.Fatalf("create: %d %s", w.Code, w.Body)
	}
	r := decode[createResponse](t, w)
	rm := s.rooms[r.Room]
	if rm == nil || !rm.web || !rm.public || rm.capacity() != proto.MaxPlayers || rm.creator != "192.0.2.7" {
		t.Fatalf("room %+v", rm)
	}
	// the given settings, the environment's for the others
	if rm.race.Difficulty != "hardest" || rm.race.Start != "anyone" || rm.race.Cabinet != "deluxe" || r.Settings != rm.race {
		t.Errorf("settings %+v", rm.race)
	}
	in, err := invite.Decode(r.Invite)
	if err != nil || in != (invite.Invite{Server: "relay.example:17777", Room: r.Room}) {
		t.Errorf("invite %q: %+v %v", r.Invite, in, err)
	}
	if r.Server != s.publicAddr || r.ExpiresIn != 600 || !r.Public {
		t.Errorf("response %+v", r)
	}

	// public is false unless asked
	r = decode[createResponse](t, s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`}))
	if s.rooms[r.Room].public || r.Public {
		t.Error("a room without \"public\" is public")
	}

	// no public address: no invite code, the room still made
	s.publicAddr = ""
	w = s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`})
	r = decode[createResponse](t, w)
	if w.Code != 201 || r.Invite != "" || r.Server != "" || s.rooms[r.Room] == nil {
		t.Errorf("without DAYTONA_PUBLIC_ADDRESS: %d %+v", w.Code, r)
	}
}

func TestWebCreateRefuses(t *testing.T) {
	s := newTestServer(t)
	big := `{"cabinet":"` + strings.Repeat("x", maxBody) + `"}`
	for _, c := range []struct {
		body, contentType string
		status            int
		code              string
	}{
		{`{"players":4}`, "", 400, "json"}, // no longer a field: rooms take up to 8
		{`{"public":"yes"}`, "", 400, "json"},
		{`{"difficulty":"insane"}`, "", 400, "settings"},
		{`{"difficulty":"Hard"}`, "", 400, "settings"},
		{`{"cabinet":""}`, "", 400, "settings"},
		{`{"start":"nobody"}`, "", 400, "settings"},
		{`{"free_play":"maybe"}`, "", 400, "settings"},
		{`{"room":"mine"}`, "", 400, "json"}, // unknown field
		{`{} {}`, "", 400, "json"},
		{`{"public":true`, "", 400, "json"},
		{`[2]`, "", 400, "json"},
		{`public=1`, "application/x-www-form-urlencoded", 415, "content_type"},
		{`{}`, "text/plain", 415, "content_type"},
		{big, "", 413, "too_large"},
	} {
		w := s.do(t, call{method: "POST", path: "/api/rooms", body: c.body, contentType: c.contentType})
		e := decode[map[string]string](t, w)
		if w.Code != c.status || e["code"] != c.code || e["error"] == "" {
			t.Errorf("%.60s: %d %v, want %d %s", c.body, w.Code, e, c.status, c.code)
		}
	}
	if len(s.rooms) != 0 {
		t.Errorf("%d rooms made by refused requests", len(s.rooms))
	}
}

func TestWebPassword(t *testing.T) {
	s := newTestServer(t)
	s.password = "server-pw"
	s.webPassword = "server-pw"
	s.publicAddr = "relay.example:7777"
	if c := decode[configJSON](t, s.do(t, call{method: "GET", path: "/api/config"})); !c.PasswordRequired {
		t.Error("password_required false")
	}
	wrong := "nope"
	for _, pw := range []*string{nil, &wrong} {
		w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`, password: pw})
		if w.Code != 401 || w.Header().Get("WWW-Authenticate") != "" {
			t.Errorf("password %v: %d %v", pw, w.Code, w.Header())
		}
	}
	right := "server-pw"
	w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{"public":true}`, password: &right})
	if w.Code != 201 {
		t.Fatalf("right password: %d %s", w.Code, w.Body)
	}
	r := decode[createResponse](t, w)
	if in, err := invite.Decode(r.Invite); err != nil || in.Password != "server-pw" {
		t.Errorf("the invite does not carry the server's password: %+v %v", in, err)
	}
	// the list is open, and never shows an invite (it holds the password)
	l := decode[roomsJSON](t, s.do(t, call{method: "GET", path: "/api/rooms"}))
	if len(l.Rooms) != 1 || l.Rooms[0].Code != r.Room || l.Rooms[0].Invite != "" {
		t.Errorf("list %+v", l)
	}

	// a separate web password
	s.webPassword = "admin"
	if w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`, password: &right}); w.Code != 401 {
		t.Errorf("the server's password with DAYTONA_WEB_PASSWORD set: %d", w.Code)
	}
	admin := "admin"
	if w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`, password: &admin}); w.Code != 201 {
		t.Errorf("web password: %d", w.Code)
	}
}

func TestWebRoomsList(t *testing.T) {
	s := newTestServer(t)
	s.publicAddr = "relay.example:7777"
	c := &clock{s.now()}
	s.now = c.now
	pub, _ := s.createWebRoom(s.race, true, "x")
	c.add(60e9)
	priv, _ := s.createWebRoom(s.race, false, "x")
	s.join(fakeClient("Ana"), proto.Hello{Op: "join", Room: pub})
	s.join(fakeClient("Bia"), proto.Hello{Op: "join", Room: priv})
	s.join(fakeClient("Caio"), proto.Hello{Op: "join", Room: priv})
	s.join(fakeClient("Duda"), proto.Hello{Op: "create", Room: "fromgame"})
	s.begin(s.rooms[priv].owner) // Bia starts a race with Caio
	c.add(30e9)

	w := s.do(t, call{method: "GET", path: "/api/rooms"})
	if w.Code != 200 || w.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("%d %v", w.Code, w.Header())
	}
	if strings.Contains(w.Body.String(), priv) || strings.Contains(w.Body.String(), "fromgame") {
		t.Fatalf("a private room's code is listed: %s", w.Body)
	}
	l := decode[roomsJSON](t, w)
	if l.Open != 3 || l.MaxRooms != 100 || len(l.Rooms) != 3 {
		t.Fatalf("list %+v", l)
	}
	p := l.Rooms[0] // public first
	in, _ := invite.Decode(p.Invite)
	if p.Code != pub || !p.Public || p.Origin != "web" || p.Max != proto.MaxPlayers || p.Joined != 1 || p.Racing != 0 || p.State != "waiting" ||
		len(p.Players) != 1 || p.Players[0] != "Ana" || p.AgeSeconds != 90 || p.ExpiresIn != nil || in.Room != pub {
		t.Errorf("public room %+v", p)
	}
	byOrigin := map[string]roomJSON{}
	for _, r := range l.Rooms[1:] {
		byOrigin[r.Origin] = r
	}
	if r := byOrigin["web"]; r.Code != "" || r.Invite != "" || r.State != "racing" || r.Joined != 2 || r.Racing != 2 || r.Players[1] != "Caio" {
		t.Errorf("private web room %+v", r)
	}
	if r := byOrigin["game"]; r.Code != "" || r.Public || r.State != "waiting" || r.Settings != s.race {
		t.Errorf("game room %+v", r)
	}

	// an empty web room shows how long it has left
	empty, _ := s.createWebRoom(s.race, true, "x")
	c.add(4 * 60e9)
	l = decode[roomsJSON](t, s.do(t, call{method: "GET", path: "/api/rooms"}))
	for _, r := range l.Rooms {
		if r.Code == empty && (r.ExpiresIn == nil || *r.ExpiresIn != 360) {
			t.Errorf("empty room %+v", r)
		}
	}
}

func TestWebIdleRoomsPerAddress(t *testing.T) {
	s := newTestServer(t)
	for i := 0; i < maxIdleWebRoomsPerIP; i++ {
		if w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`}); w.Code != 201 {
			t.Fatalf("room %d: %d", i, w.Code)
		}
	}
	w := s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`})
	if w.Code != 429 || decode[map[string]string](t, w)["code"] != "too_many" {
		t.Errorf("one room too many: %d %s", w.Code, w.Body)
	}
	if _, err := s.createWebRoom(s.race, true, "198.51.100.1"); err != nil {
		t.Errorf("another address: %v", err)
	}
	s.rooms = map[string]*room{}
	s.maxRooms = 1
	s.createWebRoom(s.race, true, "other")
	w = s.do(t, call{method: "POST", path: "/api/rooms", body: `{}`})
	if w.Code != 503 || decode[map[string]string](t, w)["code"] != "server_full" {
		t.Errorf("server full: %d %s", w.Code, w.Body)
	}
}
