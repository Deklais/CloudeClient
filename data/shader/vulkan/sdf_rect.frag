#version 450
#extension GL_ARB_separate_shader_objects : enable

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

layout (location = 0) noperspective in vec2 TexCoord;

layout (location = 0) out vec4 FragClr;

float sdRoundedBox(vec2 LocalPos, vec2 HalfSize, float Radius)
{
	vec2 q = abs(LocalPos) - HalfSize + vec2(Radius);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - Radius;
}

void main()
{
	vec2 RectSize = gSdf.gRectSize.x > 0.0 && gSdf.gRectSize.y > 0.0 ? gSdf.gRectSize : gSdf.u_resolution;
	float Progress = clamp(gSdf.u_progress, 0.0, 1.0);
	vec2 LocalPos = (TexCoord - vec2(0.5)) * RectSize;
	float Radius = clamp(gSdf.gRadius, 0.0, min(RectSize.x, RectSize.y) * 0.5);
	vec2 p = LocalPos;
	vec2 HalfSize = RectSize * 0.5;
	float d = sdRoundedBox(p, HalfSize, Radius);
	float EdgeWidth = max(0.0001, fwidth(d));
	float FillAlpha = 1.0 - smoothstep(0.0, EdgeWidth, d);
	float StrokeAlpha = 0.0;
	if(gSdf.gStrokeWidth > 0.0)
	{
		float StrokeDist = abs(d + gSdf.gStrokeWidth * 0.5) - gSdf.gStrokeWidth * 0.5;
		StrokeAlpha = (1.0 - smoothstep(0.0, EdgeWidth, StrokeDist)) * smoothstep(0.0, EdgeWidth, d + gSdf.gStrokeWidth);
	}
	float GlowAlpha = gSdf.gGlowSize > 0.0 ? 1.0 - smoothstep(0.0, gSdf.gGlowSize, max(d, 0.0)) : 0.0;

	vec4 Color = gSdf.gGlowColor * GlowAlpha;
	if(FillAlpha > 0.0)
	{
		Color = mix(Color, gSdf.gFillColor, FillAlpha * gSdf.gFillColor.a);
	}
	Color = mix(Color, gSdf.gStrokeColor, StrokeAlpha * gSdf.gStrokeColor.a);
	Color.a = max(max(Color.a, FillAlpha * gSdf.gFillColor.a), max(StrokeAlpha * gSdf.gStrokeColor.a, GlowAlpha * gSdf.gGlowColor.a));
	Color.a *= Progress;
	FragClr = Color;
}
