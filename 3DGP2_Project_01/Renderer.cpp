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
    float animationProgress;
    XMFLOAT3 cameraPosition;
};
constexpr unsigned ConstantCount = sizeof(DrawConstants) / sizeof(float);

bool UsesPointList(Pass pass) {
    return pass == Pass::Impact || pass == Pass::Tree;
}
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
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) {
        throw std::runtime_error("게임 창의 크기를 가져오지 못했음");
    }
    width = static_cast<UINT>(client.right);
    height = static_cast<UINT>(client.bottom);

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
    swapDesc.Width = width;
    swapDesc.Height = height;
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
    CreateFrameBuffers();

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
    LoadTextures();
}

bool Renderer::Resize(UINT newWidth, UINT newHeight) {
    if (!newWidth || !newHeight || (newWidth == width && newHeight == height)) return false;

    WaitForGpu();
    for (auto& target : targets) target.Reset();
    depth.Reset();
    Check(swap->ResizeBuffers(0, newWidth, newHeight, DXGI_FORMAT_UNKNOWN, 0),
        "Swap Chain 크기를 변경하지 못했음");

    width = newWidth;
    height = newHeight;
    CreateFrameBuffers();
    return true;
}

void Renderer::CreateFrameBuffers() {
    for (unsigned index = 0; index < 2; ++index) {
        Check(swap->GetBuffer(index, IID_PPV_ARGS(&targets[index])), "Swap Chain Buffer를 가져오지 못했음");
        device->CreateRenderTargetView(targets[index].Get(), nullptr, Cpu(rtvs.Get(), index, D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    }
    D3D12_HEAP_PROPERTIES gpuHeap{};
    gpuHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
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
}

void Renderer::MakePipeline() {
    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 0;  // 텍스처 레지스터 t0

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = ConstantCount;
    params[0].Constants.ShaderRegister = 0;  // 루트 상수 레지스터 b0
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &textureRange };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;  // 텍스처 샘플러 레지스터 s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{
        _countof(params), params, 1, &sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
    };

    ComPtr<ID3DBlob> signature;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, nullptr)) ||
        FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&root)))) {
        throw std::runtime_error("Root Signature를 초기화하지 못했음");
    }

    const auto vs = Compile("VSMain", "vs_5_0");
    const auto ps = Compile("PSMain", "ps_5_0");
    const auto psFont = Compile("PSFont", "ps_5_0");
    const auto psModel = Compile("PSModel", "ps_5_0");
    const auto vsImpact = Compile("VSImpact", "vs_5_0");
    const auto gsImpact = Compile("GSImpact", "gs_5_0");
    const auto psImpact = Compile("PSImpact", "ps_5_0");
    const auto vsTree = Compile("VSTree", "vs_5_0");
    const auto gsTree = Compile("GSTree", "gs_5_0");
    const auto psTree = Compile("PSTree", "ps_5_0");
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };
    const D3D12_INPUT_ELEMENT_DESC treeLayout[] = {layout[0], layout[2]};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    state.pRootSignature = root.Get();
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.DepthStencilState.DepthEnable = TRUE;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    state.SampleMask = UINT_MAX;
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
        const bool impact = pass == Pass::Impact;
        const bool tree = pass == Pass::Tree;
        const bool transparent = pass == Pass::Alpha || pass == Pass::Font || impact;
        state.InputLayout = {layout, impact ? 1u : static_cast<UINT>(_countof(layout))};
        if (tree) state.InputLayout = {treeLayout, _countof(treeLayout)};
        state.PrimitiveTopologyType = UsesPointList(pass) ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
            : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        auto* vertexShader = vs.Get();
        ID3DBlob* geometryShader = nullptr;
        auto* pixelShader = ps.Get();
        if (pass == Pass::Font) pixelShader = psFont.Get();
        if (pass == Pass::Model) pixelShader = psModel.Get();
        if (impact) {
            vertexShader = vsImpact.Get();
            geometryShader = gsImpact.Get();
            pixelShader = psImpact.Get();
        }
        if (tree) {
            vertexShader = vsTree.Get();
            geometryShader = gsTree.Get();
            pixelShader = psTree.Get();
        }
        state.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
        state.GS = {};
        if (geometryShader) {
            state.GS = {geometryShader->GetBufferPointer(), geometryShader->GetBufferSize()};
        }
        state.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
        state.DepthStencilState.DepthWriteMask = transparent ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        blend.BlendEnable = transparent;
        Check(device->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&pipeline[index])), "Graphics Pipeline State를 만들지 못했음");
    }
}

