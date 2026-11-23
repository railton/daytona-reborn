package settings

import "testing"

func TestDefaultsAndEnv(t *testing.T) {
	env := map[string]string{"DAYTONA_DIFFICULTY": "Hardest"} // country: usa by default
	got, _, err := FromEnv(func(k string) string { return env[k] })
	if err != nil {
		t.Fatal(err)
	}
	want := map[byte]byte{0x19: 0, 0x1a: 1, 0x1b: 0, 0x1c: 0, 0x1d: 0, 0x20: 3, 0x10: 26, 0x14: 1}
	if len(got) != len(want) {
		t.Fatalf("%d settings", len(got))
	}
	for _, s := range got {
		if want[s.Offset] != s.Value {
			t.Errorf("0x%02x = %d, want %d", s.Offset, s.Value, want[s.Offset])
		}
	}
	if _, _, err := FromEnv(func(k string) string {
		if k == "DAYTONA_CABINET" {
			return "giant"
		}
		return ""
	}); err == nil {
		t.Error("accepted DAYTONA_CABINET=giant")
	}
}

func TestCountryJapan(t *testing.T) {
	got, _, err := FromEnv(func(k string) string {
		if k == "DAYTONA_COUNTRY" {
			return "jpn"
		}
		return ""
	})
	if err != nil {
		t.Fatal(err)
	}
	for _, s := range got {
		if s.Offset == 0x1b && s.Value != 1 {
			t.Errorf("jpn = %d, want 1", s.Value)
		}
	}
}
