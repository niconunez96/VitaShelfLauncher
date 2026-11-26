package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	testKey   = "test-secret-key"
	testKeyID = "test-key-id"
)

type mutableClock struct {
	mu  sync.Mutex
	now time.Time
}

func newMutableClock() *mutableClock {
	return &mutableClock{now: time.Date(2026, time.November, 26, 12, 0, 0, 0, time.UTC)}
}

func (clock *mutableClock) Now() time.Time {
	clock.mu.Lock()
	defer clock.mu.Unlock()
	return clock.now
}

func (clock *mutableClock) Advance(duration time.Duration) {
	clock.mu.Lock()
	defer clock.mu.Unlock()
	clock.now = clock.now.Add(duration)
}

func newPublicHandler(clock func() time.Time, limits RateLimits, logs io.Writer) http.Handler {
	if logs == nil {
		logs = io.Discard
	}
	return NewHandler(Config{
		Key:    testKey,
		KeyID:  testKeyID,
		Clock:  clock,
		Limits: limits,
		Logger: log.New(logs, "", 0),
	})
}

func generousLimits() RateLimits {
	limits := DefaultRateLimits()
	limits.ChallengePerMinute = 1000
	limits.ChallengeBurst = 1000
	limits.ImagePerInstallationPerMinute = 1000
	limits.ImagePerInstallationBurst = 1000
	limits.ImagePerIPPerMinute = 1000
	limits.ImagePerIPBurst = 1000
	return limits
}

func sourcePNGBytes(t *testing.T, width, height int) []byte {
	t.Helper()
	contents, err := io.ReadAll(sourcePNG(t, width, height))
	if err != nil {
		t.Fatalf("read source PNG: %v", err)
	}
	return contents
}

