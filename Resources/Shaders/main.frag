#version 330 core

in vec2 TexCoord;
in vec3 Light;

uniform sampler2D ourTexture;

out vec4 FragColor;

void main() {
    vec4 albedo = texture(ourTexture, TexCoord);
    // Cut-out transparency for plant sprites.
    if (albedo.a < 0.5f) discard;
    FragColor = vec4(albedo.rgb * Light, albedo.a);
}