#version 330 core
// A cloud, not glass: the mist of each face broken up by soft noise drifting upward
// through it, and white where the light is (greyer at night).
in vec3 World;
in float Alpha;

uniform float time;
uniform float brightness;

out vec4 FragColor;

float Hash(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float Noise(vec3 p)
{
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash(i), Hash(i + vec3(1, 0, 0)), f.x), mix(Hash(i + vec3(0, 1, 0)), Hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash(i + vec3(0, 0, 1)), Hash(i + vec3(1, 0, 1)), f.x), mix(Hash(i + vec3(0, 1, 1)), Hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

void main()
{
    vec3 p = World * 1.3 + vec3(0.2, -0.6, 0.1) * time;
    float n = 0.6 * Noise(p) + 0.4 * Noise(p * 2.3 + 7.0);
    float a = Alpha * (0.35 + 1.1 * n);
    if (a < 0.01) discard;
    vec3 color = mix(vec3(0.42, 0.45, 0.5), vec3(0.97, 0.98, 1.0), brightness);
    FragColor = vec4(color, clamp(a, 0.0, 0.85));
}
