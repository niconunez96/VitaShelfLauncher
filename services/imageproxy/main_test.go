package main

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"log"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestImageReturnsCenterCroppedPNG(t *testing.T) {
	t.Parallel()

	handler := NewHandler(Config{})
	request := httptest.NewRequest(http.MethodPost, "/image?width=100&height=100", sourcePNG(t, 400, 200))
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	if contentType := response.Header().Get("Content-Type"); contentType != "image/png" {
		t.Fatalf("Content-Type = %q, want image/png", contentType)
	}
	decoded, err := png.Decode(response.Body)
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if bounds := decoded.Bounds(); bounds.Dx() != 100 || bounds.Dy() != 100 {
		t.Fatalf("dimensions = %dx%d, want 100x100", bounds.Dx(), bounds.Dy())
	}
}

func TestContainPreservesTheEntireCover(t *testing.T) {
	t.Parallel()
	for _, test := range []struct {
		name                                 string
		width, height, wantWidth, wantHeight int
	}{
		{"square", 200, 200, 256, 256},
		{"portrait", 200, 400, 160, 320},
		{"landscape", 400, 200, 256, 128},
	} {
		t.Run(test.name, func(t *testing.T) {
			request := httptest.NewRequest(http.MethodPost, "/image?width=256&height=320&mode=contain", sourcePNG(t, test.width, test.height))
			response := httptest.NewRecorder()
			NewHandler(Config{}).ServeHTTP(response, request)
			if response.Code != http.StatusOK {
				t.Fatalf("status = %d", response.Code)
			}
			result, err := png.Decode(response.Body)
			if err != nil {
				t.Fatal(err)
			}
			if result.Bounds().Dx() != test.wantWidth || result.Bounds().Dy() != test.wantHeight {
				t.Fatalf("bounds = %v, want %dx%d", result.Bounds(), test.wantWidth, test.wantHeight)
			}
			// Blue marks every source pixel, including both previously cropped edges.
			for _, p := range []image.Point{{0, 0}, {test.wantWidth - 1, test.wantHeight - 1}} {
				pixel := color.NRGBAModel.Convert(result.At(p.X, p.Y)).(color.NRGBA)
				if pixel.B != 100 || pixel.A != 255 {
					t.Fatalf("source edge lost at %v: %v", p, pixel)
				}
			}
		})
	}
}

func TestImageRejectsUnknownFitMode(t *testing.T) {
	request := httptest.NewRequest(http.MethodPost, "/image?width=100&height=100&mode=stretch", nil)
	response := httptest.NewRecorder()
	NewHandler(Config{}).ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status = %d", response.Code)
	}
}

func TestImageRejectsNonPOSTRequests(t *testing.T) {
	t.Parallel()

	handler := NewHandler(Config{})
	request := httptest.NewRequest(http.MethodGet, "/image?width=100&height=100", nil)
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusMethodNotAllowed)
	}
}

func TestHealthReturnsOK(t *testing.T) {
	t.Parallel()

	handler := NewHandler(Config{})
	request := httptest.NewRequest(http.MethodGet, "/health", nil)
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusOK)
	}
	if response.Body.String() != "ok\n" {
		t.Fatalf("body = %q, want %q", response.Body.String(), "ok\n")
	}
}

func TestRootReturnsServiceGreeting(t *testing.T) {
	t.Parallel()

	handler := NewHandler(Config{})
	request := httptest.NewRequest(http.MethodGet, "/", nil)
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusOK)
	}
	if response.Body.String() != "imageproxy is running\n" {
		t.Fatalf("body = %q, want %q", response.Body.String(), "imageproxy is running\n")
	}
}

func TestImageLogsSuccessfulProcessing(t *testing.T) {
	var logs bytes.Buffer
	handler := NewHandler(Config{Logger: log.New(&logs, "", 0)})
	request := httptest.NewRequest(http.MethodPost, "/image?width=100&height=120", sourcePNG(t, 400, 200))
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d", response.Code, http.StatusOK)
	}
	if !strings.Contains(logs.String(), "processed image: source=400x200 output=100x120") {
		t.Fatalf("log = %q, want successful processing details", logs.String())
	}
}

