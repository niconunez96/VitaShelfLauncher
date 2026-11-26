package main

import (
	"math"
	"net"
	"net/http"
	"sort"
	"strconv"
	"sync"
	"time"
)

const (
	defaultChallengePerMinute        = 5
	defaultChallengeBurst            = 3
	defaultImagePerInstallationMin   = 60
	defaultImagePerInstallationBurst = 10
	defaultImagePerIPPerMinute       = 300
	defaultImagePerIPBurst           = 50
	defaultInstallationConcurrency   = 2
	defaultGlobalConcurrency         = 4

	rateCleanupInterval = time.Minute
	rateBucketTTL       = 2 * time.Minute
	maxRateBuckets      = 8192
)

// RateLimits are token-bucket allowances per minute plus their initial bursts.
type RateLimits struct {
	ChallengePerMinute            int
	ChallengeBurst                int
	ImagePerInstallationPerMinute int
	ImagePerInstallationBurst     int
	ImagePerIPPerMinute           int
	ImagePerIPBurst               int
	InstallationConcurrency       int
	GlobalConcurrency             int
}

// DefaultRateLimits returns the protocol v1 public-mode quotas.
func DefaultRateLimits() RateLimits {
	return RateLimits{
		ChallengePerMinute:            defaultChallengePerMinute,
		ChallengeBurst:                defaultChallengeBurst,
		ImagePerInstallationPerMinute: defaultImagePerInstallationMin,
		ImagePerInstallationBurst:     defaultImagePerInstallationBurst,
		ImagePerIPPerMinute:           defaultImagePerIPPerMinute,
		ImagePerIPBurst:               defaultImagePerIPBurst,
		InstallationConcurrency:       defaultInstallationConcurrency,
		GlobalConcurrency:             defaultGlobalConcurrency,
	}
}

func normalizeRateLimits(limits RateLimits) RateLimits {
	defaults := DefaultRateLimits()
	if limits.ChallengePerMinute <= 0 {
		limits.ChallengePerMinute = defaults.ChallengePerMinute
	}
	if limits.ChallengeBurst <= 0 {
		limits.ChallengeBurst = defaults.ChallengeBurst
	}
	if limits.ImagePerInstallationPerMinute <= 0 {
		limits.ImagePerInstallationPerMinute = defaults.ImagePerInstallationPerMinute
	}
	if limits.ImagePerInstallationBurst <= 0 {
		limits.ImagePerInstallationBurst = defaults.ImagePerInstallationBurst
	}
	if limits.ImagePerIPPerMinute <= 0 {
		limits.ImagePerIPPerMinute = defaults.ImagePerIPPerMinute
	}
	if limits.ImagePerIPBurst <= 0 {
		limits.ImagePerIPBurst = defaults.ImagePerIPBurst
	}
	if limits.InstallationConcurrency <= 0 {
		limits.InstallationConcurrency = defaults.InstallationConcurrency
	}
	if limits.GlobalConcurrency <= 0 {
		limits.GlobalConcurrency = defaults.GlobalConcurrency
	}
	return limits
}

type rateBucket struct {
	tokens      float64
	refreshedAt time.Time
	seenAt      time.Time
}

type requestLimiter struct {
	mu          sync.Mutex
	now         func() time.Time
	limits      RateLimits
	buckets     map[string]*rateBucket
	active      map[string]int
	global      int
	lastCleanup time.Time
}

func newRequestLimiter(limits RateLimits, clock func() time.Time) *requestLimiter {
	if clock == nil {
		clock = time.Now
	}
	return &requestLimiter{
		now:         clock,
		limits:      normalizeRateLimits(limits),
		buckets:     make(map[string]*rateBucket),
		active:      make(map[string]int),
		lastCleanup: clock(),
	}
}

