precision mediump float;
varying vec2 vTexcoord;
uniform sampler2D s_tex;
uniform float line_height;

void main()
{
    gl_FragColor = vec4(texture2D(s_tex, vTexcoord).rgb, 1.0);
}