func TestImageDownscaleUsesQualityResampling(t *testing.T) {
	t.Parallel()

	handler := NewHandler(Config{})
	request := httptest.NewRequest(http.MethodPost, "/image?width=100&height=100", sharpEdgePNG(t, 400, 200))
	response := httptest.NewRecorder()

	handler.ServeHTTP(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	decoded, err := png.Decode(response.Body)
	if err != nil {
		t.Fatalf("decode response: %v", err)
	}
	// Center crop of 400x200 to 100x100 is x=[100,300), so a red/green edge at source x=200
	// lands at output x=50. Nearest-neighbor keeps that boundary hard; a quality scaler blends it.
	boundary := color.NRGBAModel.Convert(decoded.At(49, 50)).(color.NRGBA)
	if boundary.G == 0 || boundary.R == 255 {
		t.Fatalf("expected blended edge at (49,50), got RGBA(%d,%d,%d,%d)", boundary.R, boundary.G, boundary.B, boundary.A)
	}
}

func TestLocalModeKeepsUnauthenticatedBehavior(t *testing.T) {
	handler := NewHandler(Config{})
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/image?width=64&height=64", sourcePNG(t, 32, 32)))
	if response.Code != http.StatusOK {
		t.Fatalf("local image status = %d, want %d; body = %s", response.Code, http.StatusOK, response.Body.String())
	}
	if got := response.Header().Get("Cache-Control"); got != "public, max-age=86400" {
		t.Fatalf("Cache-Control = %q, want public, max-age=86400", got)
	}
	for _, path := range []string{"/v1/challenge", "/v1/session"} {
		response = httptest.NewRecorder()
		handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, path, nil))
		if response.Code != http.StatusNotFound {
			t.Fatalf("local %s status = %d, want 404", path, response.Code)
		}
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/health", nil))
	if response.Code != http.StatusOK || response.Body.String() != "ok\n" {
		t.Fatalf("local health = %d %q, want 200 ok", response.Code, response.Body.String())
	}
}

func TestServerUsesHardenedTimeouts(t *testing.T) {
	server := newServer(":8090", Config{})
	if server.Addr != ":8090" {
		t.Fatalf("Addr = %q, want :8090", server.Addr)
	}
	if server.ReadHeaderTimeout != 5*time.Second || server.ReadTimeout != 10*time.Second ||
		server.WriteTimeout != 60*time.Second || server.IdleTimeout != 30*time.Second ||
		server.MaxHeaderBytes != 1<<20 {
		t.Fatalf("unexpected server limits: header=%s read=%s write=%s idle=%s maxHeader=%d",
			server.ReadHeaderTimeout, server.ReadTimeout, server.WriteTimeout, server.IdleTimeout, server.MaxHeaderBytes)
	}
	if server.Handler == nil {
		t.Fatal("server.Handler is nil")
	}
}

func sourcePNG(t *testing.T, width, height int) *bytes.Reader {
	t.Helper()
	source := image.NewRGBA(image.Rect(0, 0, width, height))
	for x := 0; x < width; x++ {
		for y := 0; y < height; y++ {
			source.Set(x, y, color.RGBA{R: uint8(x), G: uint8(y), B: 100, A: 255})
		}
	}
	var contents bytes.Buffer
	err := png.Encode(&contents, source)
	if err != nil {
		t.Fatalf("encode source PNG: %v", err)
	}
	return bytes.NewReader(contents.Bytes())
}

func sharpEdgePNG(t *testing.T, width, height int) *bytes.Reader {
	t.Helper()
	source := image.NewRGBA(image.Rect(0, 0, width, height))
	mid := width / 2
	for x := 0; x < width; x++ {
		for y := 0; y < height; y++ {
			if x < mid {
				source.Set(x, y, color.RGBA{R: 255, A: 255})
			} else {
				source.Set(x, y, color.RGBA{G: 255, A: 255})
			}
		}
	}
	var contents bytes.Buffer
	err := png.Encode(&contents, source)
	if err != nil {
		t.Fatalf("encode source PNG: %v", err)
	}
	return bytes.NewReader(contents.Bytes())
}
