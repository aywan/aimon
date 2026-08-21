// Package color provides color interpolation helpers.
package color

import "aiboard-client/internal/entity"

// Percent linearly interpolates from -> to by percent (0..100).
// Values outside [0, 100] are clamped.
func Percent(from, to entity.Color, percent float64) entity.Color {
	if percent < 0 {
		percent = 0
	}
	if percent > 100 {
		percent = 100
	}

	t := percent / 100
	return entity.Color{
		lerpByte(from[0], to[0], t),
		lerpByte(from[1], to[1], t),
		lerpByte(from[2], to[2], t),
		lerpByte(from[3], to[3], t),
	}
}

func lerpByte(a, b byte, t float64) byte {
	return byte(float64(a)*(1-t) + float64(b)*t + 0.5)
}
