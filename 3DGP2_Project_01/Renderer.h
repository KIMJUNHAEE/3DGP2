#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

constexpr unsigned Width = 1280;
constexpr unsigned Height = 720;

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

enum class Pass { Opaque, Alpha, Font, Count };
enum class TextTexture { Intro, Menu, Tutorial, Count };

class Renderer {
public:
    ~Renderer();
    void Init(HWND window);
    Mesh MakeMesh(const std::vector<Vertex>& vertices);
    void BeginFrame(const XMFLOAT4& clear);
    void BindTextTexture(TextTexture texture);
    void Draw(const Mesh& mesh, const XMMATRIX& world, const XMFLOAT4& color,
        Pass pass = Pass::Opaque);
    void EndFrame();
    XMMATRIX viewProjection = XMMatrixIdentity();

private:
    ComPtr<ID3D12Resource> UploadBuffer(UINT64 bytes);
    void ResetCommands();
    void SubmitCommands();
    void WaitForGpu();
    void MakePipeline();
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
    unsigned backBuffer = 0;
    Pass currentPass = Pass::Count;

    void LoadTextTextures();
    ComPtr<ID3D12Resource> textTextures[static_cast<UINT>(TextTexture::Count)];
    ComPtr<ID3D12DescriptorHeap> textSrvHeap;
};

