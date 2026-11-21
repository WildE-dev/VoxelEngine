#version 330 core

in vec3 normal;
in vec3 WorldPos;
in vec2 faceUV;

// A held block or item: the box shows an atlas tile (top-left, bottom-right in
// tileRect) on each face, see-through where the tile is.
uniform float textured;
uniform vec4 tileRect;
uniform sampler2D atlas;
uniform vec3 boxRepeat; // (tiled: see player.vert)
uniform float highlight; // the part picked in the model editor: flashes toward white
out vec4 FragColor;

uniform vec3 cameraPos;
uniform float glow; // 1: lit by itself (glowing eyes), 0: by the light around it
uniform vec2 lightLevels;      // sky and block light where the figure stands (0..15)
uniform vec3 blockLightColor;  // as main.vert

// Distance fog (set by Renderer::SetFogUniforms): towards the edge of the render
// distance, things fade into the sky behind them, so the world's edge and chunks
// still loading there blend into the sky instead of ending sharply.
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

uniform vec3 color;
uniform vec3 sunDirection;
uniform float sunBrightness;

void main() {
    vec3 albedo = color;
    if (textured > 0.5) {
        vec2 uv = boxRepeat.x > 0.0 ? fract(faceUV) : faceUV;
        vec4 texel = texture(atlas, mix(tileRect.xy, tileRect.zw, clamp(uv, 0.02, 0.98)));
        if (texel.a < 0.5) discard;
        albedo *= texel.rgb;
    }
    // Lit like the world around it (main.vert): sky light where the sky reaches, by
    // day, shaded toward the sun so the boxes read as solid; block light from lamps
    // and the like; and almost nothing in the dark (a cave at night is black).
    float sunSide = max(dot(normalize(normal), normalize(sunDirection)), 0.0);
    float sky = lightLevels.x > 0.0 ? pow(0.85, 15.0 - lightLevels.x) : 0.0;
    float block = lightLevels.y > 0.0 ? pow(0.85, 15.0 - lightLevels.y) : 0.0;
    float shade = 0.45 + 0.55 * sunSide;
    vec3 light = vec3(0.04) + vec3(sky * sunBrightness * shade) + block * blockLightColor * (0.7 + 0.3 * shade);
    light = mix(min(light, vec3(1.0)), vec3(1.0), glow);
    vec3 lit = mix(albedo * light, vec3(1.0, 0.95, 0.4), highlight);
    FragColor = vec4(ApplyFog(lit, WorldPos), 1.0);
}
