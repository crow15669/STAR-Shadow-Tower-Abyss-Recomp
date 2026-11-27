// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef VERTEX_SHADER

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec2 a_tex;

layout(location = 0) out vec2 v_tex;

void main()
{
	gl_Position = vec4(a_pos.x, -a_pos.y, a_pos.z, a_pos.w);
	v_tex = a_tex;
}

#endif

#ifdef FRAGMENT_SHADER

layout(location = 0) in vec2 v_tex;

#if HAS_INTEGER_OUTPUT
	layout(location = 0) out uint o_col0;
	#define OUTPUT o_col0
#elif HAS_DEPTH_OUTPUT
	out float gl_FragDepth;
	#define OUTPUT gl_FragDepth
#elif HAS_FLOAT32_OUTPUT
	layout(location = 0) out float o_col0;
	#define OUTPUT o_col0
#elif HAS_STENCIL_OUTPUT
#else
	layout(location = 0) out vec4 o_col0;
	#define OUTPUT o_col0
#endif

layout(set = 0, binding = 0) uniform sampler2D samp0;

#if HAS_FLOAT32_INPUT

float sample_c(vec2 uv)
{
	return texture(samp0, uv).r;
}

#else

vec4 sample_c(vec2 uv)
{
	return texture(samp0, uv);
}

#endif

uint rgba8_to_uint(vec4 c)
{
	uvec4 i = uvec4(c * 255.5f) & 0xFFu;
	return i.r | (i.g << 8) | (i.b << 16) | (i.a << 24);
}

uint rgb5a1_to_uint(vec4 c)
{
	uvec4 i = uvec4(c * 255.5f) & uvec4(0xF8u, 0xF8u, 0xF8u, 0x80u);
	return (i.r >> 3) | (i.g << 2) | (i.b << 7) | (i.a << 8);
}

uint depth_to_uint(float d)
{
	return uint(d * exp2(32.0f));
}

vec4 uint_to_rgba8(uint i)
{
	return vec4((i & 0xFFu), ((i >> 8) & 0xFFu), ((i >> 16) & 0xFFu), ((i >> 24) & 0xFFu)) / 255.0f;
}

vec4 uint_to_rgb5a1(uint i)
{
	return vec4(uvec4(i << 3, i >> 2, i >> 7, i >> 8) & uvec4(0xF8u, 0xF8u, 0xF8u, 0x80u)) / 255.0f;
}

float uint_to_depth32(uint i)
{
	return float(i) * exp2(-32.0f);
}

float uint_to_depth24(uint i)
{
	return float(i & 0xFFFFFFu) * exp2(-32.0f);
}

float uint_to_depth16(uint i)
{
	return float(i & 0xFFFFu) * exp2(-32.0f);
}

float rgba8_to_depth32(vec4 val)
{
	return uint_to_depth32(rgba8_to_uint(val));
}

float rgba8_to_depth24(vec4 val)
{
	return uint_to_depth24(rgba8_to_uint(val));
}

float rgba8_to_depth16(vec4 val)
{
	return uint_to_depth16(rgba8_to_uint(val));
}

float rgb5a1_to_depth16(vec4 val)
{
	return uint_to_depth16(rgb5a1_to_uint(val));
}

vec4 depth32_to_rgba8(float d)
{
	return uint_to_rgba8(depth_to_uint(d));
}

vec4 depth16_to_rgb5a1(float d)
{
	return uint_to_rgb5a1(depth_to_uint(d));
}

float depth32_to_depth24(float d)
{
	return uint_to_depth24(depth_to_uint(d));
}

vec4 primid_to_rgba8(float p)
{
	return uint_to_rgba8(floatBitsToUint(p));
}

#ifdef ps_copy
void ps_copy()
{
	OUTPUT = sample_c(v_tex);
}
#endif

#ifdef ps_depth_copy
void ps_depth_copy()
{
	OUTPUT = sample_c(v_tex);
}
#endif

#ifdef ps_downsample_copy
layout(push_constant) uniform cb10
{
	ivec2 ClampMin;
	int DownsampleFactor;
	int pad0;
	float Weight;
	float step_multiplier;
	vec2 pad1;
};
void ps_downsample_copy()
{
	ivec2 coord = max(ivec2(gl_FragCoord.xy) * DownsampleFactor, ClampMin);
	vec4 result = vec4(0);
	for (int yoff = 0; yoff < DownsampleFactor; yoff++)
	{
		for (int xoff = 0; xoff < DownsampleFactor; xoff++)
		{
			result += texelFetch(samp0, coord + ivec2(xoff * step_multiplier, yoff * step_multiplier), 0);
		}
	}
	OUTPUT = result / Weight;
}
#endif

