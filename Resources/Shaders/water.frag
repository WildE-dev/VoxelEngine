#version 330 core

in vec3 WorldPos;
in vec3 FaceNormal;
in float SkyLight;
in float BlockLight;
in vec2 TexCoord;

uniform sampler2D atlas;       // block textures (the water tile)
uniform samplerCube skybox;
uniform sampler2D sceneColor;  // the scene behind the water (copied before this pass)
uniform sampler2D sceneDepth;
uniform vec2 screenSize;       // pixels
uniform float nearPlane;
uniform float farPlane;
uniform vec3 cameraPos;
uniform float time;
uniform float sunBrightness;   // 0 = night, 1 = day
uniform vec3 sunDirection;     // towards the sun
uniform vec3 blockLightColor;

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
uniform float underwater;      // 1 when the camera is inside water

out vec4 FragColor;

// Water absorbs red first, then green: the further light travels through it, the
// bluer and darker it gets (per block of water).
const vec3 kAbsorption = vec3(0.45, 0.14, 0.07);
const vec3 kDeepColor = vec3(0.03, 0.16, 0.28);   // colour scattered back by deep water
const float kF0 = 0.02;
const float kTextureOpacity = 0.45;                           // reflectance looking straight down

float LinearDepth(float depth) {
    float zNdc = depth * 2.0 - 1.0;
    return 2.0 * nearPlane * farPlane / (farPlane + nearPlane - zNdc * (farPlane - nearPlane));
}

// Height field of gentle waves in world space (continuous across cells). Returns the
// slope (dh/dx, dh/dz).
vec2 WaveSlope(vec2 p) {
    vec2 slope = vec2(0.0);
    // A few directional waves with different speeds and sizes.
    const vec3 waves[4] = vec3[4](vec3(0.8, 0.6, 1.3), vec3(-0.5, 0.9, 1.9), vec3(0.95, -0.3, 2.9), vec3(-0.7, -0.7, 4.3));
    const float amps[4] = float[4](0.035, 0.025, 0.014, 0.008);
    for (int i = 0; i < 4; i++) {
        vec2 dir = normalize(waves[i].xy);
        float k = waves[i].z;
        float phase = dot(dir, p) * k + time * (0.8 + 0.35 * float(i));
        slope += dir * (amps[i] * k * cos(phase));
    }
    return slope;
}

void main() {
    vec3 toCamera = cameraPos - WorldPos;
    vec3 V = normalize(toCamera);

    // Surface normal: waves on upward-facing surfaces, a gentler ripple on the falling
    // curtains and sides.
    vec3 n = FaceNormal;
    if (abs(FaceNormal.y) > 0.5) {
        vec2 s = WaveSlope(WorldPos.xz);
        n = normalize(vec3(-s.x, 1.0, -s.y)) * sign(FaceNormal.y);
    } else {
        vec2 s = WaveSlope(vec2(dot(WorldPos.xz, FaceNormal.zx * vec2(1.0, -1.0)), WorldPos.y * 1.7 + time * 2.0));
        n = normalize(FaceNormal + 0.6 * (s.x * vec3(FaceNormal.z, 0.0, -FaceNormal.x) + s.y * vec3(0.0, 1.0, 0.0)));
        // Lean sides towards up: the small steps between water levels then shade like
        // the surface around them instead of reading as cracks, and falling curtains
        // catch the sky rather than mirroring the ground.
        n = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.6));
    }
    // Which side we're on comes from the face itself, not the wave normal: at grazing
    // angles the waves tip the normal away from us, and the surface would then be
    // treated as seen from underwater (and drawn as clear glass).
    bool fromBelow = dot(FaceNormal, V) < 0.0;
    if (fromBelow) n = -n;
    // Keep the wave normal facing us, so a grazing view still gets a proper reflection.
    float facing = dot(n, V);
    if (facing < 0.05) n = normalize(n + V * (0.05 - facing));

    float cosTheta = clamp(dot(n, V), 0.0, 1.0);
    float fresnel = kF0 + (1.0 - kF0) * pow(1.0 - cosTheta, 5.0);
    if (fromBelow) fresnel *= 0.3; // mostly see through the surface from below

    // --- Refraction: the scene behind, bent by the waves ---
    vec2 screenUV = gl_FragCoord.xy / screenSize;
    float waterDepth = LinearDepth(gl_FragCoord.z);
    vec2 offset = n.xz * 0.04 / max(1.0, waterDepth * 0.15);
    vec2 refractUV = clamp(screenUV + offset, vec2(0.001), vec2(0.999));
    // Don't pull in things in front of the water: fall back to the straight view.
    if (LinearDepth(texture(sceneDepth, refractUV).r) < waterDepth) refractUV = screenUV;
    vec3 behind = texture(sceneColor, refractUV).rgb;
    float sceneDistance = LinearDepth(texture(sceneDepth, refractUV).r);
    // Water between this surface and what's behind it. Through a side face that can be
    // far more than the water really holds (a falling curtain with distant land behind
    // it), so it's capped there; the cap stays above the depth of a pool so the small
    // steps between water levels shade like the surface around them.
    // Looking out of the water from below, what's behind is air: the screen tint
    // handles being underwater.
    float thickness = clamp(sceneDistance - waterDepth, 0.0, 60.0);
    if (abs(FaceNormal.y) < 0.5) thickness = min(thickness, 6.0);
    if (fromBelow || underwater > 0.5) thickness = 0.0;

    // Ambient light reaching the water (dark in caves and at night).
    vec3 ambient = vec3(0.04) + vec3(SkyLight * (0.15 + 0.85 * sunBrightness)) + BlockLight * blockLightColor;
    vec3 transmittance = exp(-kAbsorption * thickness);
    vec3 refracted = behind * transmittance + kDeepColor * ambient * (1.0 - transmittance);
    // The water texture over the top, lit like the rest of the world, so water keeps
    // its familiar look; partly see-through so refraction still shows.
    vec3 tile = texture(atlas, TexCoord).rgb * min(ambient, vec3(1.0));
    refracted = mix(refracted, tile, kTextureOpacity);

    // --- Reflection: the sky, dimmed where the sky can't reach and at night ---
    vec3 R = reflect(-V, n);
    // Never reflect below the horizon: the skybox's lower half is ground-coloured, but
    // what a water surface really mirrors there is the far shore and sky.
    R.y = max(R.y, 0.03);
    R = normalize(R);
    vec3 sky = texture(skybox, R).rgb * (0.05 + 0.95 * sunBrightness) * SkyLight;
    vec3 color = mix(refracted, sky, fresnel);

    // --- Sun glint ---
    if (!fromBelow) {
        float glint = pow(max(dot(R, sunDirection), 0.0), 220.0);
        color += vec3(1.0, 0.95, 0.85) * glint * 1.6 * sunBrightness * SkyLight;
    }

    FragColor = vec4(ApplyFog(color, WorldPos), 1.0);
}
