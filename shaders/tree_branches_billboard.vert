varying vec4 world_space_pos, eye_space_pos;

void main()
{
	set_tc0_from_vert_id();
	gl_Position     = fg_ftransform();
	world_space_pos = fg_Vertex;
	eye_space_pos   = gl_ModelViewMatrix * fg_Vertex;
	gl_FrontColor   = fg_Color;
} 
