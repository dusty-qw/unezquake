#version 120

#ezquake-definitions

uniform sampler2D materialSampler;
uniform float alphaThreshold;
uniform vec4 sprayTint;

varying vec2 TextureCoord;
varying vec4 fsColor;

void main()
{
	gl_FragColor = texture2D(materialSampler, TextureCoord) * fsColor;

	if (sprayTint.a > 0.0 && gl_FragColor.a > 0.0) {
		vec3 unpremultiplied = gl_FragColor.rgb / gl_FragColor.a;
		float luminance = dot(unpremultiplied, vec3(0.299, 0.587, 0.114));
		gl_FragColor.rgb = luminance * sprayTint.rgb * gl_FragColor.a;
	}

#ifdef DRAW_FOG
	gl_FragColor = applyFogBlend(gl_FragColor, fogFragDepth());
#endif
}
