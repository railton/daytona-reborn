// The web interface: one page (web/index.html, embedded) and its JSON API,
// on its own port (DAYTONA_WEB_PORT).
//
//	GET  /             the page
//	GET  /api/config   the race options, the server's defaults, whether
//	                   creating asks a password, whether invite codes exist
//	GET  /api/rooms    the open rooms; a private room's code (and a game's
//	                   room's) is never shown; a public room's invite code
//	                   only when the server has no password
//	POST /api/rooms    create a room: {"players": 2-8, "public": bool
//	                   (default false), and any race setting, e.g.
//	                   "difficulty": "hard"}; missing settings take the
//	                   environment's. 201 with the room's code and the
//	                   invite code (needs DAYTONA_PUBLIC_ADDRESS)
//
// When creating asks a password (DAYTONA_WEB_PASSWORD, or else
// DAYTONA_PASSWORD, which travels in the invite code), POST /api/rooms needs
// HTTP basic authentication with it as the password (any user name). Errors
// are {"code": "...", "error": "English text"}.
package main

import (
	"crypto/rand"
	"crypto/subtle"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"mime"
	"net"
	"net/http"
	"sort"
	"time"

	"daytona-relay/invite"
	"daytona-relay/proto"
	"daytona-relay/settings"
)

//go:embed web/index.html
var indexHTML []byte

const (
	maxBody = 4 << 10 // a create request's JSON, at most
	// maxIdleWebRoomsPerIP: empty web rooms one address may have waiting,
	// so a visitor cannot fill the server's rooms on their own.
	maxIdleWebRoomsPerIP = 10
)

// badPasswordDelay slows password guessing down (a variable for tests).
var badPasswordDelay = 500 * time.Millisecond

func (s *server) webServer() *http.Server {
	return &http.Server{
		Handler:           s.webHandler(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    8 << 10,
	}
}

func (s *server) webHandler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-cache")
		w.Write(indexHTML)
	})
	mux.HandleFunc("GET /api/config", s.apiConfig)
	mux.HandleFunc("GET /api/rooms", s.apiRooms)
	mux.HandleFunc("POST /api/rooms", s.apiCreate)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src data:; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("X-Frame-Options", "DENY")
		mux.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetEscapeHTML(true)
	enc.Encode(v)
}

func writeError(w http.ResponseWriter, status int, code, format string, a ...any) {
	writeJSON(w, status, map[string]string{"code": code, "error": fmt.Sprintf(format, a...)})
}

type configJSON struct {
	Options          []settings.Option `json:"options"`
	Defaults         settings.Race     `json:"defaults"`
	MinPlayers       int               `json:"min_players"`
	MaxPlayers       int               `json:"max_players"`
	PasswordRequired bool              `json:"password_required"`
	Server           string            `json:"server"` // DAYTONA_PUBLIC_ADDRESS, normalized; empty: no invite codes
	IdleMinutes      int               `json:"idle_minutes"`
	MaxRooms         int               `json:"max_rooms"`
}

func (s *server) apiConfig(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, configJSON{
		Options: settings.Options(), Defaults: s.race,
		MinPlayers: proto.MinPlayers, MaxPlayers: proto.MaxPlayers,
		PasswordRequired: s.webPassword != "", Server: s.publicAddr,
		IdleMinutes: int(webRoomIdle / time.Minute), MaxRooms: s.maxRooms,
	})
}

type roomJSON struct {
	Code       string        `json:"code,omitempty"`   // public web rooms only
	Invite     string        `json:"invite,omitempty"` // public rooms, when the server has no password
	Public     bool          `json:"public"`
	Origin     string        `json:"origin"` // web, game
	Size       int           `json:"size"`
	Joined     int           `json:"joined"`
	Players    []string      `json:"players"` // ring order: the first is the MASTER
	State      string        `json:"state"`   // waiting, racing (the room is full and linked)
	Settings   settings.Race `json:"settings"`
	AgeSeconds int64         `json:"age_seconds"`
	// ExpiresIn: an empty web room, seconds before it is removed
	ExpiresIn *int64 `json:"expires_in,omitempty"`
}

type roomsJSON struct {
	Rooms    []roomJSON `json:"rooms"`
	Open     int        `json:"open"`
	MaxRooms int        `json:"max_rooms"`
}

func (s *server) apiRooms(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	now := s.now()
	out := roomsJSON{Rooms: []roomJSON{}, Open: len(s.rooms), MaxRooms: s.maxRooms}
	for _, rm := range s.rooms {
		j := roomJSON{Public: rm.public, Origin: "game", Size: rm.size, Joined: len(rm.members),
			Players: make([]string, len(rm.members)), State: "waiting", Settings: rm.race,
			AgeSeconds: int64(now.Sub(rm.created) / time.Second)}
		if rm.web {
			j.Origin = "web"
			if len(rm.members) == 0 {
				left := max(0, int64((webRoomIdle-now.Sub(rm.idleSince))/time.Second))
				j.ExpiresIn = &left
			}
		}
		if rm.public {
			j.Code = rm.code
			// an invite code holds the server's password: listed only when there is none
			if s.publicAddr != "" && s.password == "" {
				j.Invite = invite.Encode(invite.Invite{Server: s.publicAddr, Room: rm.code})
			}
		}
		for i, m := range rm.members {
			j.Players[i] = m.name
		}
		if len(rm.members) == rm.size {
			j.State = "racing"
		}
		out.Rooms = append(out.Rooms, j)
	}
	s.mu.Unlock()
	// public rooms first, then the newest first
	sort.SliceStable(out.Rooms, func(a, b int) bool {
		ra, rb := out.Rooms[a], out.Rooms[b]
		if ra.Public != rb.Public {
			return ra.Public
		}
		return ra.AgeSeconds < rb.AgeSeconds
	})
	writeJSON(w, http.StatusOK, out)
}

