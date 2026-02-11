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
    vec3 viewDir = normalize(-vWorldPos); // Approximation

    // Diffuse lighting - sharper terminator like the original
    float diff = max(dot(normal, lightDir), 0.0);
    diff = pow(diff, 0.8); // Slightly softer falloff

    // Specular highlight (Blinn-Phong)
    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 32.0) * 0.4;

    // Rim/backlight effect - subtle blue glow on edges facing away from light
    float rim = 1.0 - max(dot(normal, viewDir), 0.0);
    rim = pow(rim, 3.0) * 0.15;
    vec3 rimColor = vec3(0.3, 0.5, 0.8) * rim;

    float ambient = 0.03; // Very dark ambient -- space is dark!

    vec4 texColor = texture(uTexture, vTexCoord);
    vec3 lit = texColor.rgb * uColor * (ambient + diff * 0.97);
    lit += spec * vec3(1.0, 0.98, 0.9); // Warm specular
    lit += rimColor;

    // Add emissive
    lit += uEmissive * uEmissiveStrength;

    FragColor = vec4(lit, texColor.a);
}
