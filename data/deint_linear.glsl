precision mediump float;
varying vec2 vTexcoord;
uniform sampler2D s_ytex;
uniform sampler2D s_utex;
uniform sampler2D s_vtex;
uniform float line_height;

void main()
{
   float y, u, v;
   float y1, y2, u1, u2, v1, v2;
   float r, g, b;
   vec2 tmpcoord;
   vec2 tmpcoord_2;

   tmpcoord.x = vTexcoord.x;
   tmpcoord.y = vTexcoord.y + line_height;
   tmpcoord_2.x = vTexcoord.x;
   tmpcoord_2.y = vTexcoord.y + line_height*2.0;

   y1 = texture2D(s_ytex, vTexcoord).r;
   y2 = texture2D(s_ytex, tmpcoord).r;
   u1 = texture2D(s_utex, vTexcoord).r;
   u2 = texture2D(s_utex, tmpcoord_2).r;
   v1 = texture2D(s_vtex, vTexcoord).r;
   v2 = texture2D(s_vtex, tmpcoord_2).r;

   y = mix (y1, y2, 0.5);
   u = mix (u1, u2, 0.5);
   v = mix (v1, v2, 0.5);

   y = 1.1643 * (y - 0.0625);
   u = u - 0.5;
   v = v - 0.5;

   r = y + 1.5958 * v;
   g = y - 0.39173 * u - 0.81290 * v;
   b = y + 2.017 * u;
   gl_FragColor = vec4(r, g, b, 1.0);
}
