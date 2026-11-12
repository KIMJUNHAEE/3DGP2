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

float Terrain::SampleHeight(int column, int row) const {
    column = std::clamp(column, 0, SamplesPerSide - 1);
    row = std::clamp(row, 0, SamplesPerSide - 1);
    constexpr float minHeight = -1.5f;
    constexpr float heightPerByte = 18.0f / 255.0f;
    return minHeight + heights[row * SamplesPerSide + column] * heightPerByte;
}

bool Terrain::Contains(float x, float z) const {
    return std::abs(x) <= HalfExtent && std::abs(z) <= HalfExtent;
}

float Terrain::HeightAt(float x, float z) const {
    constexpr int lastCell = SamplesPerSide - 2;
    constexpr float lastSample = static_cast<float>(SamplesPerSide - 1);
    const float gridX = std::clamp((x + HalfExtent) / CellSize, 0.0f, lastSample);
    const float gridZ = std::clamp((z + HalfExtent) / CellSize, 0.0f, lastSample);
    const int column = std::min(static_cast<int>(gridX), lastCell);
    const int row = std::min(static_cast<int>(gridZ), lastCell);
    const float u = gridX - column, v = gridZ - row;
    const float b = SampleHeight(column + 1, row);
    const float c = SampleHeight(column, row + 1);

    // 메시와 같은 대각선을 기준으로 높이를 보간한다.
    if (u + v <= 1.0f) {
        const float a = SampleHeight(column, row);
        return a + (b - a) * u + (c - a) * v;
    }
    const float d = SampleHeight(column + 1, row + 1);
    return d + (c - d) * (1.0f - u) + (b - d) * (1.0f - v);
}

void Terrain::Load(Renderer& renderer, const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("지형 RAW 파일을 열지 못했음\n") + path);
    // RAW 파일은 헤더 없이 Z 행마다 X 방향 높이 샘플을 저장한다.
    if (!file.read(reinterpret_cast<char*>(heights.data()), static_cast<std::streamsize>(heights.size())) ||
        file.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error(std::string("지형 RAW 파일 크기가 맞지 않음\n") + path);
    }

    const auto vertex = [this](int column, int row) {
        const float x = column * CellSize - HalfExtent;
        const float z = row * CellSize - HalfExtent;
        XMFLOAT3 normal;
        XMStoreFloat3(&normal, XMVector3Normalize(XMVectorSet(
            SampleHeight(column - 1, row) - SampleHeight(column + 1, row), 2.0f * CellSize,
            SampleHeight(column, row - 1) - SampleHeight(column, row + 1), 0)));
        constexpr float tileSize = 12.0f;
        return Vertex{{x, SampleHeight(column, row), z}, normal, {x / tileSize, z / tileSize}};
    };
    std::vector<Vertex> vertices;
    vertices.reserve((SamplesPerSide - 1) * (SamplesPerSide - 1) * 6);
    for (int row = 0; row < SamplesPerSide - 1; ++row) {
        for (int column = 0; column < SamplesPerSide - 1; ++column) {
            const auto a = vertex(column, row), b = vertex(column + 1, row);
            const auto c = vertex(column, row + 1), d = vertex(column + 1, row + 1);
            vertices.insert(vertices.end(), {a,b,c,c,b,d});
        }
    }
    mesh = renderer.MakeMesh(vertices);
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
