#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv{};
};

struct Mesh {
    ComPtr<ID3D12Resource> buffer;
    D3D12_VERTEX_BUFFER_VIEW view{};
    unsigned count = 0;
};

enum class Pass { Opaque, Alpha, Font, Model, Impact, Tree, Count };
enum class TextureId { Intro, Menu, Tutorial, Tank, Helicopter, Impact, Tree, Digits, GameText, Count };

class Renderer {
public:
    ~Renderer();
    void Init(HWND window);
    bool Resize(UINT width, UINT height);
    UINT Width() const { return width; }
    UINT Height() const { return height; }
    Mesh MakeMesh(const std::vector<Vertex>& vertices);
    void BeginFrame(const XMFLOAT4& clear);
    void BindTexture(TextureId texture);
    void Draw(const Mesh& mesh, const XMMATRIX& world, const XMFLOAT4& color,
        Pass pass = Pass::Opaque, float animationProgress = 0.0f);
    void EndFrame();
    XMMATRIX viewProjection = XMMatrixIdentity();
    XMFLOAT3 cameraPosition{};

private:
    ComPtr<ID3D12Resource> UploadBuffer(UINT64 bytes);
    void ResetCommands();
    void SubmitCommands();
    void WaitForGpu();
    void MakePipeline();
    void CreateFrameBuffers();
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, unsigned index, D3D12_DESCRIPTOR_HEAP_TYPE type);

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    ComPtr<ID3D12DescriptorHeap> rtvs, dsv;
    ComPtr<ID3D12Resource> targets[2], depth;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline[static_cast<int>(Pass::Count)];
    UINT width = 0, height = 0;
    unsigned backBuffer = 0;
    Pass currentPass = Pass::Count;

    void LoadTextures();
    ComPtr<ID3D12Resource> textures[static_cast<UINT>(TextureId::Count)];
    ComPtr<ID3D12DescriptorHeap> textureSrvHeap;
};

