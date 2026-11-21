#version 330 core
out vec4 FragColor;

in vec3 TexCoords;

uniform samplerCube skybox;
uniform float brightness;
uniform vec3 tint;      // warm near sunrise and sunset
uniform float overcast; // 0..1: grey while it rains
uniform vec3 sunDirection; // toward the sun (the moon is opposite)
uniform float night;       // 0 by day .. 1 in the dead of night: the stars come out
uniform float time;

// A hash of a cell of the sky, for stars.
float Hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main()
{
    vec3 dir = normalize(TexCoords);
    vec3 c = texture(skybox, TexCoords).rgb;
    float grey = dot(c, vec3(0.3, 0.59, 0.11));
    c = mix(c, vec3(grey * 0.75), overcast);
    c *= tint * brightness;

    float clear = 1.0 - overcast; // clouds hide the sun, moon and stars
    vec3 sun = normalize(sunDirection);

    // Stars: a few cells of a fine grid over the sky light up, twinkling, above the
    // horizon, as night falls.
    if (night > 0.01 && dir.y > -0.05) {
        vec3 cell = floor(dir * 220.0);
        float h = Hash(cell);
        if (h > 0.9965) {
            vec3 centre = normalize((cell + 0.5) / 220.0);
            float d = 1.0 - dot(dir, centre);
            float star = smoothstep(2.2e-5, 0.0, d);
            float twinkle = 0.7 + 0.3 * sin(time * (2.0 + h * 40.0) + h * 100.0);
            c += vec3(0.9, 0.93, 1.0) * star * twinkle * night * clear * smoothstep(-0.05, 0.15, dir.y);
        }
    }

    // The sun: a bright disc with a glow; the moon opposite, pale, with a few marks.
    float toSun = dot(dir, sun);
    c += vec3(1.0, 0.92, 0.75) * (smoothstep(0.99955, 0.9997, toSun) * 3.0 + pow(max(toSun, 0.0), 300.0) * 0.5) * clear * (1.0 - night * 0.9);
    float toMoon = dot(dir, -sun);
    if (toMoon > 0.9990) {
        float disc = smoothstep(0.99935, 0.99955, toMoon);
        float marks = Hash(floor(dir * 2600.0)) > 0.6 ? 0.82 : 1.0;
        c = mix(c, vec3(0.86, 0.88, 0.95) * marks, disc * clear * night);
    }
    c += vec3(0.5, 0.55, 0.7) * pow(max(toMoon, 0.0), 400.0) * 0.25 * clear * night;
    FragColor = vec4(c, 1.0);
}
