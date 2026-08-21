package codex

import (
	"encoding/json"
	"fmt"
	"time"
)

// Limits is the public read API for Codex rate-limit state.
type Limits interface {
	LimitsRemainingPercent() (float64, error)
	LimitsResetAt() (time.Time, error)
}

type rpcMessage struct {
	ID     *json.Number    `json:"id,omitempty"`
	Method string          `json:"method,omitempty"`
	Params json.RawMessage `json:"params,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  json.RawMessage `json:"error,omitempty"`
}

type rateWindow struct {
	UsedPercent       float64 `json:"usedPercent"`
	WindowDurationMin int64   `json:"windowDurationMins"`
	ResetsAt          int64   `json:"resetsAt"`
}

type rateLimit struct {
	LimitID   string      `json:"limitId"`
	LimitName *string     `json:"limitName"`
	Primary   *rateWindow `json:"primary"`
	Secondary *rateWindow `json:"secondary"`
}

type rateLimitsResult struct {
	RateLimits          rateLimit            `json:"rateLimits"`
	RateLimitsByLimitID map[string]rateLimit `json:"rateLimitsByLimitId"`
}

type cachedState struct {
	remainingPercent float64
	resetAt          time.Time
	updatedAt        time.Time
}

func primaryWindow(result rateLimitsResult) (*rateWindow, bool) {
	if result.RateLimits.Primary != nil {
		return result.RateLimits.Primary, true
	}
	for _, limit := range result.RateLimitsByLimitID {
		if limit.Primary != nil {
			return limit.Primary, true
		}
	}
	return nil, false
}

func stateFromResult(result rateLimitsResult) (cachedState, error) {
	window, ok := primaryWindow(result)
	if !ok {
		return cachedState{}, fmt.Errorf("no primary rate limit window in response")
	}
	if window.ResetsAt <= 0 {
		return cachedState{}, fmt.Errorf("no reset date in rate limit response")
	}
	return cachedState{
		remainingPercent: 100 - window.UsedPercent,
		resetAt:          time.Unix(window.ResetsAt, 0),
		updatedAt:        time.Now(),
	}, nil
}
