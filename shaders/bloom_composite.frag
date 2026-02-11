#version 330 core
in vec2 vTexCoord;
uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomStrength;
out vec4 FragColor;

void main() {
    vec3 scene = texture(uScene, vTexCoord).rgb;
    vec3 bloom = texture(uBloom, vTexCoord).rgb;
    // Additive bloom
    vec3 result = scene + bloom * uBloomStrength;
    // Simple tone mapping
    result = result / (result + vec3(1.0));
    // Gamma correction
    result = pow(result, vec3(1.0 / 2.2));
    FragColor = vec4(result, 1.0);
}
