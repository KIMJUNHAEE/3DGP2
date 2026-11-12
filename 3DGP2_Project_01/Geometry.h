#pragma once
#include "Renderer.h"

struct TextMesh {
    Mesh mesh;
    float width;
    float height;
};

Mesh MakeBox(Renderer& renderer);
Mesh MakeSphere(Renderer& renderer);
Mesh MakeTerrain(Renderer& renderer);
float GroundHeight(float x, float z);
Mesh LoadObj(Renderer& renderer, const char* path);
TextMesh Make3DText(Renderer& renderer, const wchar_t* text);
TextMesh MakeTexturedQuad(Renderer& renderer, float width, float height,
    float u0, float v0, float u1, float v1);

