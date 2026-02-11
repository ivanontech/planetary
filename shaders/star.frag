#version 330 core
in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vTexCoord;
uniform sampler2D uTexture;
uniform vec3 uColor;
uniform vec3 uEmissive;
uniform float uEmissiveStrength;
uniform float uTime;

float hash(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}
float noise(vec3 p) {
    vec3 i = floor(p); vec3 f = fract(p);
    f = f*f*(3.0-2.0*f);
    return mix(mix(mix(hash(i),hash(i+vec3(1,0,0)),f.x),
                   mix(hash(i+vec3(0,1,0)),hash(i+vec3(1,1,0)),f.x),f.y),
               mix(mix(hash(i+vec3(0,0,1)),hash(i+vec3(1,0,1)),f.x),
                   mix(hash(i+vec3(0,1,1)),hash(i+vec3(1,1,1)),f.x),f.y),f.z);
}
float fbm(vec3 p) {
    float v=0.0, a=0.5;
    for(int i=0;i<6;i++){v+=a*noise(p);p=p*2.1+0.1;a*=0.5;}
    return v;
}
out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(-vWorldPos);
    // Animated turbulence
    vec3 sP = N * 4.0;
    float t1 = fbm(sP + vec3(uTime*0.08, uTime*0.05, uTime*0.03));
    float t2 = fbm(sP*1.5 + vec3(-uTime*0.06, uTime*0.1, uTime*0.04));
    float turb = t1*0.6 + t2*0.4;
    float rim = 1.0 - max(dot(N,V), 0.0);
    
    // UNIQUE color palette per star based on uColor
    // Hot center uses white, mid uses star color, edges use darker complement
    vec3 hotCore = vec3(1.0, 1.0, 0.95);
    vec3 midColor = mix(uColor, vec3(1.0, 0.85, 0.5), 0.3); // Star's unique color
    vec3 edgeColor = uColor * 0.7; // Darker version of star color
    vec3 deepEdge = uColor * 0.3 + vec3(0.1, 0.02, 0.0); // Very dark edge
    
    float ef = rim * 0.5 + (1.0-turb) * 0.5;
    vec3 plasma;
    if(ef<0.2) plasma = mix(hotCore, midColor, ef/0.2);
    else if(ef<0.45) plasma = mix(midColor, edgeColor, (ef-0.2)/0.25);
    else if(ef<0.7) plasma = mix(edgeColor, deepEdge, (ef-0.45)/0.25);
    else plasma = deepEdge * (1.0 - (ef-0.7)/0.3 * 0.5);
    
    // Convection granules
    float gran = smoothstep(0.45,0.65,turb);
    plasma = mix(plasma, plasma*1.3+vec3(0.08,0.05,0.02), gran*0.4);
    // Sunspots
    float spots = smoothstep(0.72,0.76,fbm(N*8.0+vec3(uTime*0.02)));
    plasma *= mix(1.0, 0.25, spots*(1.0-rim));
    // Fully self-luminous
    plasma *= 0.85 + turb*0.15;
    // Audio emissive
    plasma += uEmissive * uEmissiveStrength;
    // Limb brightening
    plasma += midColor * 0.12 * pow(rim, 2.0) * (1.0-spots);
    
    FragColor = vec4(plasma, 1.0);
}