#ifdef ps_filter_transparency
void ps_filter_transparency()
{
	vec4 c = sample_c(v_tex);
	OUTPUT = vec4(c.rgb, 1.0);
}
#endif

#ifdef ps_convert_rgb5a1_16bits
void ps_convert_rgb5a1_16bits()
{
	// Need to be careful with precision here, it can break games like Spider-Man 3 and Dogs Life
	OUTPUT = rgb5a1_to_uint(sample_c(v_tex));
}
#endif

#ifdef ps_datm1
void ps_datm1()
{
	if(sample_c(v_tex).a < (127.5f / 255.0f)) // >= 0x80 pass
		discard;
}
#endif

#ifdef ps_datm0
void ps_datm0()
{
	if((127.5f / 255.0f) < sample_c(v_tex).a) // < 0x80 pass (== 0x80 should not pass)
		discard;
}
#endif

#ifdef ps_datm1_rta_correction
void ps_datm1_rta_correction()
{
	if(sample_c(v_tex).a < (254.5f / 255.0f)) // >= 0x80 pass
		discard;
}
#endif

#ifdef ps_datm0_rta_correction
void ps_datm0_rta_correction()
{
	if((254.5f / 255.0f) < sample_c(v_tex).a) // < 0x80 pass (== 0x80 should not pass)
		discard;
}
#endif

#ifdef ps_rta_correction
void ps_rta_correction()
{
	vec4 value = sample_c(v_tex);
	OUTPUT = vec4(value.rgb, value.a / (128.25f / 255.0f));
}
#endif

#ifdef ps_rta_decorrection
void ps_rta_decorrection()
{
	vec4 value = sample_c(v_tex);
	OUTPUT = vec4(value.rgb, value.a * (128.25f / 255.0f));
}
#endif

#ifdef ps_colclip_init
void ps_colclip_init()
{
	vec4 value = sample_c(v_tex);
	OUTPUT = vec4(roundEven(value.rgb * 255.0f) / 65535.0f, value.a);
}
#endif

#ifdef ps_colclip_resolve
void ps_colclip_resolve()
{
	vec4 value = sample_c(v_tex);
	OUTPUT = vec4(vec3(uvec3(value.rgb * 65535.5f) & 255u) / 255.0f, value.a);
}
#endif

#ifdef ps_convert_depth32_32bits
void ps_convert_depth32_32bits()
{
	// Convert a vec32 depth texture into a 32 bits UINT texture
	OUTPUT = depth_to_uint(sample_c(v_tex));
}
#endif

#ifdef ps_convert_depth32_rgba8
void ps_convert_depth32_rgba8()
{
	// Convert a vec32 depth texture into a RGBA color texture
	OUTPUT = depth32_to_rgba8(sample_c(v_tex));
}
#endif

#ifdef ps_convert_depth16_rgb5a1
void ps_convert_depth16_rgb5a1()
{
	// Convert a vec32 (only 16 lsb) depth into a RGB5A1 color texture
	OUTPUT = depth16_to_rgb5a1(sample_c(v_tex));
}
#endif

#ifdef ps_convert_depth32_depth24
void ps_convert_depth32_depth24()
{
	// Truncates depth value to 24bits
	OUTPUT = depth32_to_depth24(sample_c(v_tex));
}
#endif

#ifdef ps_convert_primid_rgba8
void ps_convert_primid_rgba8()
{
	OUTPUT = primid_to_rgba8(sample_c(v_tex));
}
#endif

#define SAMPLE_RGBA_DEPTH_BILN(CONVERT_FN) \
	ivec2 dims = textureSize(samp0, 0); \
	vec2 top_left_f = v_tex * vec2(dims) - 0.5f; \
	ivec2 top_left = ivec2(floor(top_left_f)); \
	ivec4 coords = clamp(ivec4(top_left, top_left + 1), ivec4(0), dims.xyxy - 1); \
	vec2 mix_vals = fract(top_left_f); \
	float depthTL = CONVERT_FN(texelFetch(samp0, coords.xy, 0)); \
	float depthTR = CONVERT_FN(texelFetch(samp0, coords.zy, 0)); \
	float depthBL = CONVERT_FN(texelFetch(samp0, coords.xw, 0)); \
	float depthBR = CONVERT_FN(texelFetch(samp0, coords.zw, 0)); \
	OUTPUT = floor(mix(mix(depthTL, depthTR, mix_vals.x), mix(depthBL, depthBR, mix_vals.x), mix_vals.y) * exp2(32.0f)) * exp2(-32.0f);

