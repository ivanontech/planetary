#version 300 es
precision mediump float;

in vec4 vColor;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    // Point sprite UV
    vec2 uv = gl_PointCoord;
    vec4 texColor = texture(uTexture, uv);
    FragColor = texColor * vColor;
}