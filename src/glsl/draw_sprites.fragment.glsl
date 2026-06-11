#ezquake-definitions

EZ_LAYOUT_BINDING(0) uniform sampler2DArray materialTex;
uniform bool alpha_test;
uniform vec4 spray_tint;

in vec3 TextureCoord;
in vec4 fsColor;

out vec4 frag_color;

void main()
{
	frag_color = texture(materialTex, TextureCoord);

	frag_color *= fsColor;

	if (spray_tint.a > 0 && frag_color.a > 0) {
		vec3 unpremultiplied = frag_color.rgb / frag_color.a;
		float luminance = dot(unpremultiplied, vec3(0.299, 0.587, 0.114));
		frag_color.rgb = luminance * spray_tint.rgb * frag_color.a;
	}

	if (alpha_test && frag_color.a < 0.3) {
		discard;
	}

#ifdef DRAW_FOG
	frag_color = applyFogBlend(frag_color, fogFragDepth());
#endif
}
