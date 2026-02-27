#version 300 es
precision mediump float;

in vec2 vTexCoord;

uniform sampler2D uScene;
uniform vec2 uStarScreenPos;  // Star position in UV space (0-1)
uniform float uBassLevel;     // 0-1 bass intensity
uniform float uTime;
uniform float uActive;         // 0.0 or 1.0

out vec4 FragColor;

void main() {
    vec2 uv = vTexCoord;

    if (uActive < 0.5) {
        FragColor = texture(uScene, uv);
        return;
    }

    vec2 delta = uv - uStarScreenPos;
    float dist = length(delta);

    // Gentle expanding ripple waves - moderate pulse, not shake
    float ripple = 0.0;
    for (int i = 0; i < 2; i++) {
        float phase = uTime * 0.8 + float(i) * 3.14;
        float wavePos = fract(phase * 0.2) * 0.4;
        float waveDist = abs(dist - wavePos);
        float wave = exp(-waveDist * 80.0) * (1.0 - fract(phase * 0.2));
        ripple += wave;
    }

    // GENTLE distortion - moderate pulse, not shake
    float strength = uBassLevel * 0.003 * ripple;

    // Smooth falloff
    float falloff = smoothstep(0.03, 0.08, dist) * smoothstep(0.5, 0.2, dist);
    strength *= falloff;

    // Distort UV along radial direction
    vec2 dir = (dist > 0.001) ? normalize(delta) : vec2(0.0);
    vec2 distortedUV = uv + dir * strength;
    distortedUV = clamp(distortedUV, vec2(0.002), vec2(0.998));

    vec3 color = texture(uScene, distortedUV).rgb;

    // Very subtle blue glow at ripple peaks
    float glowIntensity = ripple * uBassLevel * falloff * 0.008;
    color += vec3(0.08, 0.2, 0.5) * glowIntensity;

    FragColor = vec4(color, 1.0);
}