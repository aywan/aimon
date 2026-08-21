package main

import (
	"fmt"
	"strings"
	"sync"
	"time"
	"unicode"

	"aiboard-client/internal/board"

	"go.uber.org/zap"
)

const idleAfter = 10 * time.Minute

// Mood colors for slot_4: one hue per hook, readable on a dark LCD.
const (
	statusIdleFg = "#c8c8c8" // quiet wait

	hookSessionStartFg      = "#22d3ee" // dawn
	hookUserPromptSubmitFg  = "#60a5fa" // listening
	hookPreToolUseFg        = "#facc15" // focus
	hookPostToolUseFg       = "#4ade80" // productive
	hookPermissionRequestFg = "#ff4a1a" // alert
	hookPreCompactFg        = "#c084fc" // pressure
	hookPostCompactFg       = "#2dd4bf" // relief
	hookSubagentStartFg     = "#f472b6" // curious
	hookSubagentStopFg      = "#e879f9" // helper done
	hookStopFg              = "#8fbf8f" // rest
	hookSessionEndFg        = "#818cf8" // dusk
	hookUnknownFg           = "#d6d3d1" // unexplained
)

func hookFg(name string) string {
	switch name {
	case "SessionStart":
		return hookSessionStartFg
	case "UserPromptSubmit":
		return hookUserPromptSubmitFg
	case "PreToolUse":
		return hookPreToolUseFg
	case "PostToolUse":
		return hookPostToolUseFg
	case "PermissionRequest":
		return hookPermissionRequestFg
	case "PreCompact":
		return hookPreCompactFg
	case "PostCompact":
		return hookPostCompactFg
	case "SubagentStart":
		return hookSubagentStartFg
	case "SubagentStop":
		return hookSubagentStopFg
	case "Stop":
		return hookStopFg
	case "SessionEnd":
		return hookSessionEndFg
	default:
		return hookUnknownFg
	}
}

// hookDisplayText is the hook name itself, broken on CamelCase so it
// wraps cleanly in a 160px slot (Montserrat 20 has no room for
// "PermissionRequest" on one line).
func hookDisplayText(name string) string {
	var b strings.Builder
	for i, r := range name {
		if i > 0 && unicode.IsUpper(r) {
			b.WriteByte('\n')
		}
		b.WriteRune(r)
	}
	return b.String()
}

// hookReactor writes the hook name as text in slot_4 and restores
// "idle" ten minutes after the last hook.
type hookReactor struct {
	log   *zap.SugaredLogger
	board *board.Client

	mu         sync.Mutex
	resetTimer *time.Timer
}

func newHookReactor(log *zap.SugaredLogger, boardClient *board.Client) *hookReactor {
	return &hookReactor{
		log:   log.Named("hook"),
		board: boardClient,
	}
}

// Apply paints slot_4 with the hook name. Unknown names still go to
// the board (stone color) so a new Codex event is visible immediately.
func (r *hookReactor) Apply(name string) error {
	name = strings.TrimSpace(name)
	if name == "" {
		return fmt.Errorf("empty hook name")
	}

	text := hookDisplayText(name)
	fg := hookFg(name)
	r.board.SetText(4, text, "status", fg)
	r.scheduleIdle()
	r.log.Infof("%s → slot_4 %q %s", name, text, fg)

	return r.board.Flush()
}

func (r *hookReactor) scheduleIdle() {
	r.mu.Lock()
	defer r.mu.Unlock()

	if r.resetTimer != nil {
		r.resetTimer.Stop()
	}
	r.resetTimer = time.AfterFunc(idleAfter, r.resetIdle)
}

func (r *hookReactor) resetIdle() {
	r.log.Infof("idle timeout (%s) → slot_4 idle", idleAfter)
	r.board.SetText(4, "idle", "status", statusIdleFg)
	if err := r.board.Flush(); err != nil {
		r.log.Warnf("idle reset flush failed: %v", err)
	}
}
