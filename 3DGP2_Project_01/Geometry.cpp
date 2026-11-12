#include "stdafx.h"
#include "Geometry.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void Quad(std::vector<Vertex>& vertices, const XMFLOAT3& topLeft, const XMFLOAT3& topRight,
    const XMFLOAT3& bottomLeft, const XMFLOAT3& bottomRight, const XMFLOAT3& normal) {
    const Vertex a{topLeft, normal}, b{topRight, normal};
    const Vertex c{bottomLeft, normal}, d{bottomRight, normal};
    vertices.insert(vertices.end(), {a, b, c, c, b, d});
}
}

Mesh MakeBox(Renderer& renderer) {
    constexpr float low = -.5f, high = .5f;
    std::vector<Vertex> vertices;
    Quad(vertices, {low,high,low}, {high,high,low}, {low,low,low}, {high,low,low}, {0,0,-1});
    Quad(vertices, {high,high,high}, {low,high,high}, {high,low,high}, {low,low,high}, {0,0,1});
    Quad(vertices, {low,high,high}, {low,high,low}, {low,low,high}, {low,low,low}, {-1,0,0});
    Quad(vertices, {high,high,low}, {high,high,high}, {high,low,low}, {high,low,high}, {1,0,0});
    Quad(vertices, {low,high,high}, {high,high,high}, {low,high,low}, {high,high,low}, {0,1,0});
    Quad(vertices, {low,low,low}, {high,low,low}, {low,low,high}, {high,low,high}, {0,-1,0});
    return renderer.MakeMesh(vertices);
}

std::array<Mesh, 6> MakeSkyboxFaces(Renderer& renderer) {
    // 면마다 왼쪽 위, 오른쪽 위, 왼쪽 아래, 오른쪽 아래 순서로 배치한다.
    constexpr XMFLOAT3 corners[6][4] = {
        {{1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {1, -1, -1}},     // 오른쪽
        {{-1, 1, -1}, {-1, 1, 1}, {-1, -1, -1}, {-1, -1, 1}}, // 왼쪽
        {{-1, 1, -1}, {1, 1, -1}, {-1, 1, 1}, {1, 1, 1}},     // 위
        {{-1, -1, 1}, {1, -1, 1}, {-1, -1, -1}, {1, -1, -1}}, // 아래
        {{-1, 1, 1}, {1, 1, 1}, {-1, -1, 1}, {1, -1, 1}},     // 앞
        {{1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}}  // 뒤
    };
    std::array<Mesh, 6> faces;
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& face = corners[i];
        const Vertex a{face[0], {}, {0, 0}}, b{face[1], {}, {1, 0}};
        const Vertex c{face[2], {}, {0, 1}}, d{face[3], {}, {1, 1}};
        faces[i] = renderer.MakeMesh({a, b, c, c, b, d});
    }
    return faces;
}

Mesh MakeSphere(Renderer& renderer) {
    std::vector<Vertex> vertices;
    auto vertex = [](int column, int row) {
        float u = column / 24.f, v = row / 12.f;
        const float azimuth = u * XM_2PI, polar = v * XM_PI;
        XMFLOAT3 p{std::sin(azimuth) * std::sin(polar), std::cos(polar), std::cos(azimuth) * std::sin(polar)};
        return Vertex{p, p};
    };
    for (int row = 0; row < 12; ++row) {
        for (int col = 0; col < 24; ++col) {
            auto a = vertex(col, row), b = vertex(col + 1, row);
            auto c = vertex(col, row + 1), d = vertex(col + 1, row + 1);
            vertices.insert(vertices.end(), {a,b,c,c,b,d});
        }
    }
    return renderer.MakeMesh(vertices);
}

float GroundHeight(float x, float z) {
    const float edge = std::clamp((std::max(std::abs(x), std::abs(z)) - 76.f) / 32.f, 0.f, 1.f);
    return .8f * std::sin(x * .075f) * std::cos(z * .056f) +
        .45f * std::sin((x + z) * .091f) + edge * edge * (10.f + 5.f * std::sin(x * .16f + z * .1f));
}

Mesh MakeTerrain(Renderer& renderer) {
    std::vector<Vertex> vertices;
    auto vertex = [](float x, float z) {
        XMFLOAT3 normal;
        XMStoreFloat3(&normal, XMVector3Normalize(XMVectorSet(
            GroundHeight(x - .1f, z) - GroundHeight(x + .1f, z), .2f,
            GroundHeight(x, z - .1f) - GroundHeight(x, z + .1f), 0)));
        return Vertex{{x, GroundHeight(x, z), z}, normal};
    };
    constexpr int step = 2;
    for (int z = -112; z < 112; z += step) {
        for (int x = -112; x < 112; x += step) {
            const auto a = vertex(float(x), float(z)), b = vertex(float(x + step), float(z));
            const auto c = vertex(float(x), float(z + step)), d = vertex(float(x + step), float(z + step));
            vertices.insert(vertices.end(), {a,b,c,c,b,d});
        }
    }
    return renderer.MakeMesh(vertices);
}

Mesh LoadObj(Renderer& renderer, const char* path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error(std::string("OBJ 파일을 열지 못했음\n") + path);
    std::vector<XMFLOAT3> positions, normals;
    std::vector<XMFLOAT2> texcoords;
    std::vector<Vertex> vertices;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string tag;
        row >> tag;
        if (tag == "v" || tag == "vn") {
            XMFLOAT3 v{};
            row >> v.x >> v.y >> v.z;
            v.x = -v.x;  // 전방 방향은 +Z로 유지하면서 오른손 좌표계를 왼손 좌표계로 변환한다.
            (tag == "v" ? positions : normals).push_back(v);
        } else if (tag == "vt") {
            XMFLOAT2 uv{};
            row >> uv.x >> uv.y;
            texcoords.push_back({uv.x, 1.0f - uv.y});
        } else if (tag == "f") {
            std::vector<Vertex> face;
            std::string corner;
            while (row >> corner) {
                std::replace(corner.begin(), corner.end(), '/', ' ');
                std::istringstream indices(corner);
                unsigned p = 0, uv = 0, n = 0;
                if (!(indices >> p >> uv >> n) || !p || !uv || !n)
                    throw std::runtime_error("OBJ Face 데이터가 v/vt/vn 형식이 아님");
                face.push_back({positions.at(p - 1), normals.at(n - 1), texcoords.at(uv - 1)});
            }
            for (size_t i = 1; i + 1 < face.size(); ++i)
                vertices.insert(vertices.end(), {face[0], face[i + 1], face[i]});
        }
    }
    return renderer.MakeMesh(vertices);
}

TextMesh MakeTexturedQuad(Renderer& renderer, float width, float height,
    float u0, float v0, float u1, float v1) {
    const float x = width / 2.0f;
    const float y = height / 2.0f;
    const Vertex a{{-x,  y, 0}, {}, {u0, v0}};
    const Vertex b{{ x,  y, 0}, {}, {u1, v0}};
    const Vertex c{{-x, -y, 0}, {}, {u0, v1}};
    const Vertex d{{ x, -y, 0}, {}, {u1, v1}};
    return {renderer.MakeMesh({a, b, c, c, b, d}), width, height};
}
