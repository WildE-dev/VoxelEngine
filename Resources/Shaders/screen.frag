#version 330 core

in vec2 texCoord;

uniform sampler2D screenTexture;
uniform float underwater; // 1 when the camera is inside water

out vec4 FragColor;

void main() {
    vec3 color = texture(screenTexture, texCoord).rgb;
    if (underwater > 0.5) {
        // Murky blue-green: absorb reds, then mix toward the water colour.
        color = mix(color * vec3(0.45, 0.75, 0.95), vec3(0.04, 0.18, 0.32), 0.35);
    }
    FragColor = vec4(color, 1.0);
}
