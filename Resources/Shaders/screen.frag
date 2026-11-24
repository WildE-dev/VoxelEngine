#version 330 core

in vec2 texCoord;

uniform sampler2D screenTexture;
uniform float underwater; // 1 when the camera is inside water
uniform float steamFog;   // 0..1: how thick the steam the camera is in is
uniform float fogLight;   // how bright the steam looks (darker at night)
uniform float burning;    // 0..1: on fire
uniform float time;       // seconds

out vec4 FragColor;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
// Flames rising: layered noise scrolling up.
float flames(vec2 uv) {
    vec2 p = vec2(uv.x * 7.0, uv.y * 4.0 - time * 2.6);
    return 0.6 * noise(p) + 0.3 * noise(p * 2.1 + 3.7) + 0.1 * noise(p * 4.3 - 1.3);
}

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
    // On fire: tongues of flame lick up from the bottom and in from the sides, flickering.
    if (burning > 0.0) {
        float fromBottom = 1.0 - texCoord.y;
        float fromSide = abs(texCoord.x - 0.5) * 2.0;
        float reach = max(pow(fromBottom, 1.6) * 1.5, pow(fromSide, 4.0) * 1.1 * (0.3 + fromBottom));
        float v = flames(texCoord) * reach * burning;
        float mask = smoothstep(0.32, 0.5, v);
        vec3 flame = mix(vec3(0.85, 0.12, 0.0), vec3(1.0, 0.55, 0.05), smoothstep(0.45, 0.7, v));
        flame = mix(flame, vec3(1.0, 0.92, 0.55), smoothstep(0.75, 1.0, v));
        color = mix(color, flame, mask * 0.9);
        color += vec3(0.12, 0.03, 0.0) * burning * (0.7 + 0.3 * sin(time * 17.0)) * 0.5; // (a warm flicker over it all)
    }
    FragColor = vec4(color, 1.0);
}
