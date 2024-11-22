#version 330 core

out vec4 fragColor;

in vec2 texCoords;          // Interpolated texture coordinates from the vertex shader
uniform sampler2D screenTexture; // Screen content texture
uniform vec2 uResolution;       // Screen resolution
uniform vec2 blackHoleCenter;   // Center of the black hole in quad UV space
uniform float eventHorizon;     // Radius of the event horizon
uniform float lensingRadius;    // Radius of the lensing effect
uniform float maxDistortion;    // Maximum distortion
uniform mat4 model;             // Model matrix for the quad
uniform mat4 view;              // View matrix for the camera
uniform mat4 projection;        // Projection matrix

void main()
{
    // Calculate the fragment's position in local quad space (UV space)
    vec2 screenCoords = texCoords;

    // Calculate distance from the black hole center (in UV space)
    float dist = length(screenCoords - blackHoleCenter);

    // Discard fragments outside the lensing radius
    if (dist > lensingRadius) {
        discard;
    }

    // Inside the event horizon, render pure black
    if (dist < eventHorizon) {
        fragColor = vec4(0.0); // Black
        return;
    }

    // Compute lensing distortion
    vec2 direction = normalize(screenCoords - blackHoleCenter);
    float distortion = maxDistortion * (1.0 - smoothstep(eventHorizon, lensingRadius, dist));

    // Apply distortion to the UV coordinates
    vec2 distortedCoords = gl_FragCoord.xy / uResolution - direction * distortion;

    // Clamp the distorted coordinates to avoid sampling outside the texture
    distortedCoords = clamp(distortedCoords, vec2(0.0), vec2(1.0));

    // Sample the texture with the distorted coordinates
    vec3 color = texture(screenTexture, distortedCoords).rgb;

    // Smooth fade-out effect toward the edge of the lensing radius
    //color *= smoothstep(lensingRadius, eventHorizon, dist);

    fragColor = vec4(color, 1.0);
    //fragColor = vec4(texture(screenTexture, gl_FragCoord.xy / uResolution).rgb, 1);
}
