#version 430

#ezquake-definitions

layout(binding = 0) uniform sampler2D normal_texture;

uniform vec4 outline_color;
uniform int outline_width;
uniform float outline_factor;
uniform float outline_factor2;

in vec2 TextureCoord;
out vec4 frag_colour;

void main()
{
	ivec2 coords = ivec2(TextureCoord.x * r_width, TextureCoord.y * r_height);
	vec4 center  = texelFetch(normal_texture, coords, 0);
	vec4 left    = texelFetchOffset(normal_texture, coords, 0, ivec2(-outline_width, 0));
	vec4 right   = texelFetchOffset(normal_texture, coords, 0, ivec2(+outline_width, 0));
	vec4 up      = texelFetchOffset(normal_texture, coords, 0, ivec2(0, -outline_width));
	vec4 down    = texelFetchOffset(normal_texture, coords, 0, ivec2(0, +outline_width));

	bool z_diff  = r_zFar * abs((right.a - center.a) - (center.a - left.a)) > outline_factor2;
	bool z_diff2 = r_zFar * abs(( down.a - center.a) - (center.a - up.a  )) > outline_factor2;

	if (center.a != 0 && (
		(left.a  != 0 && right.a != 0 && z_diff ) ||
		(down.a  != 0 && up.a    != 0 && z_diff2) ||
		(left.a  != 0 && dot(center.rgb, left.rgb ) < outline_factor) ||
		(right.a != 0 && dot(center.rgb, right.rgb) < outline_factor) ||
		(up.a    != 0 && dot(center.rgb, up.rgb   ) < outline_factor) ||
		(down.a  != 0 && dot(center.rgb, down.rgb ) < outline_factor)
		)) {
		frag_colour = outline_color;
	}
	else {
		frag_colour = vec4(0, 0, 0, 0);
	}
}
