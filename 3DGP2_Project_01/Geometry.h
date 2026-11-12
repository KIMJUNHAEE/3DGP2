#pragma once
#include "Renderer.h"
#include <array>

struct TextMesh {
    Mesh mesh;
    float width;
    float height;
};

class Terrain {
public:
    void Load(Renderer& renderer, const char* path);
    float HeightAt(float x, float z) const;
    bool Contains(float x, float z) const;
    const Mesh& GetMesh() const { return mesh; }

private:
    static constexpr int SamplesPerSide = 113;
    static constexpr float CellSize = 2.0f;
    static constexpr float HalfExtent = (SamplesPerSide - 1) * CellSize * .5f;

    float SampleHeight(int column, int row) const;

    std::array<unsigned char, SamplesPerSide * SamplesPerSide> heights{};
    Mesh mesh;
};

Mesh MakeBox(Renderer& renderer);
std::array<Mesh, 6> MakeSkyboxFaces(Renderer& renderer);
Mesh MakeSphere(Renderer& renderer);
Mesh LoadObj(Renderer& renderer, const char* path);
TextMesh MakeTexturedQuad(Renderer& renderer, float width, float height,
    float u0, float v0, float u1, float v1);

