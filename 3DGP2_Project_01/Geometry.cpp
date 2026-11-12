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

void Box(std::vector<Vertex>& vertices, float left, float right, float bottom, float top, float front, float back) {
    Quad(vertices, {left,top,front}, {right,top,front}, {left,bottom,front}, {right,bottom,front}, {0,0,-1});
    Quad(vertices, {right,top,back}, {left,top,back}, {right,bottom,back}, {left,bottom,back}, {0,0,1});
    Quad(vertices, {left,top,back}, {left,top,front}, {left,bottom,back}, {left,bottom,front}, {-1,0,0});
    Quad(vertices, {right,top,front}, {right,top,back}, {right,bottom,front}, {right,bottom,back}, {1,0,0});
    Quad(vertices, {left,top,back}, {right,top,back}, {left,top,front}, {right,top,front}, {0,1,0});
    Quad(vertices, {left,bottom,front}, {right,bottom,front}, {left,bottom,back}, {right,bottom,back}, {0,-1,0});
}

struct FontBitmap {
    FontBitmap(int width, int height, int pixelSize) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        font = CreateFontW(-pixelSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, L"Malgun Gothic");
        if (!dc || !bitmap || !font) {
            Release();
            throw std::runtime_error("Windows Font Bitmap을 만들지 못했음");
        }
        oldBitmap = SelectObject(dc, bitmap);
        oldFont = SelectObject(dc, font);
        PatBlt(dc, 0, 0, width, height, BLACKNESS);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }
    ~FontBitmap() { Release(); }
    void Release() {
        if (oldBitmap) SelectObject(dc, oldBitmap);
        if (oldFont) SelectObject(dc, oldFont);
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HFONT font = nullptr;
    HGDIOBJ oldBitmap = nullptr, oldFont = nullptr;
    unsigned* pixels = nullptr;
};
}

Mesh MakeBox(Renderer& renderer) {
    std::vector<Vertex> vertices;
    Box(vertices, -.5f, .5f, -.5f, .5f, -.5f, .5f);
    return renderer.MakeMesh(vertices);
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
    std::vector<Vertex> vertices;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string tag;
        row >> tag;
        if (tag == "v" || tag == "vn") {
            XMFLOAT3 v{};
            row >> v.x >> v.y >> v.z;
            v.x = -v.x;  // RH to LH while keeping +Z as forward.
            (tag == "v" ? positions : normals).push_back(v);
        } else if (tag == "f") {
            std::vector<Vertex> face;
            std::string corner;
            while (row >> corner) {
                std::replace(corner.begin(), corner.end(), '/', ' ');
                std::istringstream indices(corner);
                unsigned p = 0, uv = 0, n = 0;
                if (!(indices >> p >> uv >> n) || !p || !uv || !n)
                    throw std::runtime_error("OBJ Face 데이터가 v/vt/vn 형식이 아님");
                face.push_back({positions.at(p - 1), normals.at(n - 1)});
            }
            for (size_t i = 1; i + 1 < face.size(); ++i)
                vertices.insert(vertices.end(), {face[0], face[i + 1], face[i]});
        }
    }
    return renderer.MakeMesh(vertices);
}

TextMesh Make3DText(Renderer& renderer, const wchar_t* text) {
    constexpr int width = 620, height = 36;
    FontBitmap mask(width, height, 19);
    TextOutW(mask.dc, 3, 2, text, lstrlenW(text));
    GdiFlush();
    int left = width, right = 0, top = height, bottom = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((mask.pixels[y * width + x] & 255) < 128) continue;
            left = std::min(left, x);
            right = std::max(right, x + 1);
            top = std::min(top, y);
            bottom = std::max(bottom, y + 1);
        }
    }
    std::vector<Vertex> vertices;
    constexpr float pixel = .095f;
    const float midX = (left + right) * .5f, midY = (top + bottom) * .5f;
    for (int y = top; y < bottom; ++y) {
        int x = left;
        while (x < right) {
            if ((mask.pixels[y * width + x] & 255) < 128) {
                ++x;
                continue;
            }
            const int start = x;
            while (x < right && (mask.pixels[y * width + x] & 255) >= 128) ++x;
            Box(vertices, (start - midX) * pixel, (x - midX) * pixel,
                (midY - y - 1) * pixel, (midY - y) * pixel, -.15f, .15f);
        }
    }
    return {renderer.MakeMesh(vertices), (right - left) * pixel, (bottom - top) * pixel};
}
