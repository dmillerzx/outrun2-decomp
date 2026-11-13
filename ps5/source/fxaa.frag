#version 450
layout(location=0) out vec4 outColor;
layout(set=0,binding=3) uniform sampler2D scene;
vec4 at(vec2 q){vec2 size=vec2(textureSize(scene,0));q=clamp(q,vec2(0.5),size-0.5);return textureLod(scene,q/size,0.0);}
float luma(vec3 c){return dot(c,vec3(0.299,0.587,0.114));}
void main(){
    vec2 q=gl_FragCoord.xy;
    vec4 cM=at(q);
    float lNW=luma(at(q+vec2(-1.0,-1.0)).rgb),lNE=luma(at(q+vec2(1.0,-1.0)).rgb);
    float lSW=luma(at(q+vec2(-1.0,1.0)).rgb),lSE=luma(at(q+vec2(1.0,1.0)).rgb);
    float lM=luma(cM.rgb);
    float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE)));
    float lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));
    vec2 dir=vec2(-((lNW+lNE)-(lSW+lSE)),(lNW+lSW)-(lNE+lSE));
    float reduce=max((lNW+lNE+lSW+lSE)*(0.25/8.0),1.0/128.0);
    float rcpMin=1.0/(min(abs(dir.x),abs(dir.y))+reduce);
    dir=clamp(dir*rcpMin,vec2(-8.0),vec2(8.0));
    vec3 a=0.5*(at(q+dir*(1.0/3.0-0.5)).rgb+at(q+dir*(2.0/3.0-0.5)).rgb);
    vec3 b=a*0.5+0.25*(at(q+dir*-0.5).rgb+at(q+dir*0.5).rgb);
    float lB=luma(b);
    vec3 aa=(lB<lMin||lB>lMax)?a:b;
    outColor=vec4((lMax-lMin<max(0.0312,lMax*0.125))?cM.rgb:aa,cM.a);
}
