// Package settings: the race settings every game in a room uses. The
// server's environment (the Docker container's variables) gives the
// defaults; the game that creates a room may choose its own (Apply), and
// the daytona-link helper's rooms take the defaults. They are bytes of the I/O board's settings
// EEPROM, the test menu's GAME SYSTEM page, mapped on Daytona USA Revision A:
//
//	DAYTONA_CABINET          twin | uplight | deluxe        (0x1a: 01 02 00)
//	DAYTONA_COUNTRY          usa | jpn | export             (0x1b: 00 01 02; the game's texts: usa in English)
//	DAYTONA_DIFFICULTY       easy | normal | hard | hardest (0x20: 00 01 02 03)
//	DAYTONA_GAME_MODE        normal | grandprix | endurance (0x1c: 00 01 02)
//	DAYTONA_RIVAL_ARROW      on | off                       (0x1d: 00 01)
//	DAYTONA_ADVERTISE_SOUND  on | off                       (0x19: 00 01)
//	DAYTONA_FREE_PLAY        on | off: no coins, Start only (0x10 = 26, 0x14 = 1)
//
// and one rule that is not in the EEPROM, sent in the status as start=:
//
//	DAYTONA_START            master | anyone: who may start a race
//
// LINK ID and CAR NUMBER are not here: each game sets its own from its place
// in the room (the first is the MASTER).
package settings

import (
	"fmt"
	"strings"

	"daytona-relay/proto"
)

// Race is one room's settings, as names (the environment's values, and the
// keys and values the game sends with op=create).
type Race struct {
	Cabinet        string
	Country        string
	Difficulty     string
	GameMode       string
	RivalArrow     string
	AdvertiseSound string
	FreePlay       string
	Start          string
}

// Value is one choice of an option: its name and a description.
type Value struct {
	Name  string
	Label string
}

// Option describes one setting.
type Option struct {
	Key    string // as the game sends it (set=key:value)
	Env    string
	Label  string
	Values []Value

	def    string
	offset byte            // EEPROM offset; 0: not a plain EEPROM byte (free play, start)
	bytes  map[string]byte // value name -> EEPROM byte
	field  func(*Race) *string
}

var options = []Option{
	{Key: "cabinet", Env: "DAYTONA_CABINET", Label: "Cabinet", def: "twin", offset: 0x1a,
		Values: []Value{{"twin", "Twin"}, {"uplight", "Uplight"}, {"deluxe", "Deluxe"}},
		bytes:  map[string]byte{"twin": 1, "uplight": 2, "deluxe": 0},
		field:  func(r *Race) *string { return &r.Cabinet }},
	{Key: "country", Env: "DAYTONA_COUNTRY", Label: "Country", def: "usa", offset: 0x1b,
		Values: []Value{{"usa", "USA (English)"}, {"jpn", "Japan (Japanese)"}, {"export", "Export"}},
		bytes:  map[string]byte{"usa": 0, "jpn": 1, "export": 2},
		field:  func(r *Race) *string { return &r.Country }},
	{Key: "difficulty", Env: "DAYTONA_DIFFICULTY", Label: "Difficulty", def: "normal", offset: 0x20,
		Values: []Value{{"easy", "Easy"}, {"normal", "Normal"}, {"hard", "Hard"}, {"hardest", "Hardest"}},
		bytes:  map[string]byte{"easy": 0, "normal": 1, "hard": 2, "hardest": 3},
		field:  func(r *Race) *string { return &r.Difficulty }},
	{Key: "game_mode", Env: "DAYTONA_GAME_MODE", Label: "Game mode", def: "normal", offset: 0x1c,
		Values: []Value{{"normal", "Normal"}, {"grandprix", "Grand Prix"}, {"endurance", "Endurance"}},
		bytes:  map[string]byte{"normal": 0, "grandprix": 1, "endurance": 2},
		field:  func(r *Race) *string { return &r.GameMode }},
	{Key: "rival_arrow", Env: "DAYTONA_RIVAL_ARROW", Label: "Rival arrow", def: "on", offset: 0x1d,
		Values: []Value{{"on", "On"}, {"off", "Off"}},
		bytes:  map[string]byte{"on": 0, "off": 1},
		field:  func(r *Race) *string { return &r.RivalArrow }},
	{Key: "advertise_sound", Env: "DAYTONA_ADVERTISE_SOUND", Label: "Advertise sound", def: "on", offset: 0x19,
		Values: []Value{{"on", "On"}, {"off", "Off"}},
		bytes:  map[string]byte{"on": 0, "off": 1},
		field:  func(r *Race) *string { return &r.AdvertiseSound }},
	{Key: "free_play", Env: "DAYTONA_FREE_PLAY", Label: "Free play", def: "on",
		Values: []Value{{"on", "On (Start only)"}, {"off", "Off (coins)"}},
		field:  func(r *Race) *string { return &r.FreePlay }},
	{Key: "start", Env: "DAYTONA_START", Label: "Who may start", def: "master",
		Values: []Value{{"master", "The master"}, {"anyone", "Anyone"}},
		field:  func(r *Race) *string { return &r.Start }},
}

