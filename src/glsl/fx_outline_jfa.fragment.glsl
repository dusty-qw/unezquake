#version 430

#ezquake-definitions

layout(binding = 0) uniform sampler2D source;
uniform int step;

in vec2 TextureCoord;
out vec4 frag_colour;

void main()
{
	ivec2 size = textureSize(source, 0);
	ivec2 curr_coord = ivec2(gl_FragCoord.xy);
	// result.rg = nearest seed coordinate
	// result.b  = signed distance to that seed (<0 inside, >0 outside)
	// result.a  = seed depth (forwarded from the seed pass)
	vec4 result = texelFetch(source, curr_coord, 0).rgba;
	float result_sign = sign(result.z);

	for (int x = -1; x <= 1; ++x) {
		for (int y = -1; y <= 1; ++y) {
			ivec2 other_coord;
			vec4 other;
			float dist;

			if (x == 0 && y == 0) {
				continue;
			}

			other_coord = curr_coord + ivec2(x, y) * step;
			if (other_coord.x < 0 || other_coord.x >= size.x || other_coord.y < 0 || other_coord.y >= size.y) {
				continue;
			}

			other = texelFetch(source, other_coord, 0).rgba;
			if (other.z == 0.0) {
				continue;
			}

			if (result_sign != sign(other.z)) {
				// Edge transition: direct neighbor is a better candidate seed.
				dist = length(vec2(curr_coord - other_coord));
				if (dist < result.z * result_sign) {
					result = vec4(other_coord, dist * result_sign, other.a);
				}
			}
			else if (ivec2(other.rg) != other_coord) {
				// Same region: compare against propagated seed coordinate.
				dist = length(vec2(curr_coord - ivec2(other.rg)));
				if (dist < result.z * result_sign) {
					result = vec4(other.rg, dist * result_sign, other.a);
				}
			}
		}
	}

	frag_colour = result;
}
