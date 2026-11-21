package main

import (
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/gif"
	"image/jpeg"
	"image/png"
	"io"
	"log"
	"net/http"
	"os"
	"strconv"

	"golang.org/x/image/draw"
)

const (
	defaultMaxSourceBytes int64 = 8 << 20
	defaultMaxPixels            = 16_000_000
	maxOutputDimension          = 1024
)

var (
	errSourceTooLarge = errors.New("source image is too large")
	errTooManyPixels  = errors.New("source image has too many pixels")
)

// Config controls image-size limits.
type Config struct {
	MaxSourceBytes  int64
	MaxPixels       int
	MaxOutputPixels int
	Logger          *log.Logger
}

type handler struct {
	maxSourceBytes  int64
	maxPixels       int
	maxOutputPixels int
	logger          *log.Logger
}

func main() {
	address := os.Getenv("IMAGE_PROXY_ADDR")
	if address == "" {
		address = ":8090"
	}
	log.Printf("imageproxy listening on %s", address)
	if err := http.ListenAndServe(address, NewHandler(Config{})); err != nil {
		log.Fatal(err)
	}
}

// NewHandler serves POST /image?width=<px>&height=<px> with image bytes as its body.
func NewHandler(config Config) http.Handler {
	if config.MaxSourceBytes <= 0 {
		config.MaxSourceBytes = defaultMaxSourceBytes
	}
	if config.MaxPixels <= 0 {
		config.MaxPixels = defaultMaxPixels
	}
	if config.MaxOutputPixels <= 0 {
		config.MaxOutputPixels = maxOutputDimension * maxOutputDimension
	}
	if config.Logger == nil {
		config.Logger = log.Default()
	}
	return &handler{
		maxSourceBytes:  config.MaxSourceBytes,
		maxPixels:       config.MaxPixels,
		maxOutputPixels: config.MaxOutputPixels,
		logger:          config.Logger,
	}
}

func (service *handler) ServeHTTP(writer http.ResponseWriter, request *http.Request) {
	if request.Method == http.MethodGet && request.URL.Path == "/" {
		writer.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = writer.Write([]byte("imageproxy is running\n"))
		return
	}
	if request.Method == http.MethodGet && request.URL.Path == "/health" {
		writer.Header().Set("Content-Type", "text/plain; charset=utf-8")
		_, _ = writer.Write([]byte("ok\n"))
		return
	}
	if request.Method != http.MethodPost {
		writer.Header().Set("Allow", http.MethodPost)
		http.Error(writer, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if request.URL.Path != "/image" {
		http.NotFound(writer, request)
		return
	}

	width, height, err := parseDimensions(request)
	if err != nil {
		http.Error(writer, err.Error(), http.StatusBadRequest)
		return
	}
	if width*height > service.maxOutputPixels {
		http.Error(writer, "requested output is too large", http.StatusBadRequest)
		return
	}
	mode := request.URL.Query().Get("mode")
	if mode != "" && mode != "cover" && mode != "contain" {
		http.Error(writer, "mode must be cover or contain", http.StatusBadRequest)
		return
	}
	trim := request.URL.Query().Get("trim")
	if trim != "" && trim != "alpha" {
		http.Error(writer, "trim must be alpha or empty", http.StatusBadRequest)
		return
	}

	source, err := service.decodeImage(request.Body)
	if err != nil {
		service.writeImageError(writer, err)
		return
	}
	if trim == "alpha" {
		source = trimTransparent(source)
	}
	writer.Header().Set("Content-Type", "image/png")
	writer.Header().Set("Cache-Control", "public, max-age=86400")
	output := image.Image(nil)
	if mode == "contain" {
		output = containAndScale(source, width, height)
	} else {
		output = centerCropAndScale(source, width, height)
	}
	if err := png.Encode(writer, output); err != nil {
		service.logger.Printf("encode image: %v", err)
		return
	}
	bounds := source.Bounds()
	service.logger.Printf("processed image: source=%dx%d output=%dx%d", bounds.Dx(), bounds.Dy(), width, height)
}

func parseDimensions(request *http.Request) (int, int, error) {
	width, err := parseDimension(request.URL.Query().Get("width"))
	if err != nil {
		return 0, 0, fmt.Errorf("width: %w", err)
	}
	height, err := parseDimension(request.URL.Query().Get("height"))
	if err != nil {
		return 0, 0, fmt.Errorf("height: %w", err)
	}
	return width, height, nil
}

func parseDimension(raw string) (int, error) {
	dimension, err := strconv.Atoi(raw)
	if err != nil || dimension < 1 || dimension > maxOutputDimension {
		return 0, fmt.Errorf("must be an integer from 1 to %d", maxOutputDimension)
	}
	return dimension, nil
}

func (service *handler) decodeImage(body io.Reader) (image.Image, error) {
	contents, err := io.ReadAll(io.LimitReader(body, service.maxSourceBytes+1))
	if err != nil {
		return nil, fmt.Errorf("read image body: %w", err)
	}
	if int64(len(contents)) > service.maxSourceBytes {
		return nil, errSourceTooLarge
	}
	config, _, err := image.DecodeConfig(bytes.NewReader(contents))
	if err != nil {
		return nil, fmt.Errorf("decode image header: %w", err)
	}
	if config.Width <= 0 || config.Height <= 0 || int64(config.Width)*int64(config.Height) > int64(service.maxPixels) {
		return nil, errTooManyPixels
	}
	decoded, _, err := image.Decode(bytes.NewReader(contents))
	if err != nil {
		return nil, fmt.Errorf("decode image: %w", err)
	}
	return decoded, nil
}

func (service *handler) writeImageError(writer http.ResponseWriter, err error) {
	status := http.StatusUnprocessableEntity
	message := "could not decode source image"
	if errors.Is(err, errSourceTooLarge) || errors.Is(err, errTooManyPixels) {
		message = err.Error()
	}
	http.Error(writer, message, status)
	service.logger.Printf("image request failed: %v", err)
}

type trimmedImage struct {
	image.Image
	area image.Rectangle
}

func (source trimmedImage) Bounds() image.Rectangle { return source.area }

// Only alpha=0 is padding: even the faintest antialiased edge is retained.
// Empty images keep their original bounds so resizing never divides by zero.
func trimTransparent(source image.Image) image.Image {
	bounds := source.Bounds()
	left, top, right, bottom := bounds.Max.X, bounds.Max.Y, bounds.Min.X, bounds.Min.Y
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			_, _, _, alpha := source.At(x, y).RGBA()
			if alpha == 0 {
				continue
			}
			left, top = min(left, x), min(top, y)
			right, bottom = max(right, x+1), max(bottom, y+1)
		}
	}
	area := image.Rect(left, top, right, bottom)
	if left >= right || top >= bottom || area == bounds {
		return source
	}
	if cropper, ok := source.(interface {
		SubImage(image.Rectangle) image.Image
	}); ok {
		return cropper.SubImage(area)
	}
	return trimmedImage{Image: source, area: area}
}

