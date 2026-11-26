package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"sync"
	"testing"
	"time"
)

func retryAfterSeconds(t *testing.T, response *httptest.ResponseRecorder) int {
	t.Helper()
	raw := response.Header().Get("Retry-After")
	seconds, err := strconv.Atoi(raw)
	if err != nil || seconds < 1 {
		t.Fatalf("Retry-After = %q, want a positive integer number of seconds", raw)
	}
	return seconds
}

func TestChallengeAndSessionShareThePerIPBucket(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, DefaultRateLimits(), nil)

	for i := 0; i < 3; i++ {
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/v1/challenge", nil))
		if response.Code != http.StatusOK {
			t.Fatalf("challenge %d status = %d, want 200", i+1, response.Code)
		}
	}
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/v1/challenge", nil))
	if response.Code != http.StatusTooManyRequests {
		t.Fatalf("fourth challenge status = %d, want 429", response.Code)
	}
	retryAfterSeconds(t, response)

	session := postSession(t, handler, testKeyID, "installation-1", "challenge-1", "proof")
	if session.Code != http.StatusTooManyRequests {
		t.Fatalf("session status = %d, want 429; body = %s", session.Code, session.Body.String())
	}
	retryAfterSeconds(t, session)

	clock.Advance(12 * time.Second)
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/v1/challenge", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("challenge after refill status = %d, want 200", response.Code)
	}
}

func TestImageQuotaPerInstallationRejectsBeforeDecode(t *testing.T) {
	clock := newMutableClock()
	limits := generousLimits()
	limits.ImagePerInstallationPerMinute = 2
	limits.ImagePerInstallationBurst = 1
	handler := newPublicHandler(clock.Now, limits, nil)
	token := handshake(t, handler, "installation-1")

	if response := postImage(handler, token, sourcePNG(t, 32, 32)); response.Code != http.StatusOK {
		t.Fatalf("first image status = %d, want 200", response.Code)
	}
	body := &countingBody{reader: bytes.NewReader(sourcePNGBytes(t, 32, 32))}
	response := postImage(handler, token, body)
	if response.Code != http.StatusTooManyRequests {
		t.Fatalf("second image status = %d, want 429; body = %s", response.Code, response.Body.String())
	}
	retryAfterSeconds(t, response)
	if reads := body.readCount(); reads != 0 {
		t.Fatalf("body reads = %d, want 0", reads)
	}
}

func TestImageQuotaPerIPUsesInstallationIndependentBuckets(t *testing.T) {
	clock := newMutableClock()
	limits := generousLimits()
	limits.ImagePerIPPerMinute = 2
	limits.ImagePerIPBurst = 1
	handler := newPublicHandler(clock.Now, limits, nil)

	firstToken := handshake(t, handler, "installation-1")
	secondToken := handshake(t, handler, "installation-2")
	if response := postImage(handler, firstToken, sourcePNG(t, 32, 32)); response.Code != http.StatusOK {
		t.Fatalf("first image status = %d, want 200", response.Code)
	}
	response := postImage(handler, secondToken, sourcePNG(t, 32, 32))
	if response.Code != http.StatusTooManyRequests {
		t.Fatalf("second installation status = %d, want 429; body = %s", response.Code, response.Body.String())
	}
	retryAfterSeconds(t, response)
}

type gatedBody struct {
	started chan struct{}
	release chan struct{}
	once    sync.Once
}

func newGatedBody() *gatedBody {
	return &gatedBody{started: make(chan struct{}), release: make(chan struct{})}
}

func (body *gatedBody) Read([]byte) (int, error) {
	body.once.Do(func() { close(body.started) })
	<-body.release
	return 0, io.EOF
}

func (body *gatedBody) Close() error { return nil }

func TestPublicPerInstallationConcurrencyIsBounded(t *testing.T) {
	clock := newMutableClock()
	handler := newPublicHandler(clock.Now, generousLimits(), nil)
	token := handshake(t, handler, "installation-1")

	responses := make([]*httptest.ResponseRecorder, 2)
	done := make([]chan struct{}, 2)
	bodies := make([]*gatedBody, 2)
	for index := range responses {
		bodies[index] = newGatedBody()
		responses[index] = httptest.NewRecorder()
		done[index] = make(chan struct{})
		request := httptest.NewRequest(http.MethodPost, "/image?width=64&height=64", nil)
		request.Body = bodies[index]
		request.Header.Set("Authorization", "Bearer "+token)
		go func(index int) {
			defer close(done[index])
			handler.ServeHTTP(responses[index], request)
		}(index)
		<-bodies[index].started
	}
	defer func() {
		for _, body := range bodies {
			close(body.release)
		}
		for index := range responses {
			<-done[index]
		}
	}()

	response := postImage(handler, token, sourcePNG(t, 32, 32))
	if response.Code != http.StatusTooManyRequests {
		t.Fatalf("third concurrent image status = %d, want 429", response.Code)
	}
	retryAfterSeconds(t, response)
}

func TestPublicGlobalConcurrencyIsBounded(t *testing.T) {
	clock := newMutableClock()
	limits := generousLimits()
	limits.InstallationConcurrency = 10
	handler := newPublicHandler(clock.Now, limits, nil)

	tokens := make([]string, 5)
	for index := range tokens {
		tokens[index] = handshake(t, handler, fmt.Sprintf("installation-%d", index))
	}

	responses := make([]*httptest.ResponseRecorder, 4)
	done := make([]chan struct{}, 4)
	bodies := make([]*gatedBody, 4)
	for index := range responses {
		bodies[index] = newGatedBody()
		responses[index] = httptest.NewRecorder()
		done[index] = make(chan struct{})
		request := httptest.NewRequest(http.MethodPost, "/image?width=64&height=64", nil)
		request.Body = bodies[index]
		request.Header.Set("Authorization", "Bearer "+tokens[index])
		go func(index int) {
			defer close(done[index])
			handler.ServeHTTP(responses[index], request)
		}(index)
		<-bodies[index].started
	}
	defer func() {
		for _, body := range bodies {
			close(body.release)
		}
		for index := range responses {
			<-done[index]
		}
	}()

	response := postImage(handler, tokens[4], sourcePNG(t, 32, 32))
	if response.Code != http.StatusTooManyRequests {
		t.Fatalf("fifth concurrent image status = %d, want 429", response.Code)
	}
	retryAfterSeconds(t, response)
}
