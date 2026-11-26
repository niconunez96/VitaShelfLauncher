package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"image"
	"image/color"
	"image/png"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

const (
	verifyKey   = "probe-secret"
	verifyKeyID = "probe-kid"
)

type verifyClock struct{ now time.Time }

func (clock *verifyClock) Now() time.Time          { return clock.now }
func (clock *verifyClock) advance(d time.Duration) { clock.now = clock.now.Add(d) }

func verifyLimits() RateLimits {
	limits := DefaultRateLimits()
	limits.ChallengePerMinute = 1000
	limits.ChallengeBurst = 1000
	limits.ImagePerInstallationPerMinute = 1000
	limits.ImagePerInstallationBurst = 1000
	limits.ImagePerIPPerMinute = 1000
	limits.ImagePerIPBurst = 1000
	return limits
}

func verifyHandler(clock *verifyClock, limits RateLimits, logs io.Writer) http.Handler {
	if logs == nil {
		logs = io.Discard
	}
	return NewHandler(Config{
		Key: verifyKey, KeyID: verifyKeyID, Clock: clock.Now,
		Limits: limits, Logger: log.New(logs, "", 0),
	})
}

func verifyPNG(t *testing.T, width, height int) []byte {
	t.Helper()
	source := image.NewRGBA(image.Rect(0, 0, width, height))
	for x := 0; x < width; x++ {
		for y := 0; y < height; y++ {
			source.Set(x, y, color.RGBA{R: uint8(x), G: uint8(y), B: 90, A: 255})
		}
	}
	var contents bytes.Buffer
	if err := png.Encode(&contents, source); err != nil {
		t.Fatal(err)
	}
	return contents.Bytes()
}

func verifyRequest(path string, body io.Reader, token string) *http.Request {
	request := httptest.NewRequest(http.MethodPost, path, body)
	request.RemoteAddr = "198.51.100.7:4444"
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}
	return request
}

func verifyDo(handler http.Handler, request *http.Request) *httptest.ResponseRecorder {
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}

func verifyChallenge(t *testing.T, handler http.Handler) string {
	t.Helper()
	response := verifyDo(handler, verifyRequest("/v1/challenge", nil, ""))
	if response.Code != http.StatusOK {
		t.Fatalf("challenge status=%d body=%q", response.Code, response.Body.String())
	}
	var payload struct {
		Challenge string `json:"challenge"`
		ExpiresIn int    `json:"expires_in"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatal(err)
	}
	if payload.ExpiresIn != 60 || strings.ContainsAny(payload.Challenge, "+/=") {
		t.Fatalf("challenge payload=%+v", payload)
	}
	raw, err := base64.RawURLEncoding.DecodeString(payload.Challenge)
	if err != nil || len(raw) != 32 {
		t.Fatalf("challenge=%q decoded=%d err=%v", payload.Challenge, len(raw), err)
	}
	if response.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("challenge Cache-Control=%q", response.Header().Get("Cache-Control"))
	}
	return payload.Challenge
}

func verifyProof(key, keyID, installation, challenge string) string {
	message := "vitashelf-image-auth-v1\n" + keyID + "\n" + installation + "\n" + challenge + "\n"
	mac := hmac.New(sha256.New, []byte(key))
	_, _ = mac.Write([]byte(message))
	return base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func verifySession(handler http.Handler, installation, challenge, proof string) *httptest.ResponseRecorder {
	body, _ := json.Marshal(map[string]string{
		"key_id": verifyKeyID, "installation_id": installation,
		"challenge": challenge, "proof": proof,
	})
	return verifyDo(handler, verifyRequest("/v1/session", bytes.NewReader(body), ""))
}

func verifyHandshake(t *testing.T, handler http.Handler, installation string) string {
	t.Helper()
	challenge := verifyChallenge(t, handler)
	response := verifySession(handler, installation, challenge,
		verifyProof(verifyKey, verifyKeyID, installation, challenge))
	if response.Code != http.StatusOK {
		t.Fatalf("session status=%d body=%q", response.Code, response.Body.String())
	}
	var payload struct {
		Token     string `json:"token"`
		ExpiresIn int    `json:"expires_in"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &payload); err != nil {
		t.Fatal(err)
	}
	if payload.ExpiresIn != 3600 {
		t.Fatalf("expires_in=%d", payload.ExpiresIn)
	}
	return payload.Token
}

type verifyCountingBody struct {
	reader *bytes.Reader
	reads  int
}

func (body *verifyCountingBody) Read(buffer []byte) (int, error) {
	body.reads++
	return body.reader.Read(buffer)
}
func (body *verifyCountingBody) Close() error { return nil }

type verifyGate struct {
	started chan struct{}
	release chan struct{}
	once    sync.Once
}

func newVerifyGate() *verifyGate {
	return &verifyGate{started: make(chan struct{}), release: make(chan struct{})}
}
func (gate *verifyGate) Read([]byte) (int, error) {
	gate.once.Do(func() { close(gate.started) })
	<-gate.release
	return 0, io.EOF
}
func (gate *verifyGate) Close() error { return nil }

