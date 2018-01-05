#version 430

layout(std140) uniform RefdefCvars {
	mat4 modelViewMatrix;
	mat4 projectionMatrix;
	float time;
	float gamma3d;

	// if enabled, texture coordinates are always 0,0
	int r_textureless;
};

layout(binding=0) uniform sampler2D materialTex;

in vec2 TextureCoord;
in vec4 fragColour;
out vec4 frag_colour;

void main()
{
	vec4 texColor = texture(materialTex, TextureCoord);

	frag_colour = texColor * fragColour;

	frag_colour = vec4(pow(frag_colour.rgb, vec3(gamma3d)), frag_colour.a);
}