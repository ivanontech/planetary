#version 330 core

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vTexCoord;
in vec3 vLocalPos;

uniform sampler2D uTexture;
uniform vec3 uColor;
uniform vec3 uEmissive;
uniform float uEmissiveStrength;
uniform float uTime;
uniform float uAudioLevel;

out vec4 FragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 viewDir = normalize(-vWorldPos);
    float rim = 1.0 - max(dot(N, viewDir), 0.0);

    // === SMOOTH BRIGHT SPHERE ===
    // The original Planetary star is a smooth, bright, evenly-lit ball.
    // Artist color tints it but the core is very bright.

    // Core: bright white with artist color tint (40% tint, 60% white)
    vec3 core = uColor * 0.4 + vec3(0.6, 0.58, 0.55);

    // Very slight limb darkening (warm at edges)
    vec3 warmEdge = uColor * 0.6 + vec3(0.35, 0.3, 0.25);
    vec3 surface = mix(core, warmEdge, pow(rim, 2.5) * 0.3);

    // Self-luminous - uniformly bright, no dark side
    float audioPulse = 1.0 + uAudioLevel * 0.1;
    surface *= 0.95 * audioPulse;

    // Emissive boost
    surface += uEmissive * uEmissiveStrength * 0.2;

    FragColor = vec4(surface, 1.0);
}
