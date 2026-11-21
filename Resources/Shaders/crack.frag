#version 330 core
// Cracks spreading over the block being mined: the edges of a jittered cell pattern,
// in block-texture-sized pixels, showing out to a radius that grows with progress.
in vec3 Local;
out vec4 FragColor;

uniform vec3 boxSize;
uniform float progress; // 0..1
uniform float brightness;

float Hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

void main() {
    // Which face: the axis we're nearest an end of.
    vec3 edge = min(Local, 1.0 - Local) * boxSize;
    int axis = edge.x < edge.y ? (edge.x < edge.z ? 0 : 2) : (edge.y < edge.z ? 1 : 2);
    vec2 uv = axis == 0 ? vec2(Local.z * boxSize.z, Local.y * boxSize.y)
            : axis == 1 ? vec2(Local.x * boxSize.x, Local.z * boxSize.z)
                        : vec2(Local.x * boxSize.x, Local.y * boxSize.y);
    vec2 centre = axis == 0 ? vec2(boxSize.z, boxSize.y) * 0.5 : axis == 1 ? vec2(boxSize.x, boxSize.z) * 0.5 : vec2(boxSize.x, boxSize.y) * 0.5;
    uv = (floor(uv * 16.0) + 0.5) / 16.0; // block pixels

    // Distance to the nearest and second-nearest cell point: near the boundary
    // between two cells is a crack.
    vec2 p = uv * 2.6 + float(axis) * 7.31;
    vec2 cell = floor(p);
    float f1 = 9.0, f2 = 9.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 c = cell + vec2(x, y);
            vec2 point = c + vec2(Hash(c), Hash(c + 19.19)) * 0.8 + 0.1;
            float d = length(point - p);
            if (d < f1) { f2 = f1; f1 = d; }
            else if (d < f2) f2 = d;
        }
    bool crack = f2 - f1 < 0.14;
    float reach = progress * 0.85;
    if (!crack || length(uv - centre) > reach) discard;
    FragColor = vec4(vec3(0.06, 0.05, 0.04) * brightness, 0.8);
}
