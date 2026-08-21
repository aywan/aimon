package board

import (
	"encoding/json"
	"testing"
)

func TestMergeFromJSONTimeAndSlots(t *testing.T) {
	var s State
	err := s.MergeFromJSON([]byte(`{"unix":1755460500,"tz_offset":10800,"slot_1":{"type":"text","text":"hi"}}`))
	if err != nil {
		t.Fatal(err)
	}
	if s.Unix == nil || *s.Unix != 1755460500 {
		t.Fatalf("unix = %v", s.Unix)
	}
	if s.TzOffset == nil || *s.TzOffset != 10800 {
		t.Fatalf("tz_offset = %v", s.TzOffset)
	}
	if s.Slot1 == nil || s.Slot1.Type != "text" || s.Slot1.Text == nil || *s.Slot1.Text != "hi" {
		t.Fatalf("slot_1 = %+v", s.Slot1)
	}

	clone := s.Clone()
	*clone.Unix = 1
	if *s.Unix == 1 {
		t.Fatal("Clone shared unix pointer")
	}
}

func TestSlotsEqualIgnoresTime(t *testing.T) {
	a := State{Unix: ptr[int64](1), Slot1: &Slot{Type: "text"}}
	b := State{Unix: ptr[int64](2), Slot1: &Slot{Type: "text"}}
	if !slotsEqual(a, b) {
		t.Fatal("slotsEqual should ignore unix")
	}
	b.Slot1 = &Slot{Type: "gauge"}
	if slotsEqual(a, b) {
		t.Fatal("slotsEqual should see slot type change")
	}
}

func TestStampComputerTime(t *testing.T) {
	var s State
	stampComputerTime(&s)
	if s.Unix == nil || *s.Unix <= 0 {
		t.Fatalf("unix not stamped: %v", s.Unix)
	}
	if s.TzOffset == nil {
		t.Fatal("tz_offset not stamped")
	}
	raw, err := json.Marshal(s)
	if err != nil {
		t.Fatal(err)
	}
	var got map[string]any
	if err := json.Unmarshal(raw, &got); err != nil {
		t.Fatal(err)
	}
	if _, ok := got["unix"]; !ok {
		t.Fatalf("marshal missing unix: %s", raw)
	}
	if _, ok := got["tz_offset"]; !ok {
		t.Fatalf("marshal missing tz_offset: %s", raw)
	}
}
