// Package settings: the race settings every game in a room uses, from the
// server's environment (the Docker container's variables). They are bytes of
// the I/O board's settings EEPROM, the test menu's GAME SYSTEM page, mapped on
// Daytona USA Revision A:
//
//	DAYTONA_CABINET          twin | uplight | deluxe        (0x1a: 01 02 00)
//	DAYTONA_COUNTRY          jpn | usa | export             (0x1b: 01 00 02)
//	DAYTONA_DIFFICULTY       easy | normal | hard | hardest (0x20: 00 01 02 03)
//	DAYTONA_GAME_MODE        normal | grandprix | endurance (0x1c: 00 01 02)
//	DAYTONA_RIVAL_ARROW      on | off                       (0x1d: 00 01)
//	DAYTONA_ADVERTISE_SOUND  on | off                       (0x19: 00 01)
//	DAYTONA_FREE_PLAY        on | off: no coins, Start only (0x10 = 26, 0x14 = 1)
//
// LINK ID and CAR NUMBER are not here: each game sets its own from its place
// in the room (the first is the MASTER).
package settings

import (
	"fmt"
	"strings"

	"daytona-relay/proto"
)

type option struct {
	env    string
	offset byte
	def    string
	values map[string]byte
}

var options = []option{
	{"DAYTONA_ADVERTISE_SOUND", 0x19, "on", map[string]byte{"on": 0, "off": 1}},
	{"DAYTONA_CABINET", 0x1a, "twin", map[string]byte{"twin": 1, "uplight": 2, "deluxe": 0}},
	{"DAYTONA_COUNTRY", 0x1b, "jpn", map[string]byte{"jpn": 1, "usa": 0, "export": 2}},
	{"DAYTONA_GAME_MODE", 0x1c, "normal", map[string]byte{"normal": 0, "grandprix": 1, "endurance": 2}},
	{"DAYTONA_RIVAL_ARROW", 0x1d, "on", map[string]byte{"on": 0, "off": 1}},
	{"DAYTONA_DIFFICULTY", 0x20, "normal", map[string]byte{"easy": 0, "normal": 1, "hard": 2, "hardest": 3}},
}

// FromEnv reads the settings; getenv is os.Getenv (a parameter for tests).
// The text describes them for the log.
func FromEnv(getenv func(string) string) ([]proto.Setting, string, error) {
	var out []proto.Setting
	var desc []string
	for _, o := range options {
		v := strings.ToLower(strings.TrimSpace(getenv(o.env)))
		if v == "" {
			v = o.def
		}
		b, ok := o.values[v]
		if !ok {
			var valid []string
			for k := range o.values {
				valid = append(valid, k)
			}
			return nil, "", fmt.Errorf("invalid %s=%q (use: %s)", o.env, v, strings.Join(valid, ", "))
		}
		out = append(out, proto.Setting{Offset: o.offset, Value: b})
		desc = append(desc, strings.ToLower(strings.TrimPrefix(o.env, "DAYTONA_"))+"="+v)
	}
	switch v := strings.ToLower(strings.TrimSpace(getenv("DAYTONA_FREE_PLAY"))); v {
	case "", "on":
		// coin assignment 27 (FREE PLAY) and the free-play flag, as the PC solo mode sets them
		out = append(out, proto.Setting{Offset: 0x10, Value: 26}, proto.Setting{Offset: 0x14, Value: 1})
		desc = append(desc, "free_play=on")
	case "off":
		desc = append(desc, "free_play=off")
	default:
		return nil, "", fmt.Errorf("invalid DAYTONA_FREE_PLAY=%q (use: on, off)", v)
	}
	return out, strings.Join(desc, " "), nil
}
