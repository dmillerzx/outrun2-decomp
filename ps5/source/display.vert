#version 450
layout(location=0) in vec2 p;layout(location=1) in vec4 c;layout(location=2) in vec2 t;
layout(location=0) out vec4 colour;layout(location=1) out vec2 uv;
void main(){gl_Position=vec4(p.x,-p.y,0,1);colour=c;uv=t;}
