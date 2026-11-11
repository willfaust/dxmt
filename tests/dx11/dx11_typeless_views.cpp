#include <d3d11.h>
#include <cassert>
#include <initializer_list>

// A colour typeless texture must still support every compatible typed view.
int main() {
  ID3D11Device *device = nullptr;
  assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
    nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr)));
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = desc.Height = 16;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *texture = nullptr;
  assert(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
  for (auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                     DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT}) {
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *srv = nullptr;
    assert(SUCCEEDED(device->CreateShaderResourceView(texture, &view, &srv)));
    srv->Release();
  }
  texture->Release();
  device->Release();
}
