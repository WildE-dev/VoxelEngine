#version 330 core

in vec2 TexCoord;
in vec3 Light;
in vec3 WorldPos;

uniform sampler2D ourTexture;

// Distance fog (set by Renderer::SetFogUniforms): towards the edge of the render
// distance, things fade into the sky behind them, so the world's edge and chunks
// still loading there blend into the sky instead of ending sharply.
uniform vec3 cameraPos;
uniform float fogStart;      // horizontal distance where the fog begins
uniform float fogEnd;        // where everything is sky (0: no fog)
uniform samplerCube fogSky;  // the skybox, coloured as skybox.frag does
uniform float skyBrightness;
uniform vec3 skyTint;
uniform float skyOvercast;

vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (fogEnd <= 0.0) return color;
    float amount = smoothstep(fogStart, fogEnd, length(worldPos.xz - cameraPos.xz));
    vec3 c = texture(fogSky, normalize(worldPos - cameraPos)).rgb;
    float grey = dot(c, vec3(0.3, 0.59, 0.11));
    c = mix(c, vec3(grey * 0.75), skyOvercast) * skyTint * skyBrightness;
    return mix(color, c, amount);
}

out vec4 FragColor;

void main() {
    vec4 albedo = texture(ourTexture, TexCoord);
    // Cut-out transparency for plant sprites.
    if (albedo.a < 0.5f) discard;
    FragColor = vec4(ApplyFog(albedo.rgb * Light, WorldPos), albedo.a);
}