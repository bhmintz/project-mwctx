#version 450
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 salida;
void main() { salida = vec4(color, 1.0); }
