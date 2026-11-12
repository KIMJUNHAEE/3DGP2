#include "stdafx.h"
#include "Renderer.h"
#include <d3dcompiler.h>
#include <cstring>
#include <stdexcept>
#include "DDSTextureLoader12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {
void Check(HRESULT result, const char* step) {
    if (FAILED(result)) throw std::runtime_error(step);
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, entry, profile,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result)) {
        if (errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        throw std::runtime_error("Shaders.hlsl을 컴파일하지 못했음");
    }
    return code;
}

struct DrawConstants {
    XMFLOAT4X4 wvp, world;
    XMFLOAT4 color;
};
constexpr unsigned ConstantCount = sizeof(DrawConstants) / sizeof(float);
}

Renderer::~Renderer() {
    if (fenceEvent) CloseHandle(fenceEvent);
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::Cpu(ID3D12DescriptorHeap* heap, unsigned index,
    D3D12_DESCRIPTOR_HEAP_TYPE type) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(index) * device->GetDescriptorHandleIncrementSize(type);
    return handle;
}

void Renderer::Init(HWND window) {
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)), "DXGI Factory를 만들지 못했음");
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
            FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            throw std::runtime_error("WARP Device를 초기화하지 못했음");
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Command Queue를 만들지 못했음");
    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = Width;
    swapDesc.Height = Height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &swapDesc, nullptr, nullptr, &swapChain)) ||
        FAILED(swapChain.As(&swap))) {
        throw std::runtime_error("Swap Chain을 초기화하지 못했음");
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 2;
    Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvs)), "RTV Descriptor Heap을 만들지 못했음");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = 1;
    Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsv)), "DSV Descriptor Heap을 만들지 못했음");
    for (unsigned index = 0; index < 2; ++index) {
        Check(swap->GetBuffer(index, IID_PPV_ARGS(&targets[index])), "Swap Chain Buffer를 가져오지 못했음");
        device->CreateRenderTargetView(targets[index].Get(), nullptr, Cpu(rtvs.Get(), index, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    }
    D3D12_HEAP_PROPERTIES gpuHeap{};
    gpuHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = Width;
    depthDesc.Height = Height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = depthDesc.Format;
    depthClear.DepthStencil.Depth = 1;
    Check(device->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&depth)), "Depth Buffer를 만들지 못했음");
    device->CreateDepthStencilView(depth.Get(), nullptr, Cpu(dsv.Get(), 0, D3D12_DESCRIPTOR_HEAP_TYPE_DSV));

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
            IID_PPV_ARGS(&commands))) ||
        FAILED(commands->Close())) {
        throw std::runtime_error("Command List를 초기화하지 못했음");
    }
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "GPU Fence를 만들지 못했음");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) throw std::runtime_error("GPU Fence Event를 만들지 못했음");
    MakePipeline();
    LoadFontAtlas();
}

void Renderer::MakePipeline() {
    D3D12_ROOT_PARAMETER constants{};
    constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants.Constants.Num32BitValues = ConstantCount;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{1, &constants, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> signature;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&root)))) {
        throw std::runtime_error("Root Signature를 초기화하지 못했음");
    }

    const auto vs = Compile("VSMain", "vs_5_0");
    const auto ps = Compile("PSMain", "ps_5_0");
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.Get();
    state.InputLayout = {layout, _countof(layout)};
    state.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    state.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.DepthStencilState.DepthEnable = TRUE;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    state.SampleMask = UINT_MAX;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    state.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    state.SampleDesc.Count = 1;

    auto& blend = state.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    for (int index = 0; index < static_cast<int>(Pass::Count); ++index) {
        const auto pass = static_cast<Pass>(index);
        state.DepthStencilState.DepthWriteMask = pass == Pass::Opaque ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        blend.BlendEnable = pass == Pass::Alpha;
        Check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline[index])), "Graphics Pipeline State를 만들지 못했음");
    }
}