func proofFor(key, keyID, installationID, challenge string) string {
	message := "vitashelf-image-auth-v1\n" + keyID + "\n" + installationID + "\n" + challenge + "\n"
	mac := hmac.New(sha256.New, []byte(key))
	_, _ = mac.Write([]byte(message))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func issueChallenge(t *testing.T, handler http.Handler) string {
	t.Helper()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/v1/challenge", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("challenge status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	var payload struct {
		Challenge string `json:"challenge"`
		ExpiresIn int    `json:"expires_in"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode challenge response: %v", err)
	}
	if payload.ExpiresIn != 60 {
		t.Fatalf("expires_in = %d, want 60", payload.ExpiresIn)
	}
	return payload.Challenge
}

func postSession(t *testing.T, handler http.Handler, keyID, installationID, challenge, proof string) *httptest.ResponseRecorder {
	t.Helper()
	body, err := json.Marshal(map[string]string{
		"key_id":          keyID,
		"installation_id": installationID,
		"challenge":       challenge,
		"proof":           proof,
	})
	if err != nil {
		t.Fatalf("encode session request: %v", err)
	}
	request := httptest.NewRequest(http.MethodPost, "/v1/session", bytes.NewReader(body))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func handshake(t *testing.T, handler http.Handler, installationID string) string {
	t.Helper()
	challenge := issueChallenge(t, handler)
	response := postSession(t, handler, testKeyID, installationID, challenge, proofFor(testKey, testKeyID, installationID, challenge))
	if response.Code != http.StatusOK {
		t.Fatalf("session status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	var payload struct {
		Token     string `json:"token"`
		ExpiresIn int    `json:"expires_in"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode session response: %v", err)
	}
	if payload.ExpiresIn != 3600 {
		t.Fatalf("expires_in = %d, want 3600", payload.ExpiresIn)
	}
	return payload.Token
}

func postImage(handler http.Handler, token string, body io.Reader) *httptest.ResponseRecorder {
	request := httptest.NewRequest(http.MethodPost, "/image?width=64&height=64", body)
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func TestChallengeUsesBase64URLRandomBytes(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, DefaultRateLimits(), nil)

	first := issueChallenge(t, handler)
	second := issueChallenge(t, handler)
	if first == second {
		t.Fatal("challenge values must be random")
	}
	for _, value := range []string{first, second} {
		decoded, err := base64.RawURLEncoding.DecodeString(value)
		if err != nil {
			t.Fatalf("challenge %q is not base64url without padding: %v", value, err)
		}
		if len(decoded) != 32 {
			t.Fatalf("challenge entropy = %d bytes, want 32", len(decoded))
		}
		if strings.ContainsAny(value, "+/=") {
			t.Fatalf("challenge %q contains non-base64url characters", value)
		}
	}
}

func TestValidHandshakeProducesUsablePrivateSession(t *testing.T) {
	clock := newMutableClock()
	var logs bytes.Buffer
	handler := newPublicHandler(clock.Now, DefaultRateLimits(), &logs)

	token := handshake(t, handler, "installation-1")
	if token == "" {
		t.Fatal("session token is empty")
	}
	decoded, err := base64.RawURLEncoding.DecodeString(token)
	if err != nil || len(decoded) != 32 {
		t.Fatalf("token = %q, decoded = %d bytes, err = %v; want 32 base64url bytes", token, len(decoded), err)
	}

	response := postImage(handler, token, sourcePNG(t, 32, 32))
	if response.Code != http.StatusOK {
		t.Fatalf("image status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	if got := response.Header().Get("Cache-Control"); got != "private, max-age=86400" {
		t.Fatalf("Cache-Control = %q, want private, max-age=86400", got)
	}
	if strings.Contains(logs.String(), token) {
		t.Fatalf("logs contain the session token: %q", logs.String())
	}
}

func TestInvalidProofIsRejectedAndConsumesChallenge(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	challenge := issueChallenge(t, handler)

	wrongProof := base64.RawURLEncoding.EncodeToString(make([]byte, sha256.Size))
	if response := postSession(t, handler, testKeyID, "installation-1", challenge, wrongProof); response.Code != http.StatusUnauthorized {
		t.Fatalf("invalid proof status = %d, want 401", response.Code)
	}
	correctProof := proofFor(testKey, testKeyID, "installation-1", challenge)
	if response := postSession(t, handler, testKeyID, "installation-1", challenge, correctProof); response.Code != http.StatusUnauthorized {
		t.Fatalf("replayed challenge status = %d, want 401", response.Code)
	}
}

func TestTamperedChallengeIsRejected(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	challenge := issueChallenge(t, handler)
	tampered := challenge[:len(challenge)-1] + "A"
	if tampered == challenge {
		tampered = challenge[:len(challenge)-1] + "B"
	}

	proof := proofFor(testKey, testKeyID, "installation-1", tampered)
	if response := postSession(t, handler, testKeyID, "installation-1", tampered, proof); response.Code != http.StatusUnauthorized {
		t.Fatalf("tampered challenge status = %d, want 401", response.Code)
	}
}

func TestChallengeReplayIsRejected(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	challenge := issueChallenge(t, handler)
	proof := proofFor(testKey, testKeyID, "installation-1", challenge)

	if response := postSession(t, handler, testKeyID, "installation-1", challenge, proof); response.Code != http.StatusOK {
		t.Fatalf("first session status = %d, want 200", response.Code)
	}
	if response := postSession(t, handler, testKeyID, "installation-1", challenge, proof); response.Code != http.StatusUnauthorized {
		t.Fatalf("replayed challenge status = %d, want 401", response.Code)
	}
}

func TestExpiredChallengeIsRejected(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	challenge := issueChallenge(t, handler)
	proof := proofFor(testKey, testKeyID, "installation-1", challenge)

	clock.Advance(61 * time.Second)
	response := postSession(t, handler, testKeyID, "installation-1", challenge, proof)
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("expired challenge status = %d, want 401", response.Code)
	}
}

func TestSessionTokenIsRequiredValidAndTimeBounded(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	token := handshake(t, handler, "installation-1")

	t.Run("missing", func(t *testing.T) {
		body := &countingBody{reader: bytes.NewReader(sourcePNGBytes(t, 32, 32))}
		response := postImage(handler, "", body)
		if response.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want 401", response.Code)
		}
		if reads := body.readCount(); reads != 0 {
			t.Fatalf("body reads = %d, want 0", reads)
		}
	})
	t.Run("invalid", func(t *testing.T) {
		body := &countingBody{reader: bytes.NewReader(sourcePNGBytes(t, 32, 32))}
		response := postImage(handler, "not-a-real-token", body)
		if response.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want 401", response.Code)
		}
		if reads := body.readCount(); reads != 0 {
			t.Fatalf("body reads = %d, want 0", reads)
		}
	})
	t.Run("expired", func(t *testing.T) {
		clock.Advance(3601 * time.Second)
		response := postImage(handler, token, sourcePNG(t, 32, 32))
		if response.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want 401", response.Code)
		}
	})
}