#ifdef ps_convert_rgba8_depth32
void ps_convert_rgba8_depth32()
{
	// Convert an RGBA texture into a float depth texture
#if HAS_BILN
	SAMPLE_RGBA_DEPTH_BILN(rgba8_to_depth32);
#else
	OUTPUT = rgba8_to_depth32(sample_c(v_tex));
#endif
}
#endif

#ifdef ps_convert_rgba8_depth24
void ps_convert_rgba8_depth24()
{
	// Same as above but without the alpha channel (24 bits Z)
#if HAS_BILN
	SAMPLE_RGBA_DEPTH_BILN(rgba8_to_depth24);
#else
	OUTPUT = rgba8_to_depth24(sample_c(v_tex));
#endif
}
#endif

#ifdef ps_convert_rgba8_depth16
void ps_convert_rgba8_depth16()
{
	// Same as above but without the A/B channels (16 bits Z)
#if HAS_BILN
	SAMPLE_RGBA_DEPTH_BILN(rgba8_to_depth16);
#else
	OUTPUT = rgba8_to_depth16(sample_c(v_tex));
#endif
}
#endif

#ifdef ps_convert_rgb5a1_depth16
void ps_convert_rgb5a1_depth16()
{
	// Convert an RGB5A1 (saved as RGBA8) color to a 16 bit Z
#if HAS_BILN
	SAMPLE_RGBA_DEPTH_BILN(rgb5a1_to_depth16);
#else
	OUTPUT = rgb5a1_to_depth16(sample_c(v_tex));
#endif
}
#endif

#ifdef ps_convert_rgb5a1_8i
layout(push_constant) uniform cb10
{
	uint SBW;
	uint DBW;
	uint PSM;
	float cb_pad1;
	float ScaleFactor;
	vec3 cb_pad2;
};

