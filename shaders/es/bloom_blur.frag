#version 300 es
precision mediump float;
in vec2 vTexCoord;
uniform sampler2D uImage;
uniform bool uHorizontal;
uniform float uTexelSize;
out vec4 FragColor;

// 9-tap Gaussian blur
const float weight[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    vec2 offset = uHorizontal ? vec2(uTexelSize, 0.0) : vec2(0.0, uTexelSize);
    vec3 result = texture(uImage, vTexCoord).rgb * weight[0];
    for (int i = 1; i < 5; i++) {
        result += texture(uImage, vTexCoord + offset * float(i)).rgb * weight[i];
        result += texture(uImage, vTexCoord - offset * float(i)).rgb * weight[i];
    }
    FragColor = vec4(result, 1.0);
}