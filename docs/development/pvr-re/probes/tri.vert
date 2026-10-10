#version 450
/* A small triangle near the centre: corners of the target stay outside it. */
vec2 P[3] = vec2[](vec2(-0.5,-0.5), vec2(0.5,-0.5), vec2(0.0,0.5));
void main() { gl_Position = vec4(P[gl_VertexIndex], 0.0, 1.0); }
