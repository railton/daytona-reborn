package settings

import (
	"strings"
	"testing"
)

func eepromMap(r Race) map[byte]byte {
	m := map[byte]byte{}
	for _, s := range r.EEPROM() {
		m[s.Offset] = s.Value
	}
	return m
}

func TestDefaultsAndEnv(t *testing.T) {
	env := map[string]string{"DAYTONA_DIFFICULTY": "Hardest"} // country: usa by default
	r, err := FromEnv(func(k string) string { return env[k] })
	if err != nil {
		t.Fatal(err)
	}
	want := map[byte]byte{0x19: 0, 0x1a: 1, 0x1b: 0, 0x1c: 0, 0x1d: 0, 0x20: 3, 0x10: 26, 0x14: 1}
	got := eepromMap(r)
	if len(r.EEPROM()) != len(want) || len(got) != len(want) {
		t.Fatalf("%d settings", len(r.EEPROM()))
	}
	for o, v := range want {
		if got[o] != v {
			t.Errorf("0x%02x = %d, want %d", o, got[o], v)
		}
	}
	if r.AnyStart() || r.Start != "master" {
		t.Errorf("start = %q", r.Start)
	}
	if _, err := FromEnv(func(k string) string {
		if k == "DAYTONA_CABINET" {
			return "giant"
		}
		return ""
	}); err == nil {
		t.Error("accepted DAYTONA_CABINET=giant")
	}
	if _, err := FromEnv(func(k string) string {
		if k == "DAYTONA_START" {
			return "nobody"
		}
		return ""
	}); err == nil {
		t.Error("accepted DAYTONA_START=nobody")
	}
}

func TestCountryJapan(t *testing.T) {
	r, err := FromEnv(func(k string) string {
		if k == "DAYTONA_COUNTRY" {
			return "jpn"
		}
		return ""
	})
	if err != nil {
		t.Fatal(err)
	}
	if v := eepromMap(r)[0x1b]; v != 1 {
		t.Errorf("jpn = %d, want 1", v)
	}
}

// Every value of every option maps to its EEPROM byte; free play off leaves
// the coin bytes alone.
func TestEveryValue(t *testing.T) {
	base, _ := FromEnv(func(string) string { return "" })
	want := map[string]map[string][2]byte{ // key -> value -> offset, byte
		"cabinet":         {"twin": {0x1a, 1}, "uplight": {0x1a, 2}, "deluxe": {0x1a, 0}},
		"country":         {"usa": {0x1b, 0}, "jpn": {0x1b, 1}, "export": {0x1b, 2}},
		"difficulty":      {"easy": {0x20, 0}, "normal": {0x20, 1}, "hard": {0x20, 2}, "hardest": {0x20, 3}},
		"game_mode":       {"normal": {0x1c, 0}, "grandprix": {0x1c, 1}, "endurance": {0x1c, 2}},
		"rival_arrow":     {"on": {0x1d, 0}, "off": {0x1d, 1}},
		"advertise_sound": {"on": {0x19, 0}, "off": {0x19, 1}},
	}
	for _, o := range Options() {
		for _, v := range o.Values {
			r := base
			*o.field(&r) = v.Name
			if err := r.Validate(); err != nil {
				t.Fatalf("%s=%s: %v", o.Key, v.Name, err)
			}
			if w, ok := want[o.Key]; ok {
				ob := w[v.Name]
				if got := eepromMap(r)[ob[0]]; got != ob[1] {
					t.Errorf("%s=%s: 0x%02x = %d, want %d", o.Key, v.Name, ob[0], got, ob[1])
				}
			}
		}
		r := base
		*o.field(&r) = "bogus"
		if r.Validate() == nil {
			t.Errorf("%s=bogus accepted", o.Key)
		}
	}
	off := base
	off.FreePlay = "off"
	m := eepromMap(off)
	if _, ok := m[0x10]; ok || len(m) != 6 {
		t.Errorf("free play off: %v", m)
	}
	any := base
	any.Start = "anyone"
	if !any.AnyStart() {
		t.Error("start=anyone")
	}
}

func TestApply(t *testing.T) {
	r, _ := FromEnv(func(string) string { return "" })
	base := r
	if err := r.Apply("Difficulty:HARD, cabinet:deluxe,,start:anyone"); err != nil {
		t.Fatal(err)
	}
	if r.Difficulty != "hard" || r.Cabinet != "deluxe" || !r.AnyStart() || r.Country != base.Country {
		t.Errorf("applied: %+v", r)
	}
	for _, bad := range []string{"difficulty:insane", "volume:11", "cabinet"} {
		before := r
		if err := r.Apply(bad); err == nil || r != before {
			t.Errorf("%q: %v, %+v", bad, err, r)
		}
	}
	if err := r.Apply(""); err != nil {
		t.Errorf("empty: %v", err)
	}
	if !strings.Contains(base.Info(), "difficulty=normal\n") || strings.Count(base.Info(), "\n") != len(Options()) {
		t.Errorf("info %q", base.Info())
	}
}
