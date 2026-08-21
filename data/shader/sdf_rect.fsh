// Astral UI SDF rounded rectangle fragment shader (.fsh mirror for editing/reference).
// Runtime shader path in this DDNet tree uses .frag; keep this file in sync with sdf_rect.frag.

uniform vec4 gFillColor;
uniform vec4 gStrokeColor;
uniform vec4 gGlowColor;
uniform vec2 gRectSize;
uniform vec2 u_resolution;
uniform float gRadius;
uniform float gStrokeWidth;
uniform float gGlowSize;
uniform float gSoftness;
uniform float u_progress;

noperspective in vec2 TexCoord;

out vec4 FragClr;

float sdRoundedBox(vec2 LocalPos, vec2 HalfSize, float Radius)
{
	vec2 q = abs(LocalPos) - HalfSize + vec2(Radius);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - Radius;
}

void main()
{
	vec2 RectSize = gRectSize.x > 0.0 && gRectSize.y > 0.0 ? gRectSize : u_resolution;
	float Progress = clamp(u_progress, 0.0, 1.0);
	vec2 LocalPos = (TexCoord - vec2(0.5)) * RectSize;
	float Radius = clamp(gRadius, 0.0, min(RectSize.x, RectSize.y) * 0.5);
	vec2 p = LocalPos;
	vec2 HalfSize = RectSize * 0.5;
	float d = sdRoundedBox(p, HalfSize, Radius);
	float EdgeWidth = max(0.0001, fwidth(d));
	float FillAlpha = 1.0 - smoothstep(0.0, EdgeWidth, d);
	float StrokeAlpha = 0.0;
	if(gStrokeWidth > 0.0)
	{
		float StrokeDist = abs(d + gStrokeWidth * 0.5) - gStrokeWidth * 0.5;
		StrokeAlpha = (1.0 - smoothstep(0.0, EdgeWidth, StrokeDist)) * smoothstep(0.0, EdgeWidth, d + gStrokeWidth);
	}
	float GlowAlpha = gGlowSize > 0.0 ? 1.0 - smoothstep(0.0, gGlowSize, max(d, 0.0)) : 0.0;

	vec4 Color = gGlowColor * GlowAlpha;
	if(FillAlpha > 0.0)
	{
		Color = mix(Color, gFillColor, FillAlpha * gFillColor.a);
	}
	Color = mix(Color, gStrokeColor, StrokeAlpha * gStrokeColor.a);
	Color.a = max(max(Color.a, FillAlpha * gFillColor.a), max(StrokeAlpha * gStrokeColor.a, GlowAlpha * gGlowColor.a));
	Color.a *= Progress;
	FragClr = Color;
}
