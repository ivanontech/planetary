#version 300 es
precision mediump float;

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
    vec3 viewDir = normalize(-vWorldPos);

    // Diffuse -- slightly softer falloff for realistic planet look
    float diff = max(dot(normal, lightDir), 0.0);
    float diffSmooth = smoothstep(-0.1, 1.0, diff); // softer terminator

    // Blinn-Phong specular -- strong highlight for that wet/glossy look
    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 48.0) * 0.6;

    // Rim/backlight -- cyan-blue edge glow (the iconic Planetary look)
    float rim = 1.0 - max(dot(normal, viewDir), 0.0);
    rim = pow(rim, 2.5);
    float rimLight = max(dot(normal, -lightDir), 0.0) * 0.3; // backlit side only
    vec3 rimColor = vec3(0.2, 0.5, 0.9) * rim * 0.2;
    // Stronger rim on backlit edge
    rimColor += vec3(0.15, 0.4, 0.8) * rim * rimLight * 0.5;

    // Very dark ambient -- space is black
    float ambient = 0.02;

    vec4 texColor = texture(uTexture, vTexCoord);
    vec3 lit = texColor.rgb * uColor * (ambient + diffSmooth * 0.98);

    // Warm specular highlight
    lit += spec * vec3(1.0, 0.97, 0.92) * diff;

    // Rim glow
    lit += rimColor;

    // Emissive
    lit += uEmissive * uEmissiveStrength;

    // Subtle ambient occlusion at terminator
    float ao = smoothstep(0.0, 0.15, diff);
    lit *= mix(0.7, 1.0, ao);

    FragColor = vec4(lit, texColor.a);
}