type createRequest struct {
	Players int   `json:"players"`
	Public  *bool `json:"public"`
	settings.Race
}

type createResponse struct {
	Room      string        `json:"room"`
	Players   int           `json:"players"`
	Public    bool          `json:"public"`
	Settings  settings.Race `json:"settings"`
	Server    string        `json:"server"` // empty: DAYTONA_PUBLIC_ADDRESS is not set
	Invite    string        `json:"invite"` // empty when Server is
	ExpiresIn int64         `json:"expires_in"`
}

func (s *server) authorized(r *http.Request) bool {
	if s.webPassword == "" {
		return true
	}
	_, pass, ok := r.BasicAuth()
	return ok && subtle.ConstantTimeCompare([]byte(pass), []byte(s.webPassword)) == 1
}

func (s *server) apiCreate(w http.ResponseWriter, r *http.Request) {
	if !s.authorized(r) {
		time.Sleep(badPasswordDelay)
		// no WWW-Authenticate: the page asks the password itself, not the browser
		writeError(w, http.StatusUnauthorized, "password", "wrong or missing password")
		return
	}
	if mt, _, _ := mime.ParseMediaType(r.Header.Get("Content-Type")); mt != "application/json" {
		writeError(w, http.StatusUnsupportedMediaType, "content_type", "send application/json")
		return
	}
	req := createRequest{Race: s.race}
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBody))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			writeError(w, http.StatusRequestEntityTooLarge, "too_large", "request larger than %d bytes", maxBody)
			return
		}
		writeError(w, http.StatusBadRequest, "json", "invalid JSON: %v", err)
		return
	}
	if _, err := dec.Token(); !errors.Is(err, io.EOF) {
		writeError(w, http.StatusBadRequest, "json", "invalid JSON: one object only")
		return
	}
	if req.Players < proto.MinPlayers || req.Players > proto.MaxPlayers {
		writeError(w, http.StatusBadRequest, "players", "players must be %d to %d", proto.MinPlayers, proto.MaxPlayers)
		return
	}
	if err := req.Race.Validate(); err != nil {
		writeError(w, http.StatusBadRequest, "settings", "%v", err)
		return
	}
	ip, _, _ := net.SplitHostPort(r.RemoteAddr)
	public := req.Public != nil && *req.Public
	rm, err := s.createWebRoom(req.Players, req.Race, public, ip)
	if err != nil {
		var e *proto.Error
		if errors.As(err, &e) && e.Code == "too_many" {
			writeError(w, http.StatusTooManyRequests, e.Code, "%s", e.Text)
		} else if errors.As(err, &e) {
			writeError(w, http.StatusServiceUnavailable, e.Code, "%s", e.Text)
		} else {
			writeError(w, http.StatusInternalServerError, "other", "%v", err)
		}
		return
	}
	resp := createResponse{Room: rm, Players: req.Players, Public: public, Settings: req.Race,
		Server: s.publicAddr, ExpiresIn: int64(webRoomIdle / time.Second)}
	if s.publicAddr != "" {
		resp.Invite = invite.Encode(invite.Invite{Server: s.publicAddr, Room: rm, Password: s.password})
	}
	log.Printf("room %s: created on the web by %s (%d players, public: %v, %s)", rm, ip, req.Players, public, req.Race)
	writeJSON(w, http.StatusCreated, resp)
}

// roomChars: as the game's random_room (no i, l, o, 0, 1: easy to read aloud)
const roomChars = "abcdefghjkmnpqrstuvwxyz23456789"

func randomRoom() (string, error) {
	b := make([]byte, 8)
	for i := range b {
		n, err := rand.Int(rand.Reader, big.NewInt(int64(len(roomChars))))
		if err != nil {
			return "", err
		}
		b[i] = roomChars[n.Int64()]
	}
	return string(b), nil
}

// createWebRoom opens an empty room with its own settings and returns its code.
func (s *server) createWebRoom(players int, race settings.Race, public bool, creator string) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.rooms) >= s.maxRooms {
		return "", proto.Errf("server_full", "server full: %d rooms at most", s.maxRooms)
	}
	idle := 0
	for _, r := range s.rooms {
		if r.web && r.creator == creator && len(r.members) == 0 {
			idle++
		}
	}
	if idle >= maxIdleWebRoomsPerIP {
		return "", proto.Errf("too_many", "%d empty rooms from your address already wait for players", idle)
	}
	for try := 0; try < 10; try++ {
		code, err := randomRoom()
		if err != nil {
			return "", err
		}
		if s.rooms[code] != nil {
			continue
		}
		now := s.now()
		s.rooms[code] = &room{code: code, size: players, race: race, eeprom: race.EEPROM(), created: now,
			web: true, public: public, idleSince: now, creator: creator}
		return code, nil
	}
	return "", errors.New("no free room code")
}