func TestVerifyHandshakeContractAndAuthState(t *testing.T) {
	clock := &verifyClock{now: time.Date(2026, 11, 26, 12, 0, 0, 0, time.UTC)}
	var logs bytes.Buffer
	handler := verifyHandler(clock, verifyLimits(), &logs)
	challenge := verifyChallenge(t, handler)
	proof := verifyProof(verifyKey, verifyKeyID, "verify-install", challenge)
	session := verifySession(handler, "verify-install", challenge, proof)
	if session.Code != http.StatusOK {
		t.Fatalf("session status=%d body=%q", session.Code, session.Body.String())
	}
	var payload struct {
		Token string `json:"token"`
	}
	if err := json.Unmarshal(session.Body.Bytes(), &payload); err != nil {
		t.Fatal(err)
	}
	raw, err := base64.RawURLEncoding.DecodeString(payload.Token)
	if err != nil || len(raw) != 32 {
		t.Fatalf("token=%q decoded=%d err=%v", payload.Token, len(raw), err)
	}
	imageResponse := verifyDo(handler,
		verifyRequest("/image?width=32&height=32", bytes.NewReader(verifyPNG(t, 32, 32)), payload.Token))
	if imageResponse.Code != http.StatusOK ||
		imageResponse.Header().Get("Cache-Control") != "private, max-age=86400" ||
		imageResponse.Header().Get("Content-Type") != "image/png" {
		t.Fatalf("image status=%d headers=%v", imageResponse.Code, imageResponse.Header())
	}
	for _, secret := range []string{verifyKey, proof, payload.Token} {
		if strings.Contains(logs.String(), secret) {
			t.Fatalf("logs contain %q: %q", secret, logs.String())
		}
	}
	badChallenge := verifyChallenge(t, handler)
	badProof := base64.RawURLEncoding.EncodeToString(make([]byte, sha256.Size))
	correctProofForBadChallenge := verifyProof(verifyKey, verifyKeyID, "verify-install", badChallenge)
	if response := verifySession(handler, "verify-install", badChallenge, badProof); response.Code != 401 ||
		response.Body.String() != "invalid proof\n" {
		t.Fatalf("bad proof=%d %q", response.Code, response.Body.String())
	}
	if response := verifySession(handler, "verify-install", badChallenge, correctProofForBadChallenge); response.Code != 401 ||
		response.Body.String() != "invalid or expired challenge\n" {
		t.Fatalf("replay=%d %q", response.Code, response.Body.String())
	}
	if response := verifyDo(handler, verifyRequest("/image?width=16&height=16",
		bytes.NewReader(verifyPNG(t, 16, 16)), payload.Token)); response.Code != 200 {
		t.Fatalf("rejections changed auth state: status=%d", response.Code)
	}
	if verifyHandshake(t, handler, "verify-install-2") == "" {
		t.Fatal("fresh handshake after rejection failed")
	}
}

func TestVerifyRejectionsHappenBeforeBodyRead(t *testing.T) {
	clock := &verifyClock{now: time.Date(2026, 11, 26, 12, 0, 0, 0, time.UTC)}
	handler := verifyHandler(clock, verifyLimits(), nil)
	token := verifyHandshake(t, handler, "verify-install")
	for _, bad := range []string{"", "bogus-token"} {
		body := &verifyCountingBody{reader: bytes.NewReader(verifyPNG(t, 16, 16))}
		response := verifyDo(handler, verifyRequest("/image?width=16&height=16", body, bad))
		if response.Code != 401 || response.Body.String() != "unauthorized\n" || body.reads != 0 {
			t.Fatalf("token=%q status=%d body=%q reads=%d", bad, response.Code, response.Body.String(), body.reads)
		}
	}
	clock.advance(3601 * time.Second)
	body := &verifyCountingBody{reader: bytes.NewReader(verifyPNG(t, 16, 16))}
	response := verifyDo(handler, verifyRequest("/image?width=16&height=16", body, token))
	if response.Code != 401 || body.reads != 0 {
		t.Fatalf("expired status=%d reads=%d", response.Code, body.reads)
	}
}

