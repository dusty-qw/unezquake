#version 430

#ezquake-definitions

layout(binding = 0) uniform sampler2D jfa_map;
layout(binding = 1) uniform sampler2D outline_mask_texture;
// jfa_map payload: rg = nearest seed coordinate, b = signed distance, a = propagated depth.
// outline_mask_texture payload: rgb = outline color, a = encoded depth.

uniform float thickness;
uniform int mode;
uniform int inside;
uniform vec3 outline_color;

in vec2 TextureCoord;
out vec4 frag_colour;

float fwidth2(float v)
{
	float vdy = dFdy(v);
	float vdx = dFdx(v);
	return length(vec2(vdy, vdx));
}

float smoothstep_inv(float edge0, float edge1, float x)
{
	return 1.0 - smoothstep(edge0, edge1, x);
}

float linearize_depth(float depth)
{
	// Reconstruct linear eye depth directly from the projection matrix.
	// This works for both regular and reversed depth.
	float ndc_z = (projectionMatrix[2][2] > -0.5) ? depth : (depth * 2.0 - 1.0);
	float denom = ndc_z + projectionMatrix[2][2];
	return abs(projectionMatrix[3][2] / max(abs(denom), 1e-6));
}

void main()
{
	ivec2 size = textureSize(jfa_map, 0);
	ivec2 curr_coord = ivec2(TextureCoord * vec2(size));
	curr_coord = clamp(curr_coord, ivec2(0), size - ivec2(1));
	vec4 s = texelFetch(jfa_map, curr_coord, 0).rgba;
	float dist = s.z * float(inside);
	float alpha = 0.0;
	float effective_thickness = thickness;
	float max_dist;
	vec3 edge_color = outline_color;

	if (s.z == 0.0) {
		discard;
	}
	{
		ivec2 nearest_coord = clamp(ivec2(s.rg), ivec2(0), size - ivec2(1));
		vec4 mask_data = texelFetch(outline_mask_texture, nearest_coord, 0).rgba;
		const float depth_epsilon = 1.0 / 1024.0;
		float depth = 0.0;

		if (mask_data.a > 1e-6) {
			depth = clamp((mask_data.a - depth_epsilon) / (1.0 - depth_epsilon), 0.0, 1.0);
			edge_color = mask_data.rgb;
		}

		if (depth > 0.0) {
			// Keep nearby outlines unchanged, thin out far outlines so they remain
			// proportional to model size on-screen.
			float linear_depth = linearize_depth(depth);
			float reference_depth = 96.0;
			float distance_scale = clamp(reference_depth / max(linear_depth, r_zNear), 0.02, 1.0);
			effective_thickness = max(0.05, thickness * distance_scale);
		}
	}
	max_dist = (mode == 1) ? (effective_thickness * 2.0) : (effective_thickness + 1.0);
	if (dist <= -1.0 || dist > max_dist) {
		discard;
	}

	if (mode == 1) {
		// mode 1: glow
		float w = 0.5;
		float norm = clamp(dist / max(effective_thickness, 0.05), 0.0, 2.0);
		float shifted_norm = clamp(norm + 0.35, 0.0, 2.35);
		float entry = smoothstep(-w - 1.0, w - 1.0, dist);
		float falloff = exp2(-4.0 * shifted_norm * shifted_norm);
		float tail = smoothstep(2.0, 0.0, norm);

		alpha = clamp(entry * falloff * tail * 0.9, 0.0, 1.0);
	}
	else if (mode == 2) {
		// mode 2: halftone
		float dot_radius = 13.0 * min(effective_thickness, 40.0) * 0.02;
		float dot_width = dot_radius * 2.0;
		vec2 closest_dot = floor(vec2(curr_coord) / dot_width) * dot_width + vec2(dot_radius);
		float halftone_alpha = 0.0;
		int x, y;

		for (x = -1; x <= 1; ++x) {
			for (y = -1; y <= 1; ++y) {
				vec2 offset = vec2(x, y) * dot_width;
				vec2 dot_coord = closest_dot + offset;
				ivec2 dot_coord_i = ivec2(dot_coord);

				if (dot_coord_i.x < 0 || dot_coord_i.x >= size.x || dot_coord_i.y < 0 || dot_coord_i.y >= size.y) {
					continue;
				}

				float dot_strength = texelFetch(jfa_map, dot_coord_i, 0).b * float(inside);
				if (dot_strength != 0.0) {
					float strength;
					float dist_to_dot;
					float new_alpha;

					dot_strength += dot_radius * 1.3;
					dot_strength /= 1.5;

					strength = clamp(1.0 - dot_strength / max(effective_thickness, 0.05), 0.0, 1.0);
					strength = 1.0 - pow(1.0 - strength, 0.75);

					dist_to_dot = length(vec2(curr_coord) - dot_coord);
					new_alpha = smoothstep(strength * dot_width, strength * dot_width - 1.0, dist_to_dot);
					halftone_alpha = max(halftone_alpha, new_alpha);
				}
			}
		}

		alpha = halftone_alpha * smoothstep(-1.5, -0.5, s.b * float(inside));
	}
	else {
		// mode 0: outline (default)
		float w = max(clamp(fwidth2(dist), -1.0, 1.0) * 0.5, 0.5);
		float val = smoothstep_inv(effective_thickness - w, effective_thickness + w, dist) * smoothstep(-w - 1.0, w - 1.0, dist);

		alpha = clamp(val, 0.0, 1.0);
	}

	if (alpha <= 0.0) {
		discard;
	}

	// r_state_fx_world_geometry uses premultiplied alpha blending.
	frag_colour.rgb = edge_color * alpha;
	frag_colour.a = alpha;
}
