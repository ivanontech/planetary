#version 330 core

in vec2 vTexCoord;
in vec3 vWorldPos;

uniform vec3 uColor;
uniform vec3 uLightPos;
uniform float uAlpha;
uniform float uTime;

out vec4 FragColor;

// Simple hash for procedural band detail
float hash1(float p) {
    return fract(sin(p * 127.1) * 43758.5453);
}

void main() {
    // Radial coordinate (0 = inner edge, 1 = outer edge)
    float r = vTexCoord.x;

    // Edge fade - transparent at inner and outer edges
    float bands = smoothstep(0.0, 0.05, r) * smoothstep(1.0, 0.9, r);

    // Multiple concentric ring bands (Cassini-division style)
    float ringPattern = 1.0;
    ringPattern *= sin(r * 55.0) * 0.15 + 0.85;
    ringPattern *= sin(r * 22.0 + 0.8) * 0.12 + 0.88;
    ringPattern *= sin(r * 11.0 + 2.0) * 0.08 + 0.92;

    // Cassini division (gap in the middle of the rings)
    float cassini = smoothstep(0.44, 0.47, r) * smoothstep(0.56, 0.53, r);
    ringPattern *= 1.0 - cassini * 0.7;

    // Dense inner ring, sparser outer ring
    float density = mix(0.8, 0.4, r);
    ringPattern *= density;

    bands *= ringPattern;

    // Simple lighting from star
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 lightDir = normalize(uLightPos - vWorldPos);
    float light = 0.35 + 0.65 * abs(dot(up, lightDir));

    // Subtle color variation (icy inner to warm outer)
    vec3 innerColor = uColor * vec3(0.95, 0.9, 0.8);
    vec3 outerColor = uColor * vec3(0.75, 0.8, 0.9);
    vec3 ringColor = mix(innerColor, outerColor, r) * light;

    // Sparkle - tiny bright points that shimmer
    float sparkle = hash1(floor(r * 200.0) + floor(vTexCoord.y * 200.0 + uTime * 0.5));
    ringColor += vec3(0.25) * step(0.97, sparkle) * bands;

    float alpha = uAlpha * bands;

    FragColor = vec4(ringColor, alpha);
}