void Renderer::LoadTextures() {
    constexpr UINT textureCount = static_cast<UINT>(TextureId::Count);
    constexpr struct {
        const wchar_t* path;
        const char* error;
    } files[] = {
        {L"Texture/IntroText.dds", "IntroText.dds를 불러오지 못했음"},
        {L"Texture/MenuText.dds", "MenuText.dds를 불러오지 못했음"},
        {L"Texture/TutorialText.dds", "TutorialText.dds를 불러오지 못했음"},
        {L"Texture/T04_BaseColor.dds", "T04_BaseColor.dds를 불러오지 못했음"},
        {L"Texture/H04_BaseColor.dds", "H04_BaseColor.dds를 불러오지 못했음"},
        {L"Texture/ImpactSpark.dds", "ImpactSpark.dds를 불러오지 못했음"},
        {L"Texture/TreeBillboard.dds", "TreeBillboard.dds를 불러오지 못했음"},
        {L"Texture/FontDigits.dds", "FontDigits.dds를 불러오지 못했음"},
        {L"Texture/GameText.dds", "GameText.dds를 불러오지 못했음"}
    };
    static_assert(_countof(files) == textureCount);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = textureCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&textureSrvHeap)),
        "Texture용 Descriptor Heap을 만들지 못했음");

    ResetCommands();
    ComPtr<ID3D12Resource> uploads[textureCount];
    for (UINT index = 0; index < textureCount; ++index) {
        auto& texture = textures[index];
        std::unique_ptr<uint8_t[]> ddsData;
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        Check(DirectX::LoadDDSTextureFromFileEx(
            device.Get(), files[index].path, 0, D3D12_RESOURCE_FLAG_NONE, DirectX::DDS_LOADER_DEFAULT,
            texture.GetAddressOf(), ddsData, subresources), files[index].error);

        const UINT count = static_cast<UINT>(subresources.size());
        uploads[index] = UploadBuffer(GetRequiredIntermediateSize(texture.Get(), 0, count));
        if (UpdateSubresources(commands.Get(), texture.Get(), uploads[index].Get(),
            0, 0, count, subresources.data()) == 0) {
            throw std::runtime_error("Texture를 복사하지 못했음");
        }

        Transition(commands.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        const auto textureDesc = texture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = textureDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
        device->CreateShaderResourceView(texture.Get(), &srvDesc,
            Cpu(textureSrvHeap.Get(), index, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    }
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
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->SetGraphicsRootSignature(root.Get());
    ID3D12DescriptorHeap* heaps[] = {textureSrvHeap.Get()};
    commands->SetDescriptorHeaps(_countof(heaps), heaps);
}

void Renderer::BindTexture(TextureId texture) {
    auto handle = textureSrvHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    handle.ptr += UINT64(static_cast<UINT>(texture)) * increment;
    commands->SetGraphicsRootDescriptorTable(1, handle);
}

void Renderer::Draw(const Mesh& mesh, const XMMATRIX& world, const XMFLOAT4& color,
    Pass pass, float animationProgress) {
    if (pass != currentPass) {
        commands->SetPipelineState(pipeline[static_cast<int>(pass)].Get());
        commands->IASetPrimitiveTopology(UsesPointList(pass) ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        currentPass = pass;
    }
    DrawConstants constants{};
    XMStoreFloat4x4(&constants.wvp, world * viewProjection);
    XMStoreFloat4x4(&constants.world, world);
    constants.color = color;
    constants.animationProgress = animationProgress;
    constants.cameraPosition = cameraPosition;
    commands->SetGraphicsRoot32BitConstants(0, ConstantCount, &constants, 0);
    commands->IASetVertexBuffers(0, 1, &mesh.view);
    commands->DrawInstanced(mesh.count, 1, 0, 0);
}

void Renderer::EndFrame() {
    Transition(commands.Get(), targets[backBuffer].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    SubmitCommands();

    const HRESULT presentResult = swap->Present(1, 0);
    // 리소스를 재사용하거나 해제하기 전에 렌더링과 화면 표시가 끝날 때까지 기다린다.
    WaitForGpu();
    Check(presentResult, "Frame을 화면에 표시하지 못했음");
}

