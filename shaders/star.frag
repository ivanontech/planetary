#version 330 core

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform vec3 uColor;
uniform vec3 uEmissive;
uniform float uEmissiveStrength;
uniform float uTime;

// Simplex-like noise for procedural turbulence
float hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    
    float a = hash(i);
    float b = hash(i + vec3(1,0,0));
    float c = hash(i + vec3(0,1,0));
    float d = hash(i + vec3(1,1,0));
    float e = hash(i + vec3(0,0,1));
    float f1 = hash(i + vec3(1,0,1));
    float g = hash(i + vec3(0,1,1));
    float h = hash(i + vec3(1,1,1));
    
    return mix(mix(mix(a,b,f.x), mix(c,d,f.x), f.y),
               mix(mix(e,f1,f.x), mix(g,h,f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float val = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++) {
        val += amp * noise(p);
        p *= 2.1;
        amp *= 0.5;
    }
    return val;
}

out vec4 FragColor;

void main() {
    vec3 normal = normalize(vNormal);
    
    // Procedural turbulence on sphere surface
    vec3 spherePos = normal * 3.0 + vec3(uTime * 0.1, uTime * 0.05, 0.0);
    float turb = fbm(spherePos);
    float turb2 = fbm(spherePos * 2.0 + vec3(0, 0, uTime * 0.15));
    
    // Hot plasma colors: white center -> yellow -> orange -> dark red at edges
    float rim = 1.0 - max(dot(normal, normalize(-vWorldPos)), 0.0);
    float heat = turb * 0.7 + 0.3;
    
    // Color gradient based on turbulence and rim
    vec3 hotWhite = vec3(1.0, 1.0, 0.95);
    vec3 yellow = vec3(1.0, 0.85, 0.4);
    vec3 orange = vec3(1.0, 0.5, 0.15);
    vec3 darkRed = vec3(0.6, 0.1, 0.02);
    
    vec3 starSurface;
    float t = turb2 * 0.5 + rim * 0.5;
    if (t < 0.3) starSurface = mix(hotWhite, yellow, t / 0.3);
    else if (t < 0.6) starSurface = mix(yellow, orange, (t - 0.3) / 0.3);
    else starSurface = mix(orange, darkRed, (t - 0.6) / 0.4);
    
    // Mix with artist color
    starSurface = mix(starSurface, uColor, 0.15);
    
    // Bright emissive -- stars glow from within
    float emissive = uEmissiveStrength * (0.8 + turb * 0.4);
    starSurface += uEmissive * emissive;
    
    // Limb darkening (edges slightly darker like real stars)
    float limbDark = pow(1.0 - rim, 0.3);
    starSurface *= limbDark * 0.4 + 0.6;
    
    FragColor = vec4(starSurface, 1.0);
}
