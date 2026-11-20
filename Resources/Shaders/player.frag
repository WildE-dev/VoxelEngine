#version 330 core

in vec3 normal;
out vec4 FragColor;

uniform vec3 color;
uniform vec3 sunDirection;
uniform float sunBrightness;

void main() {
    // Simple directional light plus ambient, so the boxes read as solid in the world.
    float sun = max(dot(normalize(normal), normalize(sunDirection)), 0.0);
    float light = 0.35 + 0.65 * mix(0.25, 1.0, sunBrightness) * (0.4 + 0.6 * sun);
    FragColor = vec4(color * light, 1.0);
}
