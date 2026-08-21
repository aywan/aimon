// Package ble provides a low-level Bluetooth client that scans for the
// aw_extramon_1 board and writes raw JSON payloads to its control characteristic.
package ble

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"time"

	"go.uber.org/zap"
	"tinygo.org/x/bluetooth"
)

const (
	DefaultDeviceName  = "aw_extramon_1"
	serviceUUIDStr     = "b128a90e-9917-4c69-b86f-d1a5df5a88d0"
	controlUUIDStr     = "3aa26982-1c4a-4563-b78c-0ed9d9e07526"
	maxConnectAttempts = 3

	// Cached CoreBluetooth peripheral UUID — survives process restarts so we
	// can reconnect without scanning (macOS often stops re-delivering ads for
	// a known peripheral while AllowDuplicates is false).
	defaultAddrCachePath = "/tmp/aiboard-client.ble-addr"
)

// ScanResult is a found advertisement for the configured device.
type ScanResult struct {
	Address string
	RSSI    int16
}

// Client talks to the board over BLE.
type Client struct {
	log           *zap.SugaredLogger
	adapter       *bluetooth.Adapter
	name          string
	timeout       time.Duration
	addrCachePath string

	mu           sync.Mutex
	needsRecover bool
	cachedAddr   bluetooth.Address
	hasCached    bool
}

// Option configures a Client.
type Option func(*Client)

// WithDeviceName sets the BLE advertisement name to look for.
func WithDeviceName(name string) Option {
	return func(c *Client) {
		c.name = name
	}
}

// WithScanTimeout sets how long Scan/SendJSON wait for the device.
func WithScanTimeout(d time.Duration) Option {
	return func(c *Client) {
		c.timeout = d
	}
}

// New creates a BLE client. Defaults: device name aw_extramon_1, scan timeout 15s.
func New(log *zap.SugaredLogger, opts ...Option) *Client {
	if log == nil {
		log = zap.NewNop().Sugar()
	}
	c := &Client{
		log:           log.Named("ble"),
		adapter:       bluetooth.DefaultAdapter,
		name:          DefaultDeviceName,
		timeout:       15 * time.Second,
		addrCachePath: defaultAddrCachePath,
	}
	for _, opt := range opts {
		opt(c)
	}
	c.loadCachedAddr()
	return c
}

// Enable turns on the system Bluetooth adapter.
func (c *Client) Enable() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.enableLocked()
}

func (c *Client) enableLocked() error {
	if err := c.adapter.Enable(); err != nil {
		return fmt.Errorf("enable adapter: %w", err)
	}
	return nil
}

// Scan looks for the configured device advertisement without connecting.
func (c *Client) Scan() (ScanResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := c.prepareLocked(); err != nil {
		return ScanResult{}, err
	}

	result, err := c.scanForDeviceLocked()
	if err != nil {
		c.needsRecover = true
		return ScanResult{}, err
	}
	c.rememberAddr(result.Address)
	c.needsRecover = false
	return ScanResult{
		Address: result.Address.String(),
		RSSI:    result.RSSI,
	}, nil
}

// SendJSON connects and writes payload to the control characteristic.
// Prefers a cached CoreBluetooth UUID (no scan); falls back to scanning by name.
// If hold > 0, the connection stays open that long after a successful write.
//
// After a failed attempt the next call resets the CoreBluetooth adapter so
// macOS can recover from stale scan/connect state (common after the board
// goes out of range or the Mac sleeps).
func (c *Client) SendJSON(payload string, hold time.Duration) error {
	c.mu.Lock()
	defer c.mu.Unlock()

	if err := c.prepareLocked(); err != nil {
		return err
	}

	err := c.sendJSONLocked(payload, hold)
	if err != nil {
		c.needsRecover = true
		c.log.Warnf("BLE send failed (%v); will reset adapter before next attempt", err)
		return err
	}
	c.needsRecover = false
	return nil
}

func (c *Client) prepareLocked() error {
	if !c.needsRecover {
		return nil
	}
	return c.recoverLocked()
}

func (c *Client) recoverLocked() error {
	c.log.Warn("resetting BLE adapter...")
	if err := c.adapter.Reset(); err != nil {
		return fmt.Errorf("reset adapter: %w", err)
	}
	if err := c.enableLocked(); err != nil {
		return err
	}
	c.needsRecover = false
	c.log.Info("BLE adapter reset complete")
	return nil
}

func (c *Client) sendJSONLocked(payload string, hold time.Duration) error {
	serviceUUID, err := bluetooth.ParseUUID(serviceUUIDStr)
	if err != nil {
		return fmt.Errorf("parse service uuid: %w", err)
	}
	controlUUID, err := bluetooth.ParseUUID(controlUUIDStr)
	if err != nil {
		return fmt.Errorf("parse control uuid: %w", err)
	}

	// 1) Reconnect by known UUID — Apple's recommended path; works when the
	// board is back in range even if Scan no longer delivers its ads.
	if c.hasCached {
		c.log.Infof("connecting via cached address %s (skip scan)...", c.cachedAddr.String())
		if err := c.connectAndWrite(c.cachedAddr, serviceUUID, controlUUID, payload, hold); err == nil {
			return nil
		} else {
			c.log.Warnf("cached connect failed: %v; falling back to scan", err)
		}
	}

	// 2) Scan by advertisement name, then connect.
	result, err := c.scanForDeviceLocked()
	if err != nil {
		return err
	}
	c.rememberAddr(result.Address)

	return c.connectWithRetries(result.Address, serviceUUID, controlUUID, payload, hold)
}

