#version 450
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec4 inVertex;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inVertexTexCoord;

layout(push_constant) uniform SSdfRectBO {
	layout(offset = 0) mat4x2 gPos;
	layout(offset = 32) vec4 gFillColor;
	layout(offset = 48) vec4 gStrokeColor;
	layout(offset = 64) vec4 gGlowColor;
	layout(offset = 80) vec2 gRectSize;
	layout(offset = 88) float gRadius;
	layout(offset = 92) float gStrokeWidth;
	layout(offset = 96) float gGlowSize;
	layout(offset = 100) float gSoftness;
	layout(offset = 104) vec2 u_resolution;
	layout(offset = 112) float u_progress;
} gSdf;

layout (location = 0) noperspective out vec2 TexCoord;

void main()
{
	gl_Position = vec4(gSdf.gPos * vec4(inVertex.xy, 0.0, 1.0), 0.0, 1.0);
	TexCoord = inVertexTexCoord;
}
