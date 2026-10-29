#include <d3d11.h>
#include <dxgi.h>
#include <cassert>
#include <initializer_list>

// Run against the iOS DXMT port: handles stay valid while the originating texture lives.
int main() {
  ID3D11Device *a = nullptr, *b = nullptr;
  assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION, &a, nullptr, nullptr)));
  assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION, &b, nullptr, nullptr)));
  for (UINT flags : {UINT(0), UINT(D3D11_RESOURCE_MISC_SHARED)}) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = flags;
    ID3D11Texture2D *source = nullptr;
    assert(SUCCEEDED(a->CreateTexture2D(&desc, nullptr, &source)));
    IDXGIResource *resource = nullptr;
    assert(SUCCEEDED(source->QueryInterface(__uuidof(IDXGIResource), (void **)&resource)));
    HANDLE handle = nullptr;
    assert(SUCCEEDED(resource->GetSharedHandle(&handle)) && handle);
    for (auto device : {a, b}) {
      ID3D11Texture2D *opened = nullptr;
      assert(SUCCEEDED(device->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&opened)));
      D3D11_TEXTURE2D_DESC actual = {};
      opened->GetDesc(&actual);
      assert(actual.Width == desc.Width && actual.Format == desc.Format);
      opened->Release();
    }
    resource->Release();
    source->Release();
    ID3D11Texture2D *retired = nullptr;
    assert(FAILED(b->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&retired)));
  }
  a->Release();
  b->Release();
}
