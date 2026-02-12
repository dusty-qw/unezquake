#version 430

#ezquake-definitions

layout(binding = 0) uniform sampler2D outline_mask_texture;

out vec4 frag_colour;

void main()
{
	ivec2 size = textureSize(outline_mask_texture, 0);
	ivec2 coord = ivec2(gl_FragCoord.xy);
	coord = clamp(coord, ivec2(0), size - ivec2(1));
	vec4 mask_data = texelFetch(outline_mask_texture, coord, 0).rgba;
	const float depth_epsilon = 1.0 / 1024.0;
	float mask = step(1e-6, mask_data.a);
	float sign_value = (mask > 0.5) ? -1.0 : 1.0;
	float seed_depth = 0.0;

	if (mask > 0.5) {
		seed_depth = clamp((mask_data.a - depth_epsilon) / (1.0 - depth_epsilon), 0.0, 1.0);
	}

	// JFA payload:
	//   rg = nearest seed coordinate (starts at current pixel)
	//   b  = signed distance estimate (large +/- value at seed stage)
	//   a  = decoded depth copied from the source mask
	frag_colour = vec4(gl_FragCoord.xy, 1e4 * sign_value, seed_depth);
}
