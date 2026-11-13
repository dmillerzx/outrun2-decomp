#version 450
layout(location=0) in vec4 p;layout(location=1) in vec4 d0;layout(location=2) in vec4 d1;
layout(location=0) out vec4 od0;layout(location=1) out vec4 od1;
layout(location=3) in vec4 t0;layout(location=2) out vec4 ot0;
layout(location=4) in vec4 t1;layout(location=3) out vec4 ot1;
layout(location=5) in vec4 t2;layout(location=4) out vec4 ot2;
layout(location=6) in vec4 t3;layout(location=5) out vec4 ot3;
layout(location=7) in vec4 t4;layout(location=6) out vec4 ot4;
layout(location=8) in vec4 t5;layout(location=7) out vec4 ot5;
layout(location=9) in vec4 t6;layout(location=8) out vec4 ot6;
layout(location=10) in vec4 t7;layout(location=9) out vec4 ot7;

layout(location=11) in float fog;layout(location=10) out float ofog;
void main(){gl_Position=vec4(p.x,-p.y,p.z,p.w);od0=d0;od1=d1;ofog=fog;
ot0=t0;ot1=t1;ot2=t2;ot3=t3;ot4=t4;ot5=t5;ot6=t6;ot7=t7;}
