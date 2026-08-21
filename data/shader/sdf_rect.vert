layout (location = 0) in vec4 inVertex;
layout (location = 1) in vec4 inColor;
layout (location = 2) in vec2 inVertexTexCoord;

uniform mat4x2 gPos;

noperspective out vec2 TexCoord;

void main()
{
	gl_Position = vec4(gPos * vec4(inVertex.xy, 0.0, 1.0), 0.0, 1.0);
	TexCoord = inVertexTexCoord;
}