func (c *Client) connectWithRetries(address bluetooth.Address, serviceUUID, controlUUID bluetooth.UUID, payload string, hold time.Duration) error {
	// macOS's CoreBluetooth occasionally fails a connection attempt before
	// it ever reaches the peripheral — the underlying library then has a bug
	// where it returns a zero-value Device instead of an error, which panics
	// on first use. Recover from that and retry.
	var lastErr error
	for attempt := 1; attempt <= maxConnectAttempts; attempt++ {
		if attempt > 1 {
			c.log.Infof("retrying (attempt %d/%d)...", attempt, maxConnectAttempts)
			time.Sleep(1 * time.Second)
		}
		lastErr = c.connectAndWrite(address, serviceUUID, controlUUID, payload, hold)
		if lastErr == nil {
			return nil
		}
		c.log.Warnf("attempt %d failed: %v", attempt, lastErr)
		if isAdapterStuck(lastErr) {
			break
		}
	}
	return fmt.Errorf("giving up after %d attempts: %w", maxConnectAttempts, lastErr)
}

func (c *Client) scanForDeviceLocked() (bluetooth.ScanResult, error) {
	c.log.Infof("scanning for %q (timeout %s)...", c.name, c.timeout)

	found := make(chan bluetooth.ScanResult, 1)
	done := make(chan error, 1)

	go func() {
		done <- c.adapter.Scan(func(_ *bluetooth.Adapter, result bluetooth.ScanResult) {
			if result.LocalName() != c.name {
				return
			}
			select {
			case found <- result:
			default:
			}
		})
	}()

	timer := time.NewTimer(c.timeout)
	defer timer.Stop()

	stopAndWait := func() error {
		_ = c.adapter.StopScan()
		return <-done
	}

	select {
	case result := <-found:
		// Only this locked caller stops the scan — avoids a StopScan race
		// with the library's unbuffered scanChan on macOS.
		if err := stopAndWait(); err != nil {
			return bluetooth.ScanResult{}, fmt.Errorf("scan: %w", err)
		}
		return result, nil
	case err := <-done:
		select {
		case result := <-found:
			return result, nil
		default:
		}
		if err != nil {
			return bluetooth.ScanResult{}, fmt.Errorf("scan: %w", err)
		}
		return bluetooth.ScanResult{}, fmt.Errorf("scan ended before finding %q", c.name)
	case <-timer.C:
		err := stopAndWait()
		select {
		case result := <-found:
			return result, nil
		default:
		}
		if err != nil {
			return bluetooth.ScanResult{}, fmt.Errorf("scan: %w", err)
		}
		return bluetooth.ScanResult{}, fmt.Errorf("timed out waiting for device %q", c.name)
	}
}

func (c *Client) connectAndWrite(address bluetooth.Address, serviceUUID, controlUUID bluetooth.UUID, payload string, hold time.Duration) (err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("connection attempt crashed (transient CoreBluetooth glitch): %v", r)
		}
	}()

	c.log.Infof("connecting to %s...", address.String())
	device, err := c.adapter.Connect(address, bluetooth.ConnectionParams{})
	if err != nil {
		return fmt.Errorf("connect: %w", err)
	}
	defer device.Disconnect()

	services, err := device.DiscoverServices([]bluetooth.UUID{serviceUUID})
	if err != nil {
		return fmt.Errorf("discover services: %w", err)
	}
	if len(services) == 0 {
		return errors.New("control service not found on device")
	}

	chars, err := services[0].DiscoverCharacteristics([]bluetooth.UUID{controlUUID})
	if err != nil {
		return fmt.Errorf("discover characteristics: %w", err)
	}
	if len(chars) == 0 {
		return errors.New("control characteristic not found on device")
	}

	c.log.Infof("writing: %s", payload)
	if _, err := chars[0].Write([]byte(payload)); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	c.log.Info("done")

	if hold > 0 {
		c.log.Infof("holding connection open for %s (watch the status dot)...", hold)
		time.Sleep(hold)
	}

	return nil
}

func (c *Client) rememberAddr(addr bluetooth.Address) {
	c.cachedAddr = addr
	c.hasCached = true
	if c.addrCachePath == "" {
		return
	}
	if err := os.WriteFile(c.addrCachePath, []byte(addr.String()+"\n"), 0o644); err != nil {
		c.log.Warnf("failed to persist BLE address cache: %v", err)
	}
}

func (c *Client) loadCachedAddr() {
	if c.addrCachePath == "" {
		return
	}
	data, err := os.ReadFile(c.addrCachePath)
	if err != nil {
		return
	}
	s := strings.TrimSpace(string(data))
	if s == "" {
		return
	}
	uuid, err := bluetooth.ParseUUID(s)
	if err != nil {
		c.log.Warnf("ignore invalid BLE address cache %q: %v", s, err)
		return
	}
	c.cachedAddr = bluetooth.Address{UUID: uuid}
	c.hasCached = true
	c.log.Infof("loaded cached BLE address %s", c.cachedAddr.String())
}

func isAdapterStuck(err error) bool {
	if err == nil {
		return false
	}
	msg := err.Error()
	return strings.Contains(msg, "already calling Scan") ||
		strings.Contains(msg, "not calling Scan")
}
