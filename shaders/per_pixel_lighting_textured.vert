varying vec3 normal;
varying vec4 epos, vertex;

void main()
{
	gl_TexCoord[0] = gl_MultiTexCoord0;
	vec3 n = gl_NormalMatrix * gl_Normal;
	normal = (no_normalize ? n : normalize(n));
	vertex = gl_Vertex;
	epos   = gl_ModelViewMatrix * vertex;
	gl_Position = ftransform();
	set_fog();
}
