package board

import "encoding/json"

const SlotCount = 4

// Slot is one cell of the main-screen 4-slot row.
// Type is "gauge", "text", or "empty" (clears the cell).
type Slot struct {
	Type  string   `json:"type,omitempty"`
	Value *float64 `json:"value,omitempty"`
	Label *string  `json:"label,omitempty"`
	Text  *string  `json:"text,omitempty"`
	Fg    *string  `json:"fg,omitempty"`
	Bg    *string  `json:"bg,omitempty"`
}

func (s Slot) Clone() Slot {
	out := Slot{Type: s.Type}
	if s.Value != nil {
		v := *s.Value
		out.Value = &v
	}
	if s.Label != nil {
		v := *s.Label
		out.Label = &v
	}
	if s.Text != nil {
		v := *s.Text
		out.Text = &v
	}
	if s.Fg != nil {
		v := *s.Fg
		out.Fg = &v
	}
	if s.Bg != nil {
		v := *s.Bg
		out.Bg = &v
	}
	return out
}

func cloneSlotPtr(s *Slot) *Slot {
	if s == nil {
		return nil
	}
	c := s.Clone()
	return &c
}

func mergeSlot(dst **Slot, raw json.RawMessage) error {
	if string(raw) == "null" {
		empty := Slot{Type: "empty"}
		*dst = &empty
		return nil
	}
	var patch Slot
	if err := json.Unmarshal(raw, &patch); err != nil {
		return err
	}
	if *dst == nil {
		*dst = &Slot{}
	}
	if patch.Type != "" {
		(*dst).Type = patch.Type
		if patch.Type == "empty" {
			(*dst).Value = nil
			(*dst).Label = nil
			(*dst).Text = nil
			(*dst).Fg = nil
			(*dst).Bg = nil
			return nil
		}
	}
	if patch.Value != nil {
		v := *patch.Value
		(*dst).Value = &v
	}
	if patch.Label != nil {
		v := *patch.Label
		(*dst).Label = &v
	}
	if patch.Text != nil {
		v := *patch.Text
		(*dst).Text = &v
	}
	if patch.Fg != nil {
		v := *patch.Fg
		(*dst).Fg = &v
	}
	if patch.Bg != nil {
		v := *patch.Bg
		(*dst).Bg = &v
	}
	return nil
}

// State is the board control payload. Pointer/omitempty fields mean "unset"
// (leave the board's current slot as-is). Send type "empty" or JSON null to
// clear a slot. Unix/TzOffset are the computer's clock (UTC seconds + offset
// east of UTC); the board stores local civil time on the PCF85063.
type State struct {
	Unix     *int64 `json:"unix,omitempty"`
	TzOffset *int   `json:"tz_offset,omitempty"`
	Slot1    *Slot  `json:"slot_1,omitempty"`
	Slot2    *Slot  `json:"slot_2,omitempty"`
	Slot3    *Slot  `json:"slot_3,omitempty"`
	Slot4    *Slot  `json:"slot_4,omitempty"`
}

func (s *State) slotPtr(n int) **Slot {
	switch n {
	case 1:
		return &s.Slot1
	case 2:
		return &s.Slot2
	case 3:
		return &s.Slot3
	case 4:
		return &s.Slot4
	default:
		return nil
	}
}

// Clone returns a deep copy suitable for last/next snapshots.
func (s State) Clone() State {
	out := State{
		Slot1: cloneSlotPtr(s.Slot1),
		Slot2: cloneSlotPtr(s.Slot2),
		Slot3: cloneSlotPtr(s.Slot3),
		Slot4: cloneSlotPtr(s.Slot4),
	}
	if s.Unix != nil {
		v := *s.Unix
		out.Unix = &v
	}
	if s.TzOffset != nil {
		v := *s.TzOffset
		out.TzOffset = &v
	}
	return out
}

// MergeFromJSON parses a JSON object and overlays its fields onto s.
func (s *State) MergeFromJSON(raw []byte) error {
	var patch map[string]json.RawMessage
	if err := json.Unmarshal(raw, &patch); err != nil {
		return err
	}

	if v, ok := patch["unix"]; ok {
		var n int64
		if err := json.Unmarshal(v, &n); err != nil {
			return err
		}
		s.Unix = &n
	}
	if v, ok := patch["tz_offset"]; ok {
		var n int
		if err := json.Unmarshal(v, &n); err != nil {
			return err
		}
		s.TzOffset = &n
	}

	keys := []string{"slot_1", "slot_2", "slot_3", "slot_4"}
	for i, key := range keys {
		v, ok := patch[key]
		if !ok {
			continue
		}
		if err := mergeSlot(s.slotPtr(i+1), v); err != nil {
			return err
		}
	}
	return nil
}
