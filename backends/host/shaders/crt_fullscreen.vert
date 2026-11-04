#version 450
// Fullscreen triangle without a vertex buffer; every CRT pass draws three vertices.
void main()
{
    vec2 corner=vec2((gl_VertexIndex&1)*4-1,(gl_VertexIndex&2)*2-1);
    gl_Position=vec4(corner,0.0,1.0);
}
