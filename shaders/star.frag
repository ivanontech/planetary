#version 330 core

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform vec3 uColor;
uniform vec3 uEmissive;
uniform float uEmissiveStrength;
uniform float uTime;

// 3D noise for turbulent plasma
float hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i), hash(i+vec3(1,0,0)), f.x),
                   mix(hash(i+vec3(0,1,0)), hash(i+vec3(1,1,0)), f.x), f.y),
               mix(mix(hash(i+vec3(0,0,1)), hash(i+vec3(1,0,1)), f.x),
                   mix(hash(i+vec3(0,1,1)), hash(i+vec3(1,1,1)), f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 6; i++) {
        v += a * noise(p);
        p = p * 2.1 + vec3(0.1);
        a *= 0.5;
    }
    return v;
}

out vec4 FragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 viewDir = normalize(-vWorldPos);

    // Turbulent plasma on sphere surface - animated
    vec3 sPos = N * 4.0;
    float t1 = fbm(sPos + vec3(uTime * 0.08, uTime * 0.05, uTime * 0.03));
    float t2 = fbm(sPos * 1.5 + vec3(-uTime * 0.06, uTime * 0.1, uTime * 0.04));
    float turb = t1 * 0.6 + t2 * 0.4;

    // Rim: edge vs center
    float rim = 1.0 - max(dot(N, viewDir), 0.0);

    // Color palette: hot white core -> yellow -> orange -> deep red edges
    vec3 white = vec3(1.0, 1.0, 0.97);
    vec3 yellow = vec3(1.0, 0.9, 0.5);
    vec3 orange = vec3(1.0, 0.55, 0.15);
    vec3 deepRed = vec3(0.7, 0.15, 0.03);

    // Blend based on turbulence + rim (edges darker/redder)
    float edgeFactor = rim * 0.6 + (1.0 - turb) * 0.4;
    vec3 plasma;
    if (edgeFactor < 0.25) plasma = mix(white, yellow, edgeFactor / 0.25);
    else if (edgeFactor < 0.5) plasma = mix(yellow, orange, (edgeFactor - 0.25) / 0.25);
    else if (edgeFactor < 0.75) plasma = mix(orange, deepRed, (edgeFactor - 0.5) / 0.25);
    else plasma = mix(deepRed, deepRed * 0.5, (edgeFactor - 0.75) / 0.25);

    // Granulation: bright spots where turbulence is high (convection cells)
    float granules = smoothstep(0.45, 0.65, turb);
    plasma = mix(plasma, plasma * 1.3 + vec3(0.1, 0.08, 0.02), granules * 0.4);

    // Sunspots: dark patches
    float spots = smoothstep(0.7, 0.75, fbm(N * 8.0 + vec3(uTime * 0.02)));
    plasma *= mix(1.0, 0.3, spots * (1.0 - rim));

    // Artist color tint
    plasma = mix(plasma, plasma * uColor, 0.2);

    // FULLY SELF-LUMINOUS: no dark side! Stars glow uniformly.
    float brightness = 0.85 + turb * 0.15; // slight variation from turbulence
    plasma *= brightness;

    // Audio-reactive emissive boost
    plasma += uEmissive * uEmissiveStrength;

    // Slight limb brightening for corona feel (opposite of darkening)
    plasma += vec3(0.15, 0.1, 0.05) * pow(rim, 2.0) * (1.0 - spots);

    FragColor = vec4(plasma, 1.0);
}
