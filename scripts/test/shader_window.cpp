// Run extracted production WPOS HLSL on WARP and read its coordinates back.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cassert>
#include <cstdio>
#include <vector>
#include <fstream>
#include <iterator>
#include "rsx_commands.h"
#include "decoder.inc"

int main(int argc, char** argv) {
    assert(argc == 3);
    rsx_state state = {};
    assert(decode(&state, NV4097_SET_SHADER_WINDOW, 0x1438) == 0);
    assert(state.shader_window == 0x1438);
    assert(decode(&state, NV4097_SET_SHADER_WINDOW, 0x11438) == 0);
    assert(state.shader_window == 0x11438);
    std::ifstream input(argv[1]);
    std::string source((std::istreambuf_iterator<char>(input)), {});
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source.data(), source.size(), "window-test", nullptr, nullptr,
                           "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (errors) { fprintf(stderr, "%s", (char*)errors->GetBufferPointer()); errors->Release(); }
    assert(SUCCEEDED(hr));
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, &context)));
    ID3D11ComputeShader* shader = nullptr;
    assert(SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                               nullptr, &shader)));
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = 64 * 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = 16;
    ID3D11Buffer *result = nullptr, *staging = nullptr;
    assert(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &result)));
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = 64;
    ID3D11UnorderedAccessView* uav = nullptr;
    assert(SUCCEEDED(device->CreateUnorderedAccessView(result, &view, &uav)));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging)));
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch(1, 1, 1);
    ID3D11UnorderedAccessView* none = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
    context->CopyResource(staging, result);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    assert(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)));
    FILE* out = fopen(argv[2], "wb"); assert(out);
    assert(fwrite(mapped.pData, 1, desc.ByteWidth, out) == desc.ByteWidth); fclose(out);
    context->Unmap(staging, 0);
    staging->Release(); uav->Release(); result->Release(); shader->Release();
    context->Release(); device->Release(); blob->Release();
}