func TestKeyIDMismatchIsRejected(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	challenge := issueChallenge(t, handler)

	response := postSession(t, handler, "wrong-key-id", "installation-1", challenge, proofFor(testKey, "wrong-key-id", "installation-1", challenge))
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("key ID mismatch status = %d, want 401", response.Code)
	}
}

func TestMalformedSessionRequestsAreBadRequests(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)

	t.Run("invalid json", func(t *testing.T) {
		request := httptest.NewRequest(http.MethodPost, "/v1/session", strings.NewReader("{"))
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want 400", response.Code)
		}
	})
	t.Run("invalid alphabet", func(t *testing.T) {
		response := postSession(t, handler, "bad/key/id", "installation-1", "challenge-1", proofFor(testKey, "bad/key/id", "installation-1", "challenge-1"))
		if response.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want 400", response.Code)
		}
	})
	t.Run("oversized id", func(t *testing.T) {
		response := postSession(t, handler, testKeyID, strings.Repeat("a", 129), "challenge-1", "proof")
		if response.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want 400", response.Code)
		}
	})
	t.Run("missing proof", func(t *testing.T) {
		body, err := json.Marshal(map[string]string{"key_id": testKeyID, "installation_id": "installation-1", "challenge": "challenge-1"})
		if err != nil {
			t.Fatal(err)
		}
		request := httptest.NewRequest(http.MethodPost, "/v1/session", bytes.NewReader(body))
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusBadRequest {
			t.Fatalf("status = %d, want 400", response.Code)
		}
	})
}

func TestAuthModeRequiresBothCredentials(t *testing.T) {
	if enabled, err := resolveAuthMode("", ""); err != nil || enabled {
		t.Fatalf("local mode = %v, err = %v; want false, nil", enabled, err)
	}
	if enabled, err := resolveAuthMode(testKey, testKeyID); err != nil || !enabled {
		t.Fatalf("public mode = %v, err = %v; want true, nil", enabled, err)
	}
	for _, pair := range [][2]string{{testKey, ""}, {"", testKeyID}} {
		if _, err := resolveAuthMode(pair[0], pair[1]); err == nil {
			t.Fatalf("resolveAuthMode(%q, %q) should fail", pair[0], pair[1])
		} else if !strings.Contains(err.Error(), "IMAGE_PROXY_KEY") || !strings.Contains(err.Error(), "IMAGE_PROXY_KEY_ID") {
			t.Fatalf("error = %q, want both variable names", err.Error())
		}
	}
}

type countingBody struct {
	reader *bytes.Reader
	reads  int64
}

func (body *countingBody) Read(buffer []byte) (int, error) {
	body.reads++
	return body.reader.Read(buffer)
}

func (body *countingBody) Close() error { return nil }

func (body *countingBody) readCount() int64 { return body.reads }
