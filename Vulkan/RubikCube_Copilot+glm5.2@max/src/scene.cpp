#include "scene.h"

#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace rubik {

// ---- 立方体几何（24 顶点，6 面，每面 4 顶点 + 2 三角形） -----------------
void makeCubeGeometry(std::vector<Vertex>& verts, std::vector<uint32_t>& indices) {
    verts.clear();
    indices.clear();
    // 单位立方体 [-0.5,0.5]^3。每面 4 顶点，法线朝外，uv [0,1]^2。
    // 面：(法线, 4 个角点 uv)
    struct Face { float n[3]; float v[4][3]; };
    // 顺序：+X,-X,+Y,-Y,+Z,-Z；逆时针为正面（从外看）
    const Face faces[6] = {
        {{ 1, 0, 0}, {{ 0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f, 0.5f}}}, // +X
        {{-1, 0, 0}, {{-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f, 0.5f},{-0.5f, 0.5f, 0.5f},{-0.5f, 0.5f,-0.5f}}}, // -X
        {{ 0, 1, 0}, {{-0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{ 0.5f, 0.5f,-0.5f},{-0.5f, 0.5f,-0.5f}}}, // +Y
        {{ 0,-1, 0}, {{-0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f,-0.5f},{ 0.5f,-0.5f, 0.5f},{-0.5f,-0.5f, 0.5f}}}, // -Y
        {{ 0, 0, 1}, {{-0.5f,-0.5f, 0.5f},{ 0.5f,-0.5f, 0.5f},{ 0.5f, 0.5f, 0.5f},{-0.5f, 0.5f, 0.5f}}}, // +Z
        {{ 0, 0,-1}, {{ 0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f, 0.5f,-0.5f},{ 0.5f, 0.5f,-0.5f}}}, // -Z
    };
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int f = 0; f < 6; ++f) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        // 切线 = 沿 UV 的 U 方向（uv (0,0)->(1,0)）的世界向量 = v[1]-v[0]
        float tx = faces[f].v[1][0] - faces[f].v[0][0];
        float ty = faces[f].v[1][1] - faces[f].v[0][1];
        float tz = faces[f].v[1][2] - faces[f].v[0][2];
        float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
        tx /= tl; ty /= tl; tz /= tl;
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.pos[0] = faces[f].v[i][0]; v.pos[1] = faces[f].v[i][1]; v.pos[2] = faces[f].v[i][2];
            v.normal[0] = faces[f].n[0]; v.normal[1] = faces[f].n[1]; v.normal[2] = faces[f].n[2];
            v.tangent[0] = tx; v.tangent[1] = ty; v.tangent[2] = tz;
            v.uv[0] = uvs[i][0]; v.uv[1] = uvs[i][1];
            verts.push_back(v);
        }
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
}

// ---- 实例模型矩阵（仅平移，边长 1） -------------------------------------
// mat4 列主序平移矩阵：列0=(1,0,0,0) 列1=(0,1,0,0) 列2=(0,0,1,0) 列3=(tx,ty,tz,1)
static Instance makeInstance(float x, float y, float z, uint32_t matIdx) {
    Instance inst{};
    inst.model[0]  = 1.0f; inst.model[1]  = 0.0f; inst.model[2]  = 0.0f; inst.model[3]  = 0.0f;
    inst.model[4]  = 0.0f; inst.model[5]  = 1.0f; inst.model[6]  = 0.0f; inst.model[7]  = 0.0f;
    inst.model[8]  = 0.0f; inst.model[9]  = 0.0f; inst.model[10] = 1.0f; inst.model[11] = 0.0f;
    inst.model[12] = x;    inst.model[13] = y;    inst.model[14] = z;    inst.model[15] = 1.0f;
    inst.matIdx = matIdx;
    return inst;
}

// ---- 场景实例布局 -------------------------------------------------------
// 索引 0/1/2 对应坐标 -1/0/+1
static float idxToCoord(int i) { return static_cast<float>(i - 1); }

std::vector<Instance> buildSceneInstances(int matBottomLayer, int matMidLayer,
                                          int matTopLayer, int matFloor) {
    std::vector<Instance> out;

    // 第 1 层（底层）：全部 9 个，y 中心 0.5
    for (int x = 0; x < 3; ++x)
        for (int z = 0; z < 3; ++z)
            out.push_back(makeInstance(idxToCoord(x), 0.5f, idxToCoord(z),
                                       static_cast<uint32_t>(matBottomLayer)));

    // 第 2 层（中间层）：存在 6 个 (0,0)(0,1)(0,2)(1,0)(1,1)(2,0)
    const int midPresent[6][2] = {{0,0},{0,1},{0,2},{1,0},{1,1},{2,0}};
    for (auto& p : midPresent)
        out.push_back(makeInstance(idxToCoord(p[0]), 1.5f, idxToCoord(p[1]),
                                   static_cast<uint32_t>(matMidLayer)));

    // 第 3 层（顶层）：存在 3 个 (0,0)(0,1)(1,2)
    const int topPresent[3][2] = {{0,0},{0,1},{1,2}};
    for (auto& p : topPresent)
        out.push_back(makeInstance(idxToCoord(p[0]), 2.5f, idxToCoord(p[1]),
                                   static_cast<uint32_t>(matTopLayer)));

    // 地板：9x9 单位立方体，顶面 y=0，中心 y=-0.5；索引 0..8 对应 -4..+4
    for (int x = 0; x < 9; ++x)
        for (int z = 0; z < 9; ++z)
            out.push_back(makeInstance(static_cast<float>(x) - 4.0f, -0.5f,
                                       static_cast<float>(z) - 4.0f,
                                       static_cast<uint32_t>(matFloor)));

    return out;
}