void ps_convert_rgb5a1_8i()
{
	// Convert a RGB5A1 texture into a 8 bits packed texture
	// Input column: 16x2 RGB5A1 pixels
	// 0: 16 RGBA
	// 1: 16 RGBA
	// Output column: 16x4 Index pixels
	// 0: 16 R5G2
	// 1: 16 R5G2
	// 2: 16 G2B5A1
	// 3: 16 G2B5A1

	uvec2 pos = uvec2(gl_FragCoord.xy);

	// Collapse separate R G B A areas into their base pixel
	uvec2 column = (pos & ~uvec2(0u, 3u)) / uvec2(1u, 2u);
	uvec2 subcolumn = (pos & uvec2(0u, 1u));
	column.x -= (column.x / 128u) * 64u;
	column.y += (column.y / 32u) * 32u;

	// Deal with swizzling differences
	if ((PSM & 0x8u) != 0u) // PSMCT16S
	{
		if ((pos.x & 32u) != 0u)
		{
			column.y += 32u; // 4 columns high times 4 to get bottom 4 blocks
			column.x &= ~32u;
		}

		if ((pos.x & 64u) != 0u)
		{
			column.x -= 32u;
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // PSMZ16S - Untested but hopefully ok if anything uses it.
		{
			column.x ^= 32u;
			column.y ^= 16u;
		}
	}
	else // PSMCT16
	{
		if ((pos.y & 32u) != 0u)
		{
			column.y -= 16u;
			column.x += 32u;
		}

		if ((pos.x & 96u) != 0u)
		{
			uint multi = (pos.x & 96u) / 32u;
			column.y += 16u * multi; // 4 columns high times 4 to get bottom 4 blocks
			column.x -= (pos.x & 96u);
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // PSMZ16 - Untested but hopefully ok if anything uses it.
		{
			column.x ^= 32u;
			column.y ^= 32u;
		}
	}
	uvec2 coord = column | subcolumn;

	// Compensate for potentially differing page pitch.
	uvec2 block_xy = coord / uvec2(64u, 64u);
	uint block_num = (block_xy.y * (DBW / 128u)) + block_xy.x;
	uvec2 block_offset = uvec2((block_num % (SBW / 64u)) * 64u, (block_num / (SBW / 64u)) * 64u);
	coord = (coord % uvec2(64u, 64u)) + block_offset;

	// Apply offset to cols 1 and 2
	uint is_col23 = pos.y & 4u;
	uint is_col13 = pos.y & 2u;
	uint is_col12 = is_col23 ^ (is_col13 << 1);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	if (floor(ScaleFactor) != ScaleFactor)
		coord = uvec2(vec2(coord) * ScaleFactor);
	else
		coord *= uvec2(ScaleFactor);

	vec4 pixel = texelFetch(samp0, ivec2(coord), 0);

	uvec4 denorm_c = uvec4(pixel * 255.5f);
	if ((pos.y & 2u) == 0u)
	{
		uint red = (denorm_c.r >> 3) & 0x1Fu;
		uint green = (denorm_c.g >> 3) & 0x1Fu;

		o_col0 = vec4(float(((green << 5) | red) & 0xFFu) / 255.0f);
	}
	else
	{
		uint green = (denorm_c.g >> 3) & 0x1Fu;
		uint blue = (denorm_c.b >> 3) & 0x1Fu;
		uint alpha = denorm_c.a & 0x80u;

		o_col0 = vec4(float((alpha | (blue << 2) | (green >> 3)) & 0xFFu) / 255.0f);
	}
}
#endif

#ifdef ps_convert_rgba_8i
layout(push_constant) uniform cb10
{
	uint SBW;
	uint DBW;
	uint PSM;
	float cb_pad1;
	float ScaleFactor;
	vec3 cb_pad2;
};

void ps_convert_rgba_8i()
{
	// Convert a RGBA texture into a 8 bits packed texture
	// Input column: 8x2 RGBA pixels
	// 0: 8 RGBA
	// 1: 8 RGBA
	// Output column: 16x4 Index pixels
	// 0: 8 R | 8 B
	// 1: 8 R | 8 B
	// 2: 8 G | 8 A
	// 3: 8 G | 8 A
	uvec2 pos = uvec2(gl_FragCoord.xy);

	// Collapse separate R G B A areas into their base pixel
	uvec2 block = (pos & ~uvec2(15u, 3u)) >> 1;
	uvec2 subblock = pos & uvec2(7u, 1u);
	uvec2 coord = block | subblock;

	// Compensate for potentially differing page pitch.
	uvec2 block_xy = coord / uvec2(64u, 32u);
	uint block_num = (block_xy.y * (DBW / 128u)) + block_xy.x;
	uvec2 block_offset = uvec2((block_num % (SBW / 64u)) * 64u, (block_num / (SBW / 64u)) * 32u);
	coord = (coord % uvec2(64u, 32u)) + block_offset;

	// Apply offset to cols 1 and 2
	uint is_col23 = pos.y & 4u;
	uint is_col13 = pos.y & 2u;
	uint is_col12 = is_col23 ^ (is_col13 << 1);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	if (floor(ScaleFactor) != ScaleFactor)
		coord = uvec2(vec2(coord) * ScaleFactor);
	else
		coord *= uvec2(ScaleFactor);

	vec4 pixel = texelFetch(samp0, ivec2(coord), 0);
	vec2  sel0 = (pos.y & 2u) == 0u ? pixel.rb : pixel.ga;
	float sel1 = (pos.x & 8u) == 0u ? sel0.x : sel0.y;
	o_col0 = vec4(sel1); // Divide by something here?
}
#endif

#ifdef ps_convert_clut_4
layout(push_constant) uniform cb10
{
	uvec2 offset;
	uint doffset;
	uint cb_pad1;
	float scale;
	vec3 cb_pad2;
};

void ps_convert_clut_4()
{
	// CLUT4 is easy, just two rows of 8x8.
	uint index = uint(gl_FragCoord.x) + doffset;
	uvec2 pos = uvec2(index % 8u, index / 8u);

	ivec2 final = ivec2(floor(vec2(offset + pos) * vec2(scale)));
	o_col0 = texelFetch(samp0, final, 0);
}
#endif

#ifdef ps_convert_clut_8
layout(push_constant) uniform cb10
{
	uvec2 offset;
	uint doffset;
	uint cb_pad1;
	float scale;
	vec3 cb_pad2;
};

void ps_convert_clut_8()
{
	uint index = min(uint(gl_FragCoord.x) + doffset, 255u);

	// CLUT is arranged into 8 groups of 16x2, with the top-right and bottom-left quadrants swapped.
	// This can probably be done better..
	uint subgroup = (index / 8u) % 4u;
	uvec2 pos;
	pos.x = (index % 8u) + ((subgroup >= 2u) ? 8u : 0u);
	pos.y = ((index / 32u) * 2u) + (subgroup % 2u);

	ivec2 final = ivec2(floor(vec2(offset + pos) * vec2(scale)));
	o_col0 = texelFetch(samp0, final, 0);
}
#endif

#ifdef ps_star_walls
// pcsx2gs (STAR): shadows stop at walls (GSRendererHW::ApplyOwnerShadow). A creature's shadow volume darkens every
// surface inside it, so its shadow goes on through a pillar: on the side facing the light, again on the far side and on
// the ground behind. In: the owners' map (colour = the scene's depth, the GS z value's low 24 bits; alpha = the object
// shown, 1-126 a creature or thing, else scenery). Out, per texel: the same alpha, and in the colour a 24-bit code for
// how far toward the light a shadow can come from: 0 = the surface turns away from the light (no shadow at all),
// 0xFFFFFF = nothing on screen in the way, else 2^23 + 4 x (the light coordinate dot(P, L) of the first scenery met
// going from the surface toward the light). A piece's volume counts there only if the piece is nearer than that.
layout(push_constant) uniform cb10
{
	vec4 WallsI0; // texel x, y, depth, 1 -> the world (homogeneous; w = 1 / the distance along the view), dot rows
	vec4 WallsI1;
	vec4 WallsI2;
	vec4 WallsI3;
	vec4 WallsLightImage; // the direction to the light through the projection, in texel units (x w, y w, z w, w)
	vec4 WallsLight;      // the direction to the light; w = how far behind a surface still counts as inside it
};

float WallsZ(ivec2 p)
{
	vec3 c = floor(texelFetch(samp0, p, 0).rgb * 255.0f + 0.5f);
	return c.r + c.g * 256.0f + c.b * 65536.0f;
}
vec4 WallsH(vec2 p, float z)
{
	vec4 s = vec4(p, z, 1.0f);
	return vec4(dot(WallsI0, s), dot(WallsI1, s), dot(WallsI2, s), dot(WallsI3, s));
}
vec3 WallsP(ivec2 p)
{
	vec4 h = WallsH(vec2(p) + 0.5f, WallsZ(p));
	return h.xyz / h.w;
}

void ps_star_walls()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	vec4 here = texelFetch(samp0, p, 0);
	uint shown = uint(here.a * 255.0f + 0.5f);
	ivec2 size = textureSize(samp0, 0);
	float code = 16777215.0f; // nothing in the way
	float zc = WallsZ(p);
	if ((shown == 0u || shown > 126u) && zc >= 2.0f && p.x >= 2 && p.y >= 2 && p.x < size.x - 2 && p.y < size.y - 2)
	{
		vec4 hp = WallsH(vec2(p) + 0.5f, zc);
		vec3 P = hp.xyz / hp.w;
		// the surface's direction, from the texels two away (the map is the picture stretched: the next texel can be
		// the same pixel again), on each axis the side nearer in depth (not across an edge); turned to the eye
		ivec2 sx = abs(WallsZ(p + ivec2(2, 0)) - zc) <= abs(WallsZ(p - ivec2(2, 0)) - zc) ? ivec2(2, 0) : ivec2(-2, 0);
		ivec2 sy = abs(WallsZ(p + ivec2(0, 2)) - zc) <= abs(WallsZ(p - ivec2(0, 2)) - zc) ? ivec2(0, 2) : ivec2(0, -2);
		vec3 n = cross((WallsP(p + sx) - P) * float(sx.x), (WallsP(p + sy) - P) * float(sy.y));
		vec4 eye = vec4(WallsI0.z, WallsI1.z, WallsI2.z, WallsI3.z);
		if (dot(n, eye.xyz / eye.w - P) < 0.0f)
			n = -n;
		vec3 L = WallsLight.xyz;
		if (length(n) > 0.0f && dot(n, L) <= 0.0f)
		{
			code = 0.0f; // turned away from the light
		}
		else if (length(n) > 0.0f)
		{
			// from the surface toward the light, 1024 units in 24 steps: the first point that lies behind the scenery
			// the picture shows there (by more than the depth's own fuzz, less than WallsLight.w) is inside it
			float wp = 1.0f / hp.w;
			vec4 start = wp * vec4(vec2(p) + 0.5f, zc, 1.0f);
			for (int i = 1; i <= 24; i++)
			{
				float t = 1024.0f * float(i) / 24.0f;
				vec4 q = start + t * WallsLightImage;
				if (q.w <= 1.0f)
					break;
				ivec2 tq = ivec2(q.xy / q.w);
				if (any(lessThan(tq, ivec2(0))) || any(greaterThanEqual(tq, size)))
					break;
				vec4 m = texelFetch(samp0, tq, 0);
				uint a = uint(m.a * 255.0f + 0.5f);
				if (a != 0u && a <= 126u)
					continue; // a creature or thing: not scenery
				vec3 c = floor(m.rgb * 255.0f + 0.5f);
				float zs = c.r + c.g * 256.0f + c.b * 65536.0f;
				if (zs < 2.0f)
					continue;
				float behind = q.w - 1.0f / WallsH(vec2(tq) + 0.5f, zs).w;
				if (behind > 2.0f + 0.01f * q.w && behind < WallsLight.w)
				{
					float reach = dot(P, L) + t - 512.0f / 24.0f;
					code = clamp(floor(8388608.0f + reach * 4.0f), 1.0f, 16777214.0f);
					break;
				}
			}
		}
	}
	uint u = uint(code);
	OUTPUT = vec4(float(u & 255u), float((u >> 8) & 255u), float((u >> 16) & 255u), floor(here.a * 255.0f + 0.5f)) / 255.0f;
}
#endif

#ifdef ps_yuv
layout(push_constant) uniform cb10
{
	int EMODA;
	int EMODC;
};

void ps_yuv()
{
	vec4 i = sample_c(v_tex);
	vec4 o = vec4(0.0f);

	mat3 rgb2yuv;
	rgb2yuv[0] = vec3(0.587, -0.311, -0.419);
	rgb2yuv[1] = vec3(0.114, 0.500, -0.081);
	rgb2yuv[2] = vec3(0.299, -0.169, 0.500);

	vec3 yuv = rgb2yuv * i.gbr;

	float Y = float(0xDB)/255.0f * yuv.x + float(0x10)/255.0f;
	float Cr = float(0xE0)/255.0f * yuv.y + float(0x80)/255.0f;
	float Cb = float(0xE0)/255.0f * yuv.z + float(0x80)/255.0f;

	switch(EMODA)
	{
		case 0:
			o.a = i.a;
			break;
		case 1:
			o.a = Y;
			break;
		case 2:
			o.a = Y/2.0f;
			break;
		case 3:
			o.a = 0.0f;
			break;
	}

	switch(EMODC)
	{
		case 0:
			o.rgb = i.rgb;
			break;
		case 1:
			o.rgb = vec3(Y);
			break;
		case 2:
			o.rgb = vec3(Y, Cb, Cr);
			break;
		case 3:
			o.rgb = vec3(i.a);
			break;
	}

	o_col0 = o;
}
#endif

#if defined(ps_primid_image_init_0) || defined(ps_primid_image_init_1) || defined(ps_primid_image_init_2) || defined(ps_primid_image_init_3)

void main()
{
	o_col0 = vec4(PRIMID_MAX);

	#ifdef ps_primid_image_init_0
		if((127.5f / 255.0f) < sample_c(v_tex).a) // < 0x80 pass (== 0x80 should not pass)
			o_col0 = vec4(PRIMID_MIN);
	#endif
	#ifdef ps_primid_image_init_1
		if(sample_c(v_tex).a < (127.5f / 255.0f)) // >= 0x80 pass
			o_col0 = vec4(PRIMID_MIN);
	#endif
	#ifdef ps_primid_image_init_2
		if((254.5f / 255.0f) < sample_c(v_tex).a) // < 0x80 pass (== 0x80 should not pass)
			o_col0 = vec4(PRIMID_MIN);
	#endif
	#ifdef ps_primid_image_init_3
		if(sample_c(v_tex).a < (254.5f / 255.0f)) // >= 0x80 pass
			o_col0 = vec4(PRIMID_MIN);
	#endif
}
#endif

#endif