void Renderer::LoadFontAtlas() {
    std::unique_ptr<uint8_t[]> ddsData;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;

    Check(DirectX::LoadDDSTextureFromFileEx(
        device.Get(), L"Texture/FontUI.dds",
        0, D3D12_RESOURCE_FLAG_NONE, DirectX::DDS_LOADER_DEFAULT,
        fontAtlas.GetAddressOf(), ddsData, subresources),
        "FontUI.dds를 불러오지 못했음");

    const UINT count = static_cast<UINT>(subresources.size());
    auto upload = UploadBuffer(
        GetRequiredIntermediateSize(fontAtlas.Get(), 0, count));

    ResetCommands();

    if (UpdateSubresources(commands.Get(), fontAtlas.Get(), upload.Get(),
        0, 0, count, subresources.data()) == 0) {
        throw std::runtime_error("Font Texture를 복사하지 못했음");
    }

    Transition(commands.Get(), fontAtlas.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    SubmitCommands();
    WaitForGpu();
}

ComPtr<ID3D12Resource> Renderer::UploadBuffer(UINT64 bytes) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)), "Upload Buffer를 할당하지 못했음");
    return buffer;
}

Mesh Renderer::MakeMesh(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) throw std::runtime_error("Mesh에 Vertex가 없음");
    Mesh mesh;
    const auto bytes = static_cast<UINT>(vertices.size() * sizeof(Vertex));
    mesh.buffer = UploadBuffer(bytes);
    void* data = nullptr;
    D3D12_RANGE read{0, 0};
    Check(mesh.buffer->Map(0, &read, &data), "Mesh Buffer의 Map 호출에 실패했음");
    std::memcpy(data, vertices.data(), bytes);
    mesh.buffer->Unmap(0, nullptr);
    mesh.view = {mesh.buffer->GetGPUVirtualAddress(), bytes, sizeof(Vertex)};
    mesh.count = static_cast<unsigned>(vertices.size());
    return mesh;
}

void Renderer::ResetCommands() {
    if (FAILED(allocator->Reset()) || FAILED(commands->Reset(allocator.Get(), nullptr))) {
        throw std::runtime_error("Command List 기록을 시작하지 못했음");
    }
}

void Renderer::SubmitCommands() {
    Check(commands->Close(), "Command List를 닫지 못했음");
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
}

void Renderer::WaitForGpu() {
    ++fenceValue;
    Check(queue->Signal(fence.Get(), fenceValue), "GPU Fence에 신호를 보내지 못했음");
    if (fence->GetCompletedValue() < fenceValue) {
        Check(fence->SetEventOnCompletion(fenceValue, fenceEvent), "GPU 완료 대기 Event를 설정하지 못했음");
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void Renderer::BeginFrame(const XMFLOAT4& clear) {
    backBuffer = swap->GetCurrentBackBufferIndex();
    ResetCommands();
    currentPass = Pass::Count;
    Transition(commands.Get(), targets[backBuffer].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto color = Cpu(rtvs.Get(), backBuffer, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto z = Cpu(dsv.Get(), 0, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const float rgba[] = {clear.x, clear.y, clear.z, clear.w};
    commands->ClearRenderTargetView(color, rgba, 0, nullptr);
    commands->ClearDepthStencilView(z, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    commands->OMSetRenderTargets(1, &color, FALSE, &z);
    const D3D12_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1};
    const D3D12_RECT scissor{0, 0, Width, Height};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->SetGraphicsRootSignature(root.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::Draw(const Mesh& mesh, const XMMATRIX& world, const XMFLOAT4& color, Pass pass) {
    if (pass != currentPass) {
        commands->SetPipelineState(pipeline[static_cast<int>(pass)].Get());
        currentPass = pass;
    }
    DrawConstants constants{};
    XMStoreFloat4x4(&constants.wvp, world * viewProjection);
    XMStoreFloat4x4(&constants.world, world);
    constants.color = color;
    commands->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
    commands->IASetVertexBuffers(0, 1, &mesh.view);
    commands->DrawInstanced(mesh.count, 1, 0, 0);
}

void Renderer::EndFrame() {
    Transition(commands.Get(), targets[backBuffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    SubmitCommands();

    const HRESULT presentResult = swap->Present(1, 0);
    // Wait for presentation as well as drawing before reusing or releasing resources.
    WaitForGpu();
    Check(presentResult, "Frame을 화면에 표시하지 못했음");
}

