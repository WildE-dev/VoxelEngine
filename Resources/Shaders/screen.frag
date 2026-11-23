#version 330 core

in vec2 texCoord;

uniform sampler2D screenTexture;
uniform float underwater; // 1 when the camera is inside water
uniform float steamFog;   // 0..1: how thick the steam the camera is in is
uniform float fogLight;   // how bright the steam looks (darker at night)

out vec4 FragColor;

void main() {
    vec3 color = texture(screenTexture, texCoord).rgb;
    if (underwater > 0.5) {
        // Murky blue-green: absorb reds, then mix toward the water colour.
        color = mix(color * vec3(0.45, 0.75, 0.95), vec3(0.04, 0.18, 0.32), 0.35);
    }
    // In steam: a white-out, thicker the thicker it is, a little brighter toward the middle.
    if (steamFog > 0.0) {
        float edge = length(texCoord - 0.5);
        color = mix(color, vec3(0.92, 0.94, 0.96) * fogLight, clamp(steamFog * (1.05 - 0.4 * edge), 0.0, 0.92));
    }
    FragColor = vec4(color, 1.0);
}
