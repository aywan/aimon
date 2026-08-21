// Package board provides a high-level board client that tracks desired vs
// last-flushed state and pushes the full desired JSON over BLE on Flush.
package board

import (
	"encoding/json"
	"fmt"
	"reflect"
	"sync"
	"time"

	"aiboard-client/internal/ble"

	"go.uber.org/zap"
)

// Sender is the low-level transport used by Client.Flush.
type Sender interface {
	SendJSON(payload string, hold time.Duration) error
}

// Client stores last flushed and desired board state.
type Client struct {
	log  *zap.SugaredLogger
	ble  Sender
	hold time.Duration

	mu   sync.Mutex
	last State
	next State
}

// Option configures a board Client.
type Option func(*Client)

// WithHold keeps the BLE connection open this long after a successful Flush write.
func WithHold(d time.Duration) Option {
	return func(c *Client) {
		c.hold = d
	}
}

// New wraps a BLE (or other) sender.
func New(log *zap.SugaredLogger, sender Sender, opts ...Option) *Client {
	if log == nil {
		log = zap.NewNop().Sugar()
	}
	c := &Client{
		log: log.Named("board"),
		ble: sender,
	}
	for _, opt := range opts {
		opt(c)
	}
	return c
}

// NewFromBLE builds a board client over a ble.Client.
func NewFromBLE(log *zap.SugaredLogger, bleClient *ble.Client, opts ...Option) *Client {
	return New(log, bleClient, opts...)
}

const timeResyncInterval = time.Minute

func ptr[T any](v T) *T { return &v }

func stampComputerTime(s *State) {
	now := time.Now()
	u := now.Unix()
	_, off := now.Zone()
	s.Unix = &u
	s.TzOffset = &off
}

func slotsEqual(a, b State) bool {
	return reflect.DeepEqual(a.Slot1, b.Slot1) &&
		reflect.DeepEqual(a.Slot2, b.Slot2) &&
		reflect.DeepEqual(a.Slot3, b.Slot3) &&
		reflect.DeepEqual(a.Slot4, b.Slot4)
}

// SetSlot replaces desired state for slot n (1-4).
func (c *Client) SetSlot(n int, slot Slot) {
	c.mu.Lock()
	defer c.mu.Unlock()
	dst := c.next.slotPtr(n)
	if dst == nil {
		return
	}
	copy := slot.Clone()
	*dst = &copy
}

// SetGauge puts a 0-100 gauge with a bottom caption into slot n (1-4).
// fg and bg are optional "#RRGGBB" arc colors; empty string leaves the board default.
func (c *Client) SetGauge(n int, value float64, label, fg, bg string) {
	slot := Slot{
		Type:  "gauge",
		Value: ptr(value),
		Label: ptr(label),
	}
	if fg != "" {
		slot.Fg = ptr(fg)
	}
	if bg != "" {
		slot.Bg = ptr(bg)
	}
	c.SetSlot(n, slot)
}

// SetText puts a text widget into slot n (1-4). Label is the optional caption.
// fg is optional "#RRGGBB" body color; empty string leaves the board default.
func (c *Client) SetText(n int, text, label, fg string) {
	slot := Slot{Type: "text", Text: ptr(text)}
	if label != "" {
		slot.Label = ptr(label)
	}
	if fg != "" {
		slot.Fg = ptr(fg)
	}
	c.SetSlot(n, slot)
}

// ClearSlot empties slot n (1-4) on the next flush.
func (c *Client) ClearSlot(n int) {
	c.SetSlot(n, Slot{Type: "empty"})
}

// ApplyJSON merges a JSON object into the desired state.
func (c *Client) ApplyJSON(raw string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.next.MergeFromJSON([]byte(raw))
}

// Flush compares desired and last state; on mismatch sends the full desired
// JSON via the low-level client and updates last on success.
func (c *Client) Flush() error {
	return c.flush(false)
}

// ForceFlush always sends the full desired JSON, even when it matches last.
func (c *Client) ForceFlush() error {
	return c.flush(true)
}

func (c *Client) flush(force bool) error {
	c.mu.Lock()
	stampComputerTime(&c.next)
	if !force && slotsEqual(c.last, c.next) && c.last.Unix != nil && c.next.Unix != nil &&
		*c.next.Unix-*c.last.Unix < int64(timeResyncInterval/time.Second) {
		c.mu.Unlock()
		c.log.Debug("flush skipped: state unchanged")
		return nil
	}

	payload, err := json.Marshal(c.next)
	if err != nil {
		c.mu.Unlock()
		return fmt.Errorf("marshal state: %w", err)
	}
	sent := c.next.Clone()
	c.mu.Unlock()

	c.log.Infof("flushing state: %s", payload)
	if err := c.ble.SendJSON(string(payload), c.hold); err != nil {
		return err
	}

	c.mu.Lock()
	c.last = sent
	c.mu.Unlock()
	return nil
}

// Desired returns a snapshot of the desired state.
func (c *Client) Desired() State {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.next.Clone()
}
