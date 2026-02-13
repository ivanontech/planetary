#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform float uTime;
uniform float uAudioLevel;

out vec3 vNormal;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec3 vLocalPos;

void main() {
    // SMOOTH sphere - no displacement. The "flame" look comes from
    // the billboard glow halo, not the sphere surface.
    vec3 pos = aPos;

    // Audio breathing - gentle size pulse with the beat
    pos *= 1.0 + uAudioLevel * 0.05;

    vLocalPos = pos;
    vec4 worldPos = uModel * vec4(pos, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