func TestVerifyQuotaStatusRetryAfterAndState(t *testing.T) {
	clock := &verifyClock{now: time.Date(2026, 11, 26, 12, 0, 0, 0, time.UTC)}
	limits := verifyLimits()
	limits.ImagePerInstallationPerMinute = 2
	limits.ImagePerInstallationBurst = 1
	handler := verifyHandler(clock, limits, nil)
	token := verifyHandshake(t, handler, "verify-install")
	if response := verifyDo(handler, verifyRequest("/image?width=16&height=16",
		bytes.NewReader(verifyPNG(t, 16, 16)), token)); response.Code != 200 {
		t.Fatalf("first image=%d", response.Code)
	}
	body := &verifyCountingBody{reader: bytes.NewReader(verifyPNG(t, 16, 16))}
	response := verifyDo(handler, verifyRequest("/image?width=16&height=16", body, token))
	seconds, err := strconv.Atoi(response.Header().Get("Retry-After"))
	if response.Code != 429 || response.Body.String() != "rate limit exceeded\n" || err != nil || seconds < 1 || body.reads != 0 {
		t.Fatalf("quota=%d %q retry=%q reads=%d", response.Code, response.Body.String(), response.Header().Get("Retry-After"), body.reads)
	}
	clock.advance(60 * time.Second)
	if response := verifyDo(handler, verifyRequest("/image?width=16&height=16",
		bytes.NewReader(verifyPNG(t, 16, 16)), token)); response.Code != 200 {
		t.Fatalf("token after 429=%d", response.Code)
	}

	authClock := &verifyClock{now: clock.now}
	authHandler := verifyHandler(authClock, DefaultRateLimits(), nil)
	for index := 0; index < 3; index++ {
		if response := verifyDo(authHandler, verifyRequest("/v1/challenge", nil, "")); response.Code != 200 {
			t.Fatalf("challenge %d=%d", index+1, response.Code)
		}
	}
	for _, path := range []string{"/v1/challenge", "/v1/session"} {
		response := verifyDo(authHandler, verifyRequest(path, nil, ""))
		seconds, err := strconv.Atoi(response.Header().Get("Retry-After"))
		if response.Code != 429 || err != nil || seconds < 1 {
			t.Fatalf("auth quota %s=%d retry=%q", path, response.Code, response.Header().Get("Retry-After"))
		}
	}
}

func TestVerifyQuotaDimensionsAndConcurrency(t *testing.T) {
	png := verifyPNG(t, 16, 16)
	clock := &verifyClock{now: time.Date(2026, 11, 26, 12, 0, 0, 0, time.UTC)}

	ipLimits := verifyLimits()
	ipLimits.ImagePerIPPerMinute = 2
	ipLimits.ImagePerIPBurst = 1
	ipHandler := verifyHandler(clock, ipLimits, nil)
	first := verifyHandshake(t, ipHandler, "install-a")
	second := verifyHandshake(t, ipHandler, "install-b")
	if response := verifyDo(ipHandler, verifyRequest("/image?width=16&height=16", bytes.NewReader(png), first)); response.Code != 200 {
		t.Fatalf("per-ip first=%d", response.Code)
	}
	if response := verifyDo(ipHandler, verifyRequest("/image?width=16&height=16", bytes.NewReader(png), second)); response.Code != 429 {
		t.Fatalf("per-ip second installation=%d", response.Code)
	}

	installLimits := verifyLimits()
	installLimits.ImagePerInstallationPerMinute = 2
	installLimits.ImagePerInstallationBurst = 1
	installHandler := verifyHandler(clock, installLimits, nil)
	tokenA := verifyHandshake(t, installHandler, "install-a")
	tokenB := verifyHandshake(t, installHandler, "install-b")
	if response := verifyDo(installHandler, verifyRequest("/image?width=16&height=16", bytes.NewReader(png), tokenA)); response.Code != 200 {
		t.Fatalf("per-install first=%d", response.Code)
	}
	if response := verifyDo(installHandler, verifyRequest("/image?width=16&height=16", bytes.NewReader(png), tokenA)); response.Code != 429 {
		t.Fatalf("per-install replay=%d", response.Code)
	}
	if response := verifyDo(installHandler, verifyRequest("/image?width=16&height=16", bytes.NewReader(png), tokenB)); response.Code != 200 {
		t.Fatalf("other installation=%d", response.Code)
	}

	concurrentHandler := verifyHandler(clock, verifyLimits(), nil)
	concurrentToken := verifyHandshake(t, concurrentHandler, "install-concurrent")
	gates := []*verifyGate{newVerifyGate(), newVerifyGate()}
	done := []chan struct{}{make(chan struct{}), make(chan struct{})}
	for index := range gates {
		request := verifyRequest("/image?width=16&height=16", nil, concurrentToken)
		request.Body = gates[index]
		go func(index int) {
			defer close(done[index])
			verifyDo(concurrentHandler, request)
		}(index)
		<-gates[index].started
	}
	defer func() {
		for _, gate := range gates {
			close(gate.release)
		}
		for _, channel := range done {
			<-channel
		}
	}()
	if response := verifyDo(concurrentHandler, verifyRequest("/image?width=16&height=16",
		bytes.NewReader(png), concurrentToken)); response.Code != 429 {
		t.Fatalf("third concurrent request=%d", response.Code)
	}
}

func TestVerifyServerTimeouts(t *testing.T) {
	server := newServer(":0", Config{})
	if server.ReadHeaderTimeout != 5*time.Second || server.ReadTimeout != 10*time.Second ||
		server.WriteTimeout != 60*time.Second || server.IdleTimeout != 30*time.Second ||
		server.MaxHeaderBytes != 1<<20 {
		t.Fatalf("timeouts=%+v", server)
	}
}
