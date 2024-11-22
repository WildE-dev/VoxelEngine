#version 330 core

const float PI = 3.1415926535f;

out vec4 fragColor;

in vec2 texCoords;

uniform sampler2D colorTexture;

uniform float time;
uniform vec2 uResolution;

void main()
{
    vec2 screenSpaceUV = gl_FragCoord.xy / uResolution;
    vec3 colorUV = texture2D(colorTexture, screenSpaceUV).rgb;

	vec3 color = colorUV;

	// --------

	float size = 0.45f;
	vec2 center = vec2(0.5f) + vec2(0.02f * cos(time), 0.02f * sin(time));
	float dist = length(center - texCoords);

	if (dist > size){
		discard;
	}

	float ex = (1.0f / size * dist);
	color = texture2D(colorTexture, screenSpaceUV + ex + (0.02f * sin(time))).rgb;

	color *= smoothstep(colorUV * vec3(0.0f), vec3(1.0f), vec3(dist * 4.0f));

	fragColor = vec4(color, 1.0f);
}