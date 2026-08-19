#version 450
// 全屏三角形：不依赖顶点缓冲，由 gl_VertexIndex 生成覆盖整个视口的三角形。
// 顶点坐标超出 [-1,1]，保证光栅化覆盖每个像素中心（包括边缘像素）。
void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