func (limiter *requestLimiter) allow(key string, perMinute, burst int) (bool, time.Duration) {
	now := limiter.now()
	limiter.mu.Lock()
	defer limiter.mu.Unlock()
	limiter.cleanupLocked(now)

	bucket, present := limiter.buckets[key]
	if !present {
		bucket = &rateBucket{tokens: float64(burst), refreshedAt: now}
		limiter.buckets[key] = bucket
	} else if elapsed := now.Sub(bucket.refreshedAt); elapsed > 0 {
		bucket.tokens = math.Min(float64(burst), bucket.tokens+elapsed.Seconds()*float64(perMinute)/60)
		bucket.refreshedAt = now
	}
	bucket.seenAt = now
	if bucket.tokens >= 1 {
		bucket.tokens--
		return true, 0
	}
	retryAfter := (1 - bucket.tokens) * 60 / float64(perMinute)
	return false, time.Duration(math.Ceil(retryAfter) * float64(time.Second))
}

func (limiter *requestLimiter) allowAuth(ip string) (bool, time.Duration) {
	return limiter.allow("auth:"+ip, limiter.limits.ChallengePerMinute, limiter.limits.ChallengeBurst)
}

func (limiter *requestLimiter) allowImage(installationID, ip string) (bool, time.Duration) {
	if allowed, retryAfter := limiter.allow("image-installation:"+installationID,
		limiter.limits.ImagePerInstallationPerMinute, limiter.limits.ImagePerInstallationBurst); !allowed {
		return false, retryAfter
	}
	return limiter.allow("image-ip:"+ip, limiter.limits.ImagePerIPPerMinute, limiter.limits.ImagePerIPBurst)
}

func (limiter *requestLimiter) acquire(installationID string) (func(), bool) {
	limiter.mu.Lock()
	if limiter.global >= limiter.limits.GlobalConcurrency {
		limiter.mu.Unlock()
		return nil, false
	}
	active := limiter.active[installationID]
	if active >= limiter.limits.InstallationConcurrency {
		limiter.mu.Unlock()
		return nil, false
	}
	limiter.global++
	limiter.active[installationID] = active + 1
	limiter.mu.Unlock()

	var once sync.Once
	return func() {
		once.Do(func() {
			limiter.mu.Lock()
			defer limiter.mu.Unlock()
			limiter.global--
			if remaining := limiter.active[installationID] - 1; remaining > 0 {
				limiter.active[installationID] = remaining
			} else {
				delete(limiter.active, installationID)
			}
		})
	}, true
}

func (limiter *requestLimiter) cleanupLocked(now time.Time) {
	if !now.Before(limiter.lastCleanup) && now.Sub(limiter.lastCleanup) < rateCleanupInterval {
		return
	}
	for key, bucket := range limiter.buckets {
		if !now.Before(bucket.seenAt) && now.Sub(bucket.seenAt) > rateBucketTTL {
			delete(limiter.buckets, key)
		}
	}
	if len(limiter.buckets) > maxRateBuckets {
		type staleBucket struct {
			key  string
			seen time.Time
		}
		oldest := make([]staleBucket, 0, len(limiter.buckets))
		for key, bucket := range limiter.buckets {
			oldest = append(oldest, staleBucket{key: key, seen: bucket.seenAt})
		}
		sort.Slice(oldest, func(i, j int) bool { return oldest[i].seen.Before(oldest[j].seen) })
		for index := 0; index < len(oldest)-maxRateBuckets; index++ {
			delete(limiter.buckets, oldest[index].key)
		}
	}
	limiter.lastCleanup = now
}

func clientIP(request *http.Request) string {
	host, _, err := net.SplitHostPort(request.RemoteAddr)
	if err == nil {
		return host
	}
	return request.RemoteAddr
}

func writeRateLimit(writer http.ResponseWriter, retryAfter time.Duration) {
	seconds := int64(math.Ceil(retryAfter.Seconds()))
	if seconds < 1 {
		seconds = 1
	}
	writer.Header().Set("Retry-After", strconv.FormatInt(seconds, 10))
	http.Error(writer, "rate limit exceeded", http.StatusTooManyRequests)
}
