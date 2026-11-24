package main

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestPrepare(t *testing.T) {
	for _, key := range []bool{true, false} {
		t.Run(map[bool]string{true: "keyed icon", false: "opaque wallpaper"}[key], func(t *testing.T) {
			dir := t.TempDir()
			src, dst := filepath.Join(dir, "source.png"), filepath.Join(dir, "out.png")
			im := image.NewNRGBA(image.Rect(0, 0, 32, 32))
			for y := 0; y < 32; y++ {
				for x := 0; x < 32; x++ {
					c := color.NRGBA{230, 70, 160, 255}
					if x >= 8 && x < 24 && y >= 8 && y < 24 {
						c = color.NRGBA{20, 180, 140, 255}
					}
					im.SetNRGBA(x, y, c)
				}
			}
			f, err := os.Create(src)
			if err != nil {
				t.Fatal(err)
			}
			if err := png.Encode(f, im); err != nil {
				t.Fatal(err)
			}
			f.Close()
			args := []string{"-in", src, "-out", dst, "-width", "16", "-height", "16"}
			if key {
				args = append(args, "-key")
			}
			if err := run(args); err != nil {
				t.Fatal(err)
			}
			f, err = os.Open(dst)
			if err != nil {
				t.Fatal(err)
			}
			defer f.Close()
			out, err := png.Decode(f)
			if err != nil {
				t.Fatal(err)
			}
			if out.Bounds().Dx() != 16 || out.Bounds().Dy() != 16 {
				t.Fatal(out.Bounds())
			}
			_, _, _, a := out.At(0, 0).RGBA()
			if key && a != 0 {
				t.Fatal("keyed background remains opaque")
			}
			if !key && a != 65535 {
				t.Fatal("wallpaper changed alpha")
			}
			r, g, b, a := out.At(8, 8).RGBA()
			if g <= r || g <= b || a != 65535 {
				t.Fatal("foreground color/alpha lost")
			}
		})
	}
}

func TestRejectDoesNotOverwrite(t *testing.T) {
	dir := t.TempDir()
	dst := filepath.Join(dir, "keep.png")
	before := []byte("existing asset")
	if err := os.WriteFile(dst, before, 0600); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{
		{"-in", "missing", "-out", dst},
		{"-in", "missing", "-out", dst, "-width", "0"},
	} {
		if run(args) == nil {
			t.Fatal("invalid command succeeded")
		}
		after, err := os.ReadFile(dst)
		if err != nil || !bytes.Equal(before, after) {
			t.Fatal("rejected input overwrote output")
		}
	}
}
