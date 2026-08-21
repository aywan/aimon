package main

import (
	"context"
	"errors"
	"fmt"
	"time"

	"aiboard-client/internal/board"
	"aiboard-client/internal/codex"
	"aiboard-client/internal/ipc"

	"go.uber.org/zap"
)

const resetWindow = 7 * 24 * time.Hour

const (
	quotaGaugeFg = "#3ecf8e"
	quotaGaugeBg = "#1a3328"
	resetGaugeFg = "#40a0ff"
	resetGaugeBg = "#1a2833"
)

// resetProgressPercent maps 7 days until reset -> 0%, reset moment -> 100%.
func resetProgressPercent(resetAt time.Time) float64 {
	until := time.Until(resetAt)
	if until >= resetWindow {
		return 0
	}
	if until <= 0 {
		return 100
	}
	return (1 - until.Seconds()/resetWindow.Seconds()) * 100
}

const remainingCaption = "to reset"

// formatRemaining compact duration for slot_2: "1d3h" / "15h" / "9h50m".
// Minutes only when remaining is under 10 hours. Latin only: board font
// has no Cyrillic. Caption is always "to reset".
func formatRemaining(until time.Duration) (text, label string) {
	if until < 0 {
		until = 0
	}
	label = remainingCaption

	const tenHours = 10 * time.Hour
	if until >= tenHours {
		days := int(until / (24 * time.Hour))
		hours := int((until % (24 * time.Hour)) / time.Hour)
		if days > 0 {
			return fmt.Sprintf("%dd%dh", days, hours), label
		}
		return fmt.Sprintf("%dh", hours), label
	}

	hours := int(until / time.Hour)
	minutes := int((until % time.Hour) / time.Minute)
	if until > 0 && hours == 0 && minutes == 0 {
		minutes = 1
	}
	if hours > 0 {
		return fmt.Sprintf("%dh%dm", hours, minutes), label
	}
	return fmt.Sprintf("%dm", minutes), label
}

func applyCodexQuota(log *zap.SugaredLogger, limits codex.Limits, boardClient *board.Client) error {
	remaining, err := limits.LimitsRemainingPercent()
	if err != nil {
		return err
	}

	resetAt, err := limits.LimitsResetAt()
	if err != nil {
		return err
	}

	until := time.Until(resetAt)
	leftText, leftLabel := formatRemaining(until)
	resetProgress := resetProgressPercent(resetAt)

	log.Infof("Сброс: %s", resetAt.Local().Format("02.01.2006 15:04:05 MST"))

	boardClient.SetGauge(1, remaining, "codex", quotaGaugeFg, quotaGaugeBg)
	boardClient.SetText(2, leftText, leftLabel, "")
	boardClient.SetGauge(3, resetProgress, "reset", resetGaugeFg, resetGaugeBg)

	log.Infof("codex quota: remaining=%.1f%% left=%s %s reset=%.1f%%",
		remaining, leftText, leftLabel, resetProgress)

	return boardClient.Flush()
}

func runCodexQuota(log *zap.SugaredLogger, boardClient *board.Client, interval time.Duration, socketPath string) {
	log = log.Named("codex.run")
	log.Infof("codex run started (interval %s)", interval)

	hooks := newHookReactor(log, boardClient)
	boardClient.SetText(4, "idle", "status", statusIdleFg)
	ipcServer := ipc.NewServer(log, socketPath, hooks.Apply)
	go func() {
		if err := ipcServer.Serve(); err != nil {
			log.Warnf("ipc server stopped: %v", err)
		}
	}()
	defer ipcServer.Close() //nolint:errcheck

	client, err := codex.NewCachedClient(log)
	if err != nil {
		log.Fatalf("codex client: %v", err)
	}
	defer client.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	err = client.Start(ctx)
	cancel()
	if err != nil {
		log.Fatalf("codex start: %v", err)
	}

	for {
		if err := applyCodexQuota(log, client, boardClient); err != nil {
			if errors.Is(err, codex.ErrNotInitialized) {
				log.Warnf("codex not initialized yet: %v", err)
			} else {
				log.Warnf("codex quota update failed: %v", err)
			}
		}

		time.Sleep(interval)
	}
}