func centerCropAndScale(source image.Image, targetWidth, targetHeight int) *image.NRGBA {
	bounds := source.Bounds()
	sourceWidth, sourceHeight := bounds.Dx(), bounds.Dy()
	cropWidth, cropHeight := sourceWidth, sourceHeight
	if int64(sourceWidth)*int64(targetHeight) > int64(sourceHeight)*int64(targetWidth) {
		cropWidth = sourceHeight * targetWidth / targetHeight
	} else {
		cropHeight = sourceWidth * targetHeight / targetWidth
	}
	startX := bounds.Min.X + (sourceWidth-cropWidth)/2
	startY := bounds.Min.Y + (sourceHeight-cropHeight)/2
	crop := image.Rect(startX, startY, startX+cropWidth, startY+cropHeight)

	destination := image.NewNRGBA(image.Rect(0, 0, targetWidth, targetHeight))
	draw.CatmullRom.Scale(destination, destination.Bounds(), source, crop, draw.Over, nil)
	return destination
}

// Return the source aspect ratio inside the requested bounds. The renderer fits
// these real dimensions into its cards without baked bars or cropped box art.
func containAndScale(source image.Image, maxWidth, maxHeight int) *image.NRGBA {
	bounds := source.Bounds()
	width, height := maxWidth, maxHeight
	if int64(bounds.Dx())*int64(maxHeight) > int64(bounds.Dy())*int64(maxWidth) {
		height = max(1, bounds.Dy()*maxWidth/bounds.Dx())
	} else {
		width = max(1, bounds.Dx()*maxHeight/bounds.Dy())
	}
	destination := image.NewNRGBA(image.Rect(0, 0, width, height))
	draw.CatmullRom.Scale(destination, destination.Bounds(), source, bounds, draw.Src, nil)
	return destination
}

// Register standard-library decoders used by image.Decode.
var (
	_ = gif.GIF{}
	_ = jpeg.DefaultQuality
)
