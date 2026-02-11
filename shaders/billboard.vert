#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aSize;

uniform mat4 uView;
uniform mat4 uProjection;
uniform vec2 uScreenSize;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    // Transform to view space
    vec4 viewPos = uView * vec4(aPos, 1.0);
    
    // Billboard offset in screen space
    vec2 offset = (aTexCoord - 0.5) * 2.0 * aSize;
    viewPos.xy += offset;
    
    gl_Position = uProjection * viewPos;
    vTexCoord = aTexCoord;
    vColor = aColor;
}