// ---- 相机矩阵（列主序） -------------------------------------------------
static void mulMat4(const float a[16], const float b[16], float out[16]) {
    float tmp[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            tmp[c * 4 + r] = s;
        }
    }
    for (int i = 0; i < 16; ++i) out[i] = tmp[i];
}

static void lookAt(const float eye[3], const float center[3], const float up[3],
                   float out[16]) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz); fx /= fl; fy /= fl; fz /= fl;
    // right = normalize(cross(f, up))
    float rx = fy*up[2] - fz*up[1];
    float ry = fz*up[0] - fx*up[2];
    float rz = fx*up[1] - fy*up[0];
    float rl = std::sqrt(rx*rx + ry*ry + rz*rz); rx /= rl; ry /= rl; rz /= rl;
    // up = cross(right, f)
    float ux = ry*fz - rz*fy;
    float uy = rz*fx - rx*fz;
    float uz = rx*fy - ry*fx;
    // 列主序 view 矩阵
    out[0]=rx; out[1]=ux; out[2]=-fx; out[3]=0.0f;
    out[4]=ry; out[5]=uy; out[6]=-fy; out[7]=0.0f;
    out[8]=rz; out[9]=uz; out[10]=-fz; out[11]=0.0f;
    out[12]=-(rx*eye[0]+ry*eye[1]+rz*eye[2]);
    out[13]=-(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[14]=  (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[15]=1.0f;
}

static void perspective(float fovDeg, float aspect, float nearZ, float farZ,
                        float out[16]) {
    float f = 1.0f / std::tan(fovDeg * 3.14159265359f / 360.0f);
    float range = farZ - nearZ;
    // 列主序透视投影。Vulkan 的 NDC Y 轴向下（与 OpenGL 相反），
    // 故投影矩阵的 Y 缩放取负，配合 OpenGL 风格 view 矩阵得到正立画面。
    out[0]=f/aspect; out[1]=0; out[2]=0; out[3]=0;
    out[4]=0; out[5]=-f; out[6]=0; out[7]=0;
    out[8]=0; out[9]=0; out[10]=-(farZ+nearZ)/range; out[11]=-1.0f;
    out[12]=0; out[13]=0; out[14]=-(2.0f*farZ*nearZ)/range; out[15]=0;
}

CameraUbo buildCameraUbo(const Camera& cam, uint32_t fbW, uint32_t fbH) {
    CameraUbo ubo{};
    float aspect = (fbH > 0) ? static_cast<float>(fbW) / static_cast<float>(fbH) : 1.0f;
    float view[16], proj[16];
    lookAt(cam.pos, cam.target, cam.up, view);
    perspective(cam.fovDeg, aspect, cam.nearZ, cam.farZ, proj);
    mulMat4(proj, view, ubo.viewProj); // viewProj = proj * view（列主序：proj*view）
    ubo.camPos[0] = cam.pos[0]; ubo.camPos[1] = cam.pos[1]; ubo.camPos[2] = cam.pos[2];
    ubo._pad = 0.0f;
    return ubo;
}

// ---- 轨道相机：由球面坐标推导 pos ---------------------------------------
// 约定：yaw=0 时相机在 +X 方向；pitch=0 时在 XZ 平面，正 pitch 抬高。
// pos = target + distance * (cos(pitch)*cos(yaw), sin(pitch), cos(pitch)*sin(yaw))
void updateCameraOrbit(Camera& cam) {
    if (!cam.orbitInit) {
        // 首次用初始 pos/target 反推球面坐标
        float dx = cam.pos[0] - cam.target[0];
        float dy = cam.pos[1] - cam.target[1];
        float dz = cam.pos[2] - cam.target[2];
        cam.distance = std::sqrt(dx*dx + dy*dy + dz*dz);
        cam.pitch = std::asin(dy / std::max(cam.distance, 1e-6f));
        cam.yaw = std::atan2(dz, dx);
        cam.orbitInit = true;
    }
    // 限制 pitch 避免万向锁
    cam.pitch = std::clamp(cam.pitch, -1.55f, 1.55f);
    cam.distance = std::clamp(cam.distance, 1.0f, 80.0f);
    float cp = std::cos(cam.pitch);
    cam.pos[0] = cam.target[0] + cam.distance * cp * std::cos(cam.yaw);
    cam.pos[1] = cam.target[1] + cam.distance * std::sin(cam.pitch);
    cam.pos[2] = cam.target[2] + cam.distance * cp * std::sin(cam.yaw);
}

// ---- 颜色空间与材质参数 -------------------------------------------------
float srgbToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

MaterialParams buildMaterialParams(const Material& m) {
    MaterialParams p{};
    p.baseColorLinearSRGBA[0] = srgbToLinear(m.baseColorSRGB[0]);
    p.baseColorLinearSRGBA[1] = srgbToLinear(m.baseColorSRGB[1]);
    p.baseColorLinearSRGBA[2] = srgbToLinear(m.baseColorSRGB[2]);
    p.baseColorLinearSRGBA[3] = m.ambientOcclusion;
    p.metallicRoughnessNormStrength[0] = m.metallic;
    p.metallicRoughnessNormStrength[1] = m.roughness;
    p.metallicRoughnessNormStrength[2] = m.normalStrength;
    p.metallicRoughnessNormStrength[3] = 0.0f;
    return p;
}

} // namespace rubik
