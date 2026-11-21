package main

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestAlphaTrimBeforeResize(t *testing.T) {
	source := image.NewNRGBA(image.Rect(0, 0, 12, 10))
	for y := 3; y < 5; y++ {
		for x := 4; x < 8; x++ {
			source.SetNRGBA(x, y, color.NRGBA{R: 60, G: 120, B: 180, A: 192})
		}
	}
	source.SetNRGBA(4, 3, color.NRGBA{R: 60, G: 120, B: 180, A: 1})
	for _, test := range []struct {
		name, query   string
		width, height int
	}{
		{"natural size and soft edges", "width=4&height=4&mode=contain&trim=alpha", 4, 2},
		{"upscale", "width=8&height=8&mode=contain&trim=alpha", 8, 4},
		{"cover still crops", "width=4&height=4&mode=cover&trim=alpha", 4, 4},
		{"default still covers", "width=4&height=4&trim=alpha", 4, 4},
		{"omitted trim preserves canvas", "width=12&height=12&mode=contain", 12, 10},
	} {
		t.Run(test.name, func(t *testing.T) {
			response := imageResponse(t, source, test.query)
			if response.Code != http.StatusOK {
				t.Fatalf("status = %d; body = %s", response.Code, response.Body.String())
			}
			if response.Header().Get("Content-Type") != "image/png" {
				t.Fatalf("Content-Type = %q", response.Header().Get("Content-Type"))
			}
			result, err := png.Decode(response.Body)
			if err != nil {
				t.Fatal(err)
			}
			if result.Bounds().Dx() != test.width || result.Bounds().Dy() != test.height {
				t.Fatalf("bounds = %v, want %dx%d", result.Bounds(), test.width, test.height)
			}
			if test.name == "natural size and soft edges" {
				for y := 0; y < 2; y++ {
					for x := 0; x < 4; x++ {
						got := color.NRGBAModel.Convert(result.At(x, y)).(color.NRGBA)
						want := source.NRGBAAt(x+4, y+3)
						if got.A != want.A {
							t.Fatalf("pixel (%d,%d) = %v, want %v", x, y, got, want)
						}
						// The existing resampler rounds premultiplied color channels.
						for _, channel := range [][2]uint8{{got.R, want.R}, {got.G, want.G}, {got.B, want.B}} {
							if delta := int(channel[0]) - int(channel[1]); delta < -1 || delta > 1 {
								t.Fatalf("color changed at (%d,%d): %v, want %v", x, y, got, want)
							}
						}
					}
				}
			}
		})
	}
}

func TestAlphaTrimNoOpForOpaqueAndEmptyImages(t *testing.T) {
	for _, alpha := range []uint8{0, 255} {
		source := image.NewNRGBA(image.Rect(0, 0, 6, 4))
		for y := 0; y < 4; y++ {
			for x := 0; x < 6; x++ {
				source.SetNRGBA(x, y, color.NRGBA{R: 100, A: alpha})
			}
		}
		for _, mode := range []string{"cover", "contain"} {
			query := "width=9&height=9&mode=" + mode
			before := imageResponse(t, source, query)
			after := imageResponse(t, source, query+"&trim=alpha")
			if before.Code != http.StatusOK || after.Code != http.StatusOK || !bytes.Equal(before.Body.Bytes(), after.Body.Bytes()) {
				t.Fatalf("trim changed an opaque/empty source: alpha=%d mode=%s", alpha, mode)
			}
		}
	}
}

func TestImageRejectsUnknownTrim(t *testing.T) {
	request := httptest.NewRequest(http.MethodPost, "/image?width=4&height=4&trim=white", nil)
	response := httptest.NewRecorder()
	NewHandler(Config{}).ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest || response.Body.String() != "trim must be alpha or empty\n" {
		t.Fatalf("response = %d %q", response.Code, response.Body.String())
	}
}

func imageResponse(t *testing.T, source image.Image, query string) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	if err := png.Encode(&body, source); err != nil {
		t.Fatal(err)
	}
	response := httptest.NewRecorder()
	NewHandler(Config{}).ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/image?"+query, &body))
	return response
}
