// Command aiboard-client sends a JSON control payload to the aw_extramon_1
// ESP32-S3 board over BLE.
//
// Usage:
//
//	aiboard-client send --json '{"slot_1":{"type":"gauge","value":80,"label":"codex","fg":"#3ecf8e","bg":"#1a3328"},"slot_2":{"type":"text","text":"3","label":"days"},"slot_3":{"type":"gauge","value":40,"label":"reset","fg":"#40a0ff","bg":"#1a2833"},"slot_4":{"type":"text","text":"idle","label":"status","fg":"#c8c8c8"}}'
//	aiboard-client status
//	aiboard-client run                # polls codex quota; accepts hook IPC
//	aiboard-client hook SessionStart  # notify a running `run` daemon
//
// The board must already be bonded with this computer (tap Pair on the
// board's Settings screen and enter the passkey; already-bonded reconnects
// work anytime). Forget on the board does not drop macOS's LTK — forget
// the device in System Settings too.
package main

import (
	"os"
	"time"

	"aiboard-client/internal/ble"
	"aiboard-client/internal/board"
	"aiboard-client/internal/ipc"

	"github.com/spf13/cobra"
	"go.uber.org/zap"
)

var (
	log *zap.SugaredLogger

	deviceName  string
	scanTimeout time.Duration

	jsonPayload string
	hold        time.Duration

	pollInterval time.Duration
	socketPath   string
)

func main() {
	zapLog, err := zap.NewDevelopment()
	if err != nil {
		panic(err)
	}
	defer zapLog.Sync() //nolint:errcheck
	log = zapLog.Sugar()

	rootCmd := &cobra.Command{
		Use:   "aiboard-client",
		Short: "Control aw_extramon_1 over BLE",
	}

	rootCmd.PersistentFlags().StringVar(&deviceName, "name", ble.DefaultDeviceName, "BLE device name to look for")
	rootCmd.PersistentFlags().DurationVar(&scanTimeout, "timeout", 15*time.Second, "how long to scan before giving up")
	rootCmd.PersistentFlags().StringVar(&socketPath, "socket", ipc.DefaultSocketPath, "Unix socket for run↔hook IPC")

	rootCmd.AddCommand(
		newSendCmd(),
		newStatusCmd(),
		newRunCmd(),
		newHookCmd(),
	)

	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func newBLEClient() *ble.Client {
	return ble.New(
		log,
		ble.WithDeviceName(deviceName),
		ble.WithScanTimeout(scanTimeout),
	)
}

func newSendCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "send",
		Short: "Send a JSON control payload to the board",
		Run: func(cmd *cobra.Command, args []string) {
			payload := jsonPayload
			if payload == "" {
				if hold == 0 {
					log.Fatal("missing --json payload (or pass --hold to just observe the connection)")
				}
				payload = "{}"
			}

			bleClient := newBLEClient()
			if err := bleClient.Enable(); err != nil {
				log.Fatal(err)
			}

			boardClient := board.NewFromBLE(log, bleClient, board.WithHold(hold))
			if err := boardClient.ApplyJSON(payload); err != nil {
				log.Fatal(err)
			}
			if err := boardClient.ForceFlush(); err != nil {
				log.Fatal(err)
			}
		},
	}

	cmd.Flags().StringVar(&jsonPayload, "json", "", `JSON payload to send, e.g. {"slot_1":{"type":"gauge","value":80,"label":"codex"}}`)
	cmd.Flags().DurationVar(&hold, "hold", 0, "keep the connection open this long after writing before disconnecting")
	return cmd
}

func newStatusCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "status",
		Short: "Scan for the device and report whether it's reachable",
		Run: func(cmd *cobra.Command, args []string) {
			bleClient := newBLEClient()
			if err := bleClient.Enable(); err != nil {
				log.Fatal(err)
			}

			result, err := bleClient.Scan()
			if err != nil {
				log.Fatalf("NOT FOUND: %v", err)
			}
			log.Infof("FOUND %s: address=%s rssi=%d dBm", deviceName, result.Address, result.RSSI)
		},
	}
}

func newRunCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "run",
		Short: "Poll codex quota and accept hook IPC from `hook` commands",
		Run: func(cmd *cobra.Command, args []string) {
			bleClient := newBLEClient()
			if err := bleClient.Enable(); err != nil {
				log.Fatal(err)
			}
			log.Info("ble recovery: cached-address reconnect + adapter reset on failure")

			boardClient := board.NewFromBLE(log, bleClient)
			runCodexQuota(log, boardClient, pollInterval, socketPath)
		},
	}

	cmd.Flags().DurationVar(&pollInterval, "interval", time.Minute, "how often to poll codex quota")
	return cmd
}

func newHookCmd() *cobra.Command {
	return &cobra.Command{
		Use:   "hook <name>",
		Short: "Send a hook event to a running `run` daemon (e.g. SessionStart, Stop)",
		Args:  cobra.ExactArgs(1),
		Run: func(cmd *cobra.Command, args []string) {
			name := args[0]
			handled, err := ipc.SendRequest(socketPath, ipc.Request{
				Cmd:  ipc.CmdHook,
				Args: []string{name},
			})
			if !handled {
				log.Fatalf("no run daemon listening on %s — start `aiboard-client run` first", socketPath)
			}
			if err != nil {
				log.Fatal(err)
			}
			log.Infof("hook %s delivered", name)
		},
	}
}
