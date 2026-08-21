package main

import (
	"testing"
	"time"
)

func TestFormatRemaining(t *testing.T) {
	cases := []struct {
		until time.Duration
		text  string
		label string
	}{
		{8*24*time.Hour + 3*time.Hour, "8d3h", "to reset"},
		{24 * time.Hour, "1d0h", "to reset"},
		{24*time.Hour + 3*time.Hour, "1d3h", "to reset"},
		{23 * time.Hour, "23h", "to reset"},
		{10 * time.Hour, "10h", "to reset"},
		{10*time.Hour - time.Minute, "9h59m", "to reset"},
		{9*time.Hour + 50*time.Minute, "9h50m", "to reset"},
		{time.Hour, "1h0m", "to reset"},
		{59 * time.Minute, "59m", "to reset"},
		{time.Minute, "1m", "to reset"},
		{30 * time.Second, "1m", "to reset"},
		{0, "0m", "to reset"},
		{-time.Hour, "0m", "to reset"},
	}
	for _, tc := range cases {
		text, label := formatRemaining(tc.until)
		if text != tc.text || label != tc.label {
			t.Errorf("formatRemaining(%v) = %q %q, want %q %q",
				tc.until, text, label, tc.text, tc.label)
		}
	}
}
