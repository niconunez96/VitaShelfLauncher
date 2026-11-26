package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"net/http"
	"net/http/httptest"
	"sort"
	"strings"
	"testing"
)

const baselineLocalParityDigest = "06279c5e8139f7c8fb1f9293eee52470e7c2dd46b0586612311f62586b8b9acf"

func paritySourcePNG(t *testing.T) []byte {
	t.Helper()
	source := image.NewRGBA(image.Rect(0, 0, 24, 18))
	for x := 0; x < 24; x++ {
		for y := 0; y < 18; y++ {
			source.Set(x, y, color.RGBA{R: uint8(x * 7), G: uint8(y * 9), B: 120, A: 255})
		}
	}
	var contents bytes.Buffer
	if err := png.Encode(&contents, source); err != nil {
		t.Fatal(err)
	}
	return contents.Bytes()
}

func parityRecord(handler http.Handler, method, target string, body []byte) string {
	request := httptest.NewRequest(method, target, bytes.NewReader(body))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	keys := make([]string, 0, len(response.Header()))
	for key := range response.Header() {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var out strings.Builder
	fmt.Fprintf(&out, "%s %s -> %d\n", method, target, response.Code)
	for _, key := range keys {
		fmt.Fprintf(&out, "%s: %s\n", key, strings.Join(response.Header().Values(key), ","))
	}
	fmt.Fprintf(&out, "body[%d]: %x\n", response.Body.Len(), response.Body.Bytes())
	return out.String()
}

func TestVerifyLocalModeParityDigest(t *testing.T) {
	handler := NewHandler(Config{})
	png := paritySourcePNG(t)
	cases := []struct {
		method, target string
		body           []byte
	}{
		{http.MethodGet, "/", nil},
		{http.MethodGet, "/health", nil},
		{http.MethodGet, "/v1/challenge", nil},
		{http.MethodPost, "/notfound", nil},
		{http.MethodPost, "/image?width=16&height=16", png},
		{http.MethodPost, "/image?width=16&height=16&mode=contain", png},
		{http.MethodPost, "/image?width=0&height=16", png},
		{http.MethodPost, "/image?width=16&height=16&mode=stretch", png},
		{http.MethodPost, "/image?width=16&height=16&trim=bad", png},
		{http.MethodPost, "/image?width=16&height=16", []byte("not a png")},
		{http.MethodGet, "/image?width=16&height=16", nil},
	}
	var all strings.Builder
	for _, test := range cases {
		all.WriteString(parityRecord(handler, test.method, test.target, test.body))
	}
	digest := sha256.Sum256([]byte(all.String()))
	got := hex.EncodeToString(digest[:])
	if got != baselineLocalParityDigest {
		t.Fatalf("local-mode parity digest changed: got %s want %s", got, baselineLocalParityDigest)
	}
}
