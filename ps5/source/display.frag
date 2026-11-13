#version 450
layout(location=0) in vec4 colour;layout(location=1) in vec2 uv;
layout(set=0,binding=3) uniform sampler2D image;layout(location=0) out vec4 pixel;
void main(){pixel=texture(image,uv)*colour;}
