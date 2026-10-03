#version 450
layout(push_constant) uniform Constantes { float t; } pc;
layout(location = 0) out vec3 color;
void main() {
  vec2 p[3] = vec2[](vec2(0.0, -0.7), vec2(0.7, 0.6), vec2(-0.7, 0.6));
  vec3 c[3] = vec3[](vec3(1.0, 0.1, 0.1), vec3(0.1, 1.0, 0.1), vec3(0.2, 0.3, 1.0));
  float a = pc.t;
  mat2 r = mat2(cos(a), sin(a), -sin(a), cos(a));
  gl_Position = vec4(r * p[gl_VertexIndex] * vec2(0.5, 1.0), 0.0, 1.0);
  color = c[gl_VertexIndex];
}
