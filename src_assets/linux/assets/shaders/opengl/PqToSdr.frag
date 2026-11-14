#version 300 es

#ifdef GL_ES
precision highp float;
#endif

// Tone maps a PQ (SMPTE ST 2084) BT.2020 frame to SDR sRGB BT.709.
// Mirrors src/platform/linux/pq_tonemap_math.h; keep the two in step.

uniform sampler2D image;
uniform sampler2D output_lut;
uniform float sdr_white_nits;

in vec2 tex;
layout(location = 0) out vec4 color;

float output_lut_coordinate(float value)
{
	float size = float(textureSize(output_lut, 0).x);
	return (clamp(value, 0.0, 1.0) * (size - 1.0) + 0.5) / size;
}

vec3 pq_to_nits(vec3 encoded)
{
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	vec3 p = pow(clamp(encoded, 0.0, 1.0), vec3(1.0 / m2));
	vec3 ratio = max(p - c1, 0.0) / (c2 - c3 * p);
	return 10000.0 * pow(ratio, vec3(1.0 / m1));
}

vec3 srgb_encode(vec3 linear)
{
	vec3 low = linear * 12.92;
	vec3 high = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, vec3(lessThanEqual(linear, vec3(0.0031308))));
}

void main()
{
	vec3 encoded = texture(image, tex).rgb;
	encoded = vec3(
		texture(output_lut, vec2(output_lut_coordinate(encoded.r), 0.5)).r,
		texture(output_lut, vec2(output_lut_coordinate(encoded.g), 0.5)).g,
		texture(output_lut, vec2(output_lut_coordinate(encoded.b), 0.5)).b
	);

	vec3 bt2020 = pq_to_nits(encoded) / sdr_white_nits;

	// BT.2020 -> BT.709 primaries (column-major).
	const mat3 to_bt709 = mat3(
		1.6605, -0.1246, -0.0182,
		-0.5876, 1.1329, -0.1006,
		-0.0728, -0.0083, 1.1187
	);
	vec3 rgb = max(to_bt709 * bt2020, 0.0);

	// SDR content (up to SDR white) passes through exactly. Brighter
	// highlights are scaled down as a whole, so they keep their hue.
	float peak = max(rgb.r, max(rgb.g, rgb.b));
	if (peak > 1.0) {
		rgb /= peak;
	}

	color = vec4(srgb_encode(clamp(rgb, 0.0, 1.0)), 1.0);
}
