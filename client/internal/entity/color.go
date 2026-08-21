package entity

import "fmt"

// Color is an RGBA color as [R, G, B, A].
type Color [4]byte

var (
	Red   = Color{0xff, 0x00, 0x00, 0xff}
	Green = Color{0x00, 0xff, 0x00, 0xff}
	Blue  = Color{0x00, 0x00, 0xff, 0xff}
	White = Color{0xff, 0xff, 0xff, 0xff}
)

// ToHexRGB returns "#rrggbb" (alpha is omitted).
func (c Color) ToHexRGB() string {
	return fmt.Sprintf("#%02x%02x%02x", c[0], c[1], c[2])
}

// ToHexRGBA returns "#rrggbbaa".
func (c Color) ToHexRGBA() string {
	return fmt.Sprintf("#%02x%02x%02x%02x", c[0], c[1], c[2], c[3])
}

// String implements fmt.Stringer as ToHexRGB.
func (c Color) String() string {
	return c.ToHexRGB()
}
