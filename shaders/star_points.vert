#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;

uniform mat4 uView;
uniform mat4 uProjection;

out vec4 vColor;

void main() {
    vec4 viewPos = uView * vec4(aPos, 1.0);
    gl_Position = uProjection * viewPos;
    
    // Point size attenuation
    float dist = length(viewPos.xyz);
    gl_PointSize = aSize * (300.0 / max(dist, 1.0));
    
    vColor = aColor;
}