// eepromOrder: the plain EEPROM bytes in the order the status lists them.
var eepromOrder = []string{"advertise_sound", "cabinet", "country", "game_mode", "rival_arrow", "difficulty"}

// Options lists the settings and their values.
func Options() []Option { return options }

func (o Option) valid(v string) bool {
	for _, x := range o.Values {
		if x.Name == v {
			return true
		}
	}
	return false
}

func (o Option) names() string {
	var n []string
	for _, x := range o.Values {
		n = append(n, x.Name)
	}
	return strings.Join(n, ", ")
}

// FromEnv reads the settings; getenv is os.Getenv (a parameter for tests).
// An empty variable takes its default.
func FromEnv(getenv func(string) string) (Race, error) {
	var r Race
	for _, o := range options {
		v := strings.ToLower(strings.TrimSpace(getenv(o.Env)))
		if v == "" {
			v = o.def
		}
		if !o.valid(v) {
			return Race{}, fmt.Errorf("invalid %s=%q (use: %s)", o.Env, v, o.names())
		}
		*o.field(&r) = v
	}
	return r, nil
}

// Validate checks every value; the error names the first wrong one by its
// JSON key.
func (r Race) Validate() error {
	for _, o := range options {
		if v := *o.field(&r); !o.valid(v) {
			return fmt.Errorf("invalid %s %q (use: %s)", o.Key, v, o.names())
		}
	}
	return nil
}

// EEPROM: the bytes every game of the room applies. r must be valid.
func (r Race) EEPROM() []proto.Setting {
	var out []proto.Setting
	for _, key := range eepromOrder {
		for _, o := range options {
			if o.Key == key {
				out = append(out, proto.Setting{Offset: o.offset, Value: o.bytes[*o.field(&r)]})
			}
		}
	}
	if r.FreePlay == "on" {
		// coin assignment 27 (FREE PLAY) and the free-play flag, as the PC solo mode sets them
		out = append(out, proto.Setting{Offset: 0x10, Value: 26}, proto.Setting{Offset: 0x14, Value: 1})
	}
	return out
}

// Apply sets the values of spec, "key:value,key:value" (the game's set=),
// over r; an unknown key or value is an error and leaves r unchanged.
func (r *Race) Apply(spec string) error {
	next := *r
	for _, kv := range strings.Split(spec, ",") {
		if strings.TrimSpace(kv) == "" {
			continue
		}
		k, v, _ := strings.Cut(kv, ":")
		k, v = strings.ToLower(strings.TrimSpace(k)), strings.ToLower(strings.TrimSpace(v))
		found := false
		for _, o := range options {
			if o.Key != k {
				continue
			}
			if !o.valid(v) {
				return fmt.Errorf("invalid %s %q (use: %s)", k, v, o.names())
			}
			*o.field(&next) = v
			found = true
		}
		if !found {
			return fmt.Errorf("unknown setting %q", k)
		}
	}
	*r = next
	return nil
}

// Info is op=info's answer: "key=value" lines, every setting.
func (r Race) Info() string {
	var b strings.Builder
	for _, o := range options {
		b.WriteString(o.Key + "=" + *o.field(&r) + "\n")
	}
	return b.String()
}

// AnyStart: any player may start a race, not only the MASTER.
func (r Race) AnyStart() bool { return r.Start == "anyone" }

// String describes the settings for the log.
func (r Race) String() string {
	var d []string
	for _, o := range options {
		d = append(d, o.Key+"="+*o.field(&r))
	}
	return strings.Join(d, " ")
}
