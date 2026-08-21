package codex

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"go.uber.org/zap"
)

const DefaultCacheTTL = 5 * time.Second

// CachedClient keeps a TTL cache of Codex limits over a live Session.
type CachedClient struct {
	log     *zap.SugaredLogger
	session *Session
	ttl     time.Duration

	mu    sync.Mutex
	state *cachedState
}

type cachedConfig struct {
	ttl         time.Duration
	sessionOpts []SessionOption
}

// CachedOption configures CachedClient.
type CachedOption func(*cachedConfig)

// WithTTL sets how long cached limits stay fresh.
func WithTTL(d time.Duration) CachedOption {
	return func(c *cachedConfig) {
		c.ttl = d
	}
}

// WithSocketPath sets the Unix socket path used by app-server.
func WithSocketPath(path string) CachedOption {
	return func(c *cachedConfig) {
		c.sessionOpts = append(c.sessionOpts, withSocketPath(path))
	}
}

// NewCachedClient builds a caching Limits client. Call Start before getters.
func NewCachedClient(log *zap.SugaredLogger, opts ...CachedOption) (*CachedClient, error) {
	if log == nil {
		log = zap.NewNop().Sugar()
	}
	log = log.Named("codex")

	cfg := cachedConfig{ttl: DefaultCacheTTL}
	for _, opt := range opts {
		opt(&cfg)
	}

	session, err := NewSession(log, cfg.sessionOpts...)
	if err != nil {
		return nil, err
	}

	c := &CachedClient{
		log:     log,
		session: session,
		ttl:     cfg.ttl,
	}

	session.SetLimitsNotify(func(result rateLimitsResult) {
		state, err := stateFromResult(result)
		if err != nil {
			c.log.Debugf("ignore rateLimits/updated: %v", err)
			return
		}
		c.mu.Lock()
		c.state = &state
		c.mu.Unlock()
		c.log.Debugf("limits updated via notify: remaining=%.1f%% resetAt=%s",
			state.remainingPercent, state.resetAt)
	})

	return c, nil
}

// Start launches app-server and connects.
func (c *CachedClient) Start(ctx context.Context) error {
	return c.session.Start(ctx)
}

// Close tears down the session.
func (c *CachedClient) Close() error {
	return c.session.Close()
}

// LimitsRemainingPercent returns remaining quota percent (0-100).
func (c *CachedClient) LimitsRemainingPercent() (float64, error) {
	state, err := c.getState()
	if err != nil {
		return 0, err
	}
	return state.remainingPercent, nil
}

// LimitsResetAt returns when the primary rate-limit window resets.
func (c *CachedClient) LimitsResetAt() (time.Time, error) {
	state, err := c.getState()
	if err != nil {
		return time.Time{}, err
	}
	return state.resetAt, nil
}

func (c *CachedClient) getState() (cachedState, error) {
	c.mu.Lock()
	if c.state != nil && time.Since(c.state.updatedAt) < c.ttl {
		state := *c.state
		c.mu.Unlock()
		return state, nil
	}
	c.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), rpcTimeout)
	defer cancel()

	result, err := c.session.ReadRateLimits(ctx)
	if err != nil {
		c.mu.Lock()
		defer c.mu.Unlock()
		if c.state == nil {
			return cachedState{}, fmt.Errorf("%w: %v", ErrNotInitialized, err)
		}
		return cachedState{}, err
	}

	state, err := stateFromResult(result)
	if err != nil {
		if c.state == nil {
			return cachedState{}, errors.Join(ErrNotInitialized, err)
		}
		return cachedState{}, err
	}

	c.mu.Lock()
	c.state = &state
	c.mu.Unlock()
	return state, nil
}
