#version 330 core

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform vec3 uLightPos;
uniform vec3 uColor;
uniform vec3 uEmissive;
uniform float uEmissiveStrength;

out vec4 FragColor;

void main() {
    vec3 normal = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos - vWorldPos);
    
    // Diffuse lighting
    float diff = max(dot(normal, lightDir), 0.0);
    float ambient = 0.15;
    
    vec4 texColor = texture(uTexture, vTexCoord);
    vec3 lit = texColor.rgb * uColor * (ambient + diff * 0.85);
    
    // Add emissive glow
    lit += uEmissive * uEmissiveStrength;
    
    FragColor = vec4(lit, texColor.a);
}
