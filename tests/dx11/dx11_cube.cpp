#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <assert.h>
#include <stdint.h>

/* Atomic single-syscall trace: format then issue ONE WriteFile to stderr.
 * fprintf+fflush gets interleaved on iOS where ntdll's syscall trace
 * shares fd 2 with our stdout via dup2, and stdio buffering boundaries
 * don't match write() atomicity. */
#define ATOMIC_TRACE(...) do { \
    char _buf[256]; \
    int _n = snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
    if (_n > 0) { \
        DWORD _written = 0; \
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), _buf, \
                  (DWORD)(_n < (int)sizeof(_buf) ? _n : (int)sizeof(_buf)), \
                  &_written, NULL); \
    } \
} while(0)

#include "3DMaths.h"

static bool global_windowDidResize = false;

// Latest mouse text — rendered as a top-left overlay each frame.
static char global_mouse_text[128] = "(no input yet)";

/* 5x7 bitmap font for the chars used in the mouse text overlay. Each entry
 * is 7 bytes: 5 bits per row, top-aligned. Bit 0x10 = leftmost column. */
struct GlyphRow { unsigned char rows[7]; };
static const struct {
    char ch;
    GlyphRow g;
} kGlyphs[] = {
    {' ', {{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}},
    {'(', {{0x04,0x08,0x10,0x10,0x10,0x08,0x04}}},
    {')', {{0x10,0x08,0x04,0x04,0x04,0x08,0x10}}},
    {'[', {{0x1C,0x10,0x10,0x10,0x10,0x10,0x1C}}},
    {']', {{0x1C,0x04,0x04,0x04,0x04,0x04,0x1C}}},
    {'=', {{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}}},
    {',', {{0x00,0x00,0x00,0x00,0x00,0x08,0x10}}},
    {'.', {{0x00,0x00,0x00,0x00,0x00,0x00,0x08}}},
    {'-', {{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}},
    {'0', {{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}},
    {'1', {{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}},
    {'2', {{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}},
    {'3', {{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}}},
    {'4', {{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}},
    {'5', {{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}},
    {'6', {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}},
    {'7', {{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}},
    {'8', {{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}},
    {'9', {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}},
    {'A', {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}},
    {'B', {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}}},
    {'C', {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}},
    {'D', {{0x1E,0x09,0x09,0x09,0x09,0x09,0x1E}}},
    {'E', {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}},
    {'F', {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}},
    {'G', {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}},
    {'H', {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}}},
    {'I', {{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}},
    {'K', {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}},
    {'L', {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}},
    {'M', {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}},
    {'N', {{0x11,0x11,0x19,0x15,0x13,0x11,0x11}}},
    {'O', {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}},
    {'P', {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}},
    {'R', {{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}},
    {'S', {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}},
    {'T', {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}},
    {'U', {{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}},
    {'V', {{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}},
    {'W', {{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}},
    {'X', {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}},
    {'Y', {{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}},
    {'a', {{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}}},
    {'b', {{0x10,0x10,0x16,0x19,0x11,0x11,0x1E}}},
    {'c', {{0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}}},
    {'d', {{0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}}},
    {'e', {{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}}},
    {'h', {{0x10,0x10,0x16,0x19,0x11,0x11,0x11}}},
    {'l', {{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}}},
    {'m', {{0x00,0x00,0x1A,0x15,0x15,0x15,0x15}}},
    {'n', {{0x00,0x00,0x16,0x19,0x11,0x11,0x11}}},
    {'o', {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}}},
    {'p', {{0x00,0x00,0x16,0x19,0x1E,0x10,0x10}}},
    {'r', {{0x00,0x00,0x16,0x19,0x10,0x10,0x10}}},
    {'s', {{0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}}},
    {'t', {{0x10,0x10,0x1E,0x10,0x10,0x10,0x0E}}},
    {'u', {{0x00,0x00,0x11,0x11,0x11,0x13,0x0D}}},
    {'w', {{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}}},
    {'x', {{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}}},
    {'y', {{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}}},
};

static const GlyphRow* glyph_for(char c) {
    for (size_t i = 0; i < sizeof(kGlyphs)/sizeof(kGlyphs[0]); i++)
        if (kGlyphs[i].ch == c) return &kGlyphs[i].g;
    return &kGlyphs[0].g; // unknown -> space
}

// Input
enum GameAction {
    GameActionMoveCamFwd,
    GameActionMoveCamBack,
    GameActionMoveCamLeft,
    GameActionMoveCamRight,
    GameActionTurnCamLeft,
    GameActionTurnCamRight,
    GameActionLookUp,
    GameActionLookDown,
    GameActionRaiseCam,
    GameActionLowerCam,
    GameActionCount
};
static bool global_keyIsDown[GameActionCount] = {};

bool win32CreateD3D11RenderTargets(ID3D11Device1* d3d11Device, IDXGISwapChain1* swapChain, ID3D11RenderTargetView** d3d11FrameBufferView, ID3D11DepthStencilView** depthBufferView)
{
    ID3D11Texture2D* d3d11FrameBuffer;
    HRESULT hResult = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&d3d11FrameBuffer);
    assert(SUCCEEDED(hResult));

    hResult = d3d11Device->CreateRenderTargetView(d3d11FrameBuffer, 0, d3d11FrameBufferView);
    assert(SUCCEEDED(hResult));

    D3D11_TEXTURE2D_DESC depthBufferDesc;
    d3d11FrameBuffer->GetDesc(&depthBufferDesc);

    d3d11FrameBuffer->Release();

    depthBufferDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthBufferDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* depthBuffer;
    d3d11Device->CreateTexture2D(&depthBufferDesc, nullptr, &depthBuffer);

    d3d11Device->CreateDepthStencilView(depthBuffer, nullptr, depthBufferView);

    depthBuffer->Release();

    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    LRESULT result = 0;
    switch(msg)
    {
        case WM_KEYDOWN:
        case WM_KEYUP:
        {
            bool isDown = (msg == WM_KEYDOWN);
            if(wparam == VK_ESCAPE)
                DestroyWindow(hwnd);
            else if(wparam == 'W')
                global_keyIsDown[GameActionMoveCamFwd] = isDown;
            else if(wparam == 'A')
                global_keyIsDown[GameActionMoveCamLeft] = isDown;
            else if(wparam == 'S')
                global_keyIsDown[GameActionMoveCamBack] = isDown;
            else if(wparam == 'D')
                global_keyIsDown[GameActionMoveCamRight] = isDown;
            else if(wparam == 'E')
                global_keyIsDown[GameActionRaiseCam] = isDown;
            else if(wparam == 'Q')
                global_keyIsDown[GameActionLowerCam] = isDown;
            else if(wparam == VK_UP)
                global_keyIsDown[GameActionLookUp] = isDown;
            else if(wparam == VK_LEFT)
                global_keyIsDown[GameActionTurnCamLeft] = isDown;
            else if(wparam == VK_DOWN)
                global_keyIsDown[GameActionLookDown] = isDown;
            else if(wparam == VK_RIGHT)
                global_keyIsDown[GameActionTurnCamRight] = isDown;
            break;
        }
        case WM_DESTROY:
        {
            PostQuitMessage(0);
            break;
        }
        case WM_SIZE:
        {
            global_windowDidResize = true;
            break;
        }
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        {
            const int x = (int)(short)LOWORD(lparam);
            const int y = (int)(short)HIWORD(lparam);
            const char *what = (msg == WM_LBUTTONDOWN) ? "DOWN" :
                               (msg == WM_LBUTTONUP)   ? "UP"   : "MOVE";
            ATOMIC_TRACE("[cube] mouse %s x=%d y=%d wparam=0x%lx\n",
                         what, x, y, (unsigned long)wparam);
            snprintf(global_mouse_text, sizeof(global_mouse_text),
                     "mouse %s x=%d y=%d wparam=0x%lx",
                     what, x, y, (unsigned long)wparam);
            break;
        }
        default:
            result = DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return result;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nShowCmd*/)
{
    // Open a window
    HWND hwnd;
    {
        WNDCLASSEXW winClass = {};
        winClass.cbSize = sizeof(WNDCLASSEXW);
        winClass.style = CS_HREDRAW | CS_VREDRAW;
        winClass.lpfnWndProc = &WndProc;
        winClass.hInstance = hInstance;
        winClass.hIcon = LoadIconW((HINSTANCE)nullptr, (LPCWSTR)IDI_APPLICATION);
        winClass.hCursor = LoadCursorW((HINSTANCE)nullptr, (LPCWSTR)IDC_ARROW);
        winClass.lpszClassName = L"MyWindowClass";
        winClass.hIconSm = LoadIconW((HINSTANCE)nullptr, (LPCWSTR)IDI_APPLICATION);

        if(!RegisterClassExW(&winClass)) {
            MessageBoxA(0, "RegisterClassEx failed", "Fatal Error", MB_OK);
            return GetLastError();
        }

        RECT initialRect = { 0, 0, 1024, 768 };
        AdjustWindowRectEx(&initialRect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_OVERLAPPEDWINDOW);
        LONG initialWidth = initialRect.right - initialRect.left;
        LONG initialHeight = initialRect.bottom - initialRect.top;

        hwnd = CreateWindowExW(WS_EX_OVERLAPPEDWINDOW,
                                winClass.lpszClassName,
                                L"08. Drawing a Cube",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                initialWidth, 
                                initialHeight,
                                0, 0, hInstance, 0);

        if(!hwnd) {
            MessageBoxA(0, "CreateWindowEx failed", "Fatal Error", MB_OK);
            return GetLastError();
        }
    }

    // Create D3D11 Device and Context
    ID3D11Device1* d3d11Device;
    ID3D11DeviceContext1* d3d11DeviceContext;
    {
        ID3D11Device* baseDevice;
        ID3D11DeviceContext* baseDeviceContext;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        #if defined(DEBUG_BUILD)
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
        #endif

        HRESULT hResult = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE,
                                            0, creationFlags,
                                            featureLevels, ARRAYSIZE(featureLevels),
                                            D3D11_SDK_VERSION, &baseDevice,
                                            0, &baseDeviceContext);
        if(FAILED(hResult)){
            MessageBoxA(0, "D3D11CreateDevice() failed", "Fatal Error", MB_OK);
            return GetLastError();
        }

        // Get 1.1 interface of D3D11 Device and Context
        hResult = baseDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&d3d11Device);
        assert(SUCCEEDED(hResult));
        baseDevice->Release();

        hResult = baseDeviceContext->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&d3d11DeviceContext);
        assert(SUCCEEDED(hResult));
        baseDeviceContext->Release();
    }

#ifdef DEBUG_BUILD
    // Set up debug layer to break on D3D11 errors
    ID3D11Debug *d3dDebug = nullptr;
    d3d11Device->QueryInterface(__uuidof(ID3D11Debug), (void**)&d3dDebug);
    if (d3dDebug)
    {
        ID3D11InfoQueue *d3dInfoQueue = nullptr;
        if (SUCCEEDED(d3dDebug->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&d3dInfoQueue)))
        {
            d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
            d3dInfoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
            d3dInfoQueue->Release();
        }
        d3dDebug->Release();
    }
#endif

    // Create Swap Chain
    IDXGISwapChain1* d3d11SwapChain;
    {
        // Get DXGI Factory (needed to create Swap Chain)
        IDXGIFactory2* dxgiFactory;
        {
            IDXGIDevice1* dxgiDevice;
            HRESULT hResult = d3d11Device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice);
            assert(SUCCEEDED(hResult));

            IDXGIAdapter* dxgiAdapter;
            hResult = dxgiDevice->GetAdapter(&dxgiAdapter);
            assert(SUCCEEDED(hResult));
            dxgiDevice->Release();

            DXGI_ADAPTER_DESC adapterDesc;
            dxgiAdapter->GetDesc(&adapterDesc);

            for (int i = 0; i < 128 && adapterDesc.Description[i]; i++)

            hResult = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);
            assert(SUCCEEDED(hResult));
            dxgiAdapter->Release();
        }
        
        DXGI_SWAP_CHAIN_DESC1 d3d11SwapChainDesc = {};
        d3d11SwapChainDesc.Width = 1024;   // iOS: explicit (no real window)
        d3d11SwapChainDesc.Height = 768;
        d3d11SwapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        d3d11SwapChainDesc.SampleDesc.Count = 1;
        d3d11SwapChainDesc.SampleDesc.Quality = 0;
        d3d11SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d3d11SwapChainDesc.BufferCount = 2;
        d3d11SwapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        d3d11SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        d3d11SwapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        d3d11SwapChainDesc.Flags = 0;

        HRESULT hResult = dxgiFactory->CreateSwapChainForHwnd(d3d11Device, hwnd, &d3d11SwapChainDesc, 0, 0, &d3d11SwapChain);
        assert(SUCCEEDED(hResult));

        dxgiFactory->Release();
    }

    // Create Render Target and Depth Buffer
    ID3D11RenderTargetView* d3d11FrameBufferView;
    ID3D11DepthStencilView* depthBufferView;
    win32CreateD3D11RenderTargets(d3d11Device, d3d11SwapChain, &d3d11FrameBufferView, &depthBufferView);

    UINT shaderCompileFlags = 0;
    // Compiling with this flag allows debugging shaders with Visual Studio
    #if defined(DEBUG_BUILD)
    shaderCompileFlags |= D3DCOMPILE_DEBUG;
    #endif

    // Create Vertex Shader
    ID3DBlob* vsBlob;
    ID3D11VertexShader* vertexShader;
    {
        ID3DBlob* shaderCompileErrorsBlob;
        HRESULT hResult = D3DCompileFromFile(L"shader_cube.hlsl", nullptr, nullptr, "vs_main", "vs_5_0", shaderCompileFlags, 0, &vsBlob, &shaderCompileErrorsBlob);
        if(FAILED(hResult))
        {
            const char* errorString = NULL;
            if(hResult == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                errorString = "Could not compile shader; file not found";
            else if(shaderCompileErrorsBlob){
                errorString = (const char*)shaderCompileErrorsBlob->GetBufferPointer();
                shaderCompileErrorsBlob->Release();
            }
            MessageBoxA(0, errorString, "Shader Compiler Error", MB_ICONERROR | MB_OK);
            return 1;
        }

        hResult = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
        assert(SUCCEEDED(hResult));
    }

    // Create Pixel Shader
    ID3D11PixelShader* pixelShader;
    {
        ID3DBlob* psBlob;
        ID3DBlob* shaderCompileErrorsBlob;
        HRESULT hResult = D3DCompileFromFile(L"shader_cube.hlsl", nullptr, nullptr, "ps_main", "ps_5_0", shaderCompileFlags, 0, &psBlob, &shaderCompileErrorsBlob);
        if(FAILED(hResult))
        {
            const char* errorString = NULL;
            if(hResult == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
                errorString = "Could not compile shader; file not found";
            else if(shaderCompileErrorsBlob){
                errorString = (const char*)shaderCompileErrorsBlob->GetBufferPointer();
                shaderCompileErrorsBlob->Release();
            }
            MessageBoxA(0, errorString, "Shader Compiler Error", MB_ICONERROR | MB_OK);
            return 1;
        }

        hResult = d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
        assert(SUCCEEDED(hResult));
        psBlob->Release();
    }

    // Create Input Layout
    ID3D11InputLayout* inputLayout;
    {
        D3D11_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            { "POS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };

        HRESULT hResult = d3d11Device->CreateInputLayout(inputElementDesc, ARRAYSIZE(inputElementDesc), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
        assert(SUCCEEDED(hResult));
        vsBlob->Release();
    }

    // Create Vertex and Index Buffer
    ID3D11Buffer* vertexBuffer;
    ID3D11Buffer* indexBuffer;
    // UINT numVerts;
    UINT numIndices;
    UINT stride;
    UINT offset;
    {
        float vertexData[] = { // x, y, z
            -0.5f,-0.5f, -0.5f,
            -0.5f,-0.5f,  0.5f,
            -0.5f, 0.5f, -0.5f,
            -0.5f, 0.5f,  0.5f,
            0.5f,-0.5f, -0.5f,
            0.5f,-0.5f,  0.5f,
            0.5f, 0.5f, -0.5f,
            0.5f, 0.5f,  0.5f
        };

        uint16_t indices[] = {
            0, 6, 4,
            0, 2, 6, 
            0, 3, 2, 
            0, 1, 3, 
            2, 7, 6, 
            2, 3, 7, 
            4, 6, 7, 
            4, 7, 5, 
            0, 4, 5, 
            0, 5, 1, 
            1, 5, 7, 
            1, 7, 3  
        };
        stride = 3 * sizeof(float);
        // numVerts = sizeof(vertexData) / stride;
        offset = 0;
        numIndices = sizeof(indices) / sizeof(indices[0]);

        D3D11_BUFFER_DESC vertexBufferDesc = {};
        vertexBufferDesc.ByteWidth = sizeof(vertexData);
        vertexBufferDesc.Usage     = D3D11_USAGE_IMMUTABLE;
        vertexBufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA vertexSubresourceData = { vertexData };

        HRESULT hResult = d3d11Device->CreateBuffer(&vertexBufferDesc, &vertexSubresourceData, &vertexBuffer);
        assert(SUCCEEDED(hResult));

        D3D11_BUFFER_DESC indexBufferDesc = {};
        indexBufferDesc.ByteWidth = sizeof(indices);
        indexBufferDesc.Usage     = D3D11_USAGE_IMMUTABLE;
        indexBufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA indexSubresourceData = { indices };

        hResult = d3d11Device->CreateBuffer(&indexBufferDesc, &indexSubresourceData, &indexBuffer);
        assert(SUCCEEDED(hResult));
    }

    // Create Constant Buffer
    struct Constants
    {
        float4x4 modelViewProj;
    };

    ID3D11Buffer* constantBuffer;
    {
        D3D11_BUFFER_DESC constantBufferDesc = {};
        // ByteWidth must be a multiple of 16, per the docs
        constantBufferDesc.ByteWidth      = sizeof(Constants) + 0xf & 0xfffffff0;
        constantBufferDesc.Usage          = D3D11_USAGE_DYNAMIC;
        constantBufferDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        constantBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hResult = d3d11Device->CreateBuffer(&constantBufferDesc, nullptr, &constantBuffer);
        assert(SUCCEEDED(hResult));
    }

    ID3D11RasterizerState* rasterizerState;
    {
        D3D11_RASTERIZER_DESC rasterizerDesc = {};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_BACK;
        rasterizerDesc.FrontCounterClockwise = TRUE;

        d3d11Device->CreateRasterizerState(&rasterizerDesc, &rasterizerState);
    }

    ID3D11DepthStencilState* depthStencilState;
    {
        D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
        depthStencilDesc.DepthEnable    = TRUE;
        depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc      = D3D11_COMPARISON_LESS;

        d3d11Device->CreateDepthStencilState(&depthStencilDesc, &depthStencilState);
    }

    // Overlay state: depth-disabled DSS + dynamic vertex buffer for text quads.
    // Reuses the cube's existing pipeline (input layout / VS / PS / cbuffer);
    // we just rewrite the cbuffer with an identity MVP so positions pass
    // through to clip space directly. Each on-pixel of bitmap-font text is
    // a single screen-space quad.
    ID3D11DepthStencilState* overlayDSS;
    {
        D3D11_DEPTH_STENCIL_DESC desc = {};
        desc.DepthEnable = FALSE;
        d3d11Device->CreateDepthStencilState(&desc, &overlayDSS);
    }
    // 128 chars × 5 cols × 7 rows × 6 verts/quad × 12 bytes/vert ~= 322KB max
    const UINT kOverlayMaxVerts = 128 * 5 * 7 * 6;
    ID3D11Buffer* overlayVB;
    {
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth      = kOverlayMaxVerts * sizeof(float) * 3;
        desc.Usage          = D3D11_USAGE_DYNAMIC;
        desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        d3d11Device->CreateBuffer(&desc, nullptr, &overlayVB);
    }

    // Camera
    float3 cameraPos = {0, 0, 2};
    float3 cameraFwd = {0, 0, -1};
    float cameraPitch = 0.f;
    float cameraYaw = 0.f;

    float4x4 perspectiveMat = {};
    global_windowDidResize = true; // To force initial perspectiveMat calculation

    // Timing
    LONGLONG startPerfCount = 0;
    LONGLONG perfCounterFrequency = 0;
    {
        LARGE_INTEGER perfCount;
        QueryPerformanceCounter(&perfCount);
        startPerfCount = perfCount.QuadPart;
        LARGE_INTEGER perfFreq;
        QueryPerformanceFrequency(&perfFreq);
        perfCounterFrequency = perfFreq.QuadPart;
    }
    double currentTimeInSeconds = 0.0;

    // Main Loop
    bool isRunning = true;
    while(isRunning)
    {
        float dt;
        {
            double previousTimeInSeconds = currentTimeInSeconds;
            LARGE_INTEGER perfCount;
            QueryPerformanceCounter(&perfCount);

            currentTimeInSeconds = (double)(perfCount.QuadPart - startPerfCount) / (double)perfCounterFrequency;
            dt = (float)(currentTimeInSeconds - previousTimeInSeconds);
            if(dt > (1.f / 60.f))
                dt = (1.f / 60.f);
        }

        // Pump messages — winios.drv now bridges UIKit touches into the
        // Wine queue, so PeekMessage delivers WM_LBUTTONDOWN etc.
        {
            static int peek_call_count = 0;
            static int peek_hit_count = 0;
            peek_call_count++;
            MSG msg = {};
            while(PeekMessageW(&msg, 0, 0, 0, PM_REMOVE))
            {
                peek_hit_count++;
                ATOMIC_TRACE("[cube] PeekMessage got msg=0x%x hwnd=%p (call=%d hit=%d)\n",
                             msg.message, msg.hwnd, peek_call_count, peek_hit_count);
                if(msg.message == WM_QUIT) isRunning = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if ((peek_call_count % 600) == 0) {
                ATOMIC_TRACE("[cube] PeekMessage stats: call=%d hit=%d\n",
                             peek_call_count, peek_hit_count);
            }
        }

        // Get window dimensions (hardcoded on iOS — our Wine has no real
        // window manager, GetClientRect returns uninitialized garbage).
        int windowWidth = 1024, windowHeight = 768;
        float windowAspectRatio = (float)windowWidth / (float)windowHeight;

        if(global_windowDidResize)
        {
            d3d11DeviceContext->OMSetRenderTargets(0, 0, 0);
            d3d11FrameBufferView->Release();
            depthBufferView->Release();

            HRESULT res = d3d11SwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
            assert(SUCCEEDED(res));
            
            win32CreateD3D11RenderTargets(d3d11Device, d3d11SwapChain, &d3d11FrameBufferView, &depthBufferView);
            perspectiveMat = makePerspectiveMat(windowAspectRatio, degreesToRadians(84), 0.1f, 1000.f);

            global_windowDidResize = false;
        }

        // Update camera
        {
            float3 camFwdXZ = normalise((float3){cameraFwd.x, 0, cameraFwd.z});
            float3 cameraRightXZ = cross(camFwdXZ, {0, 1, 0});

            const float CAM_MOVE_SPEED = 5.f; // in metres per second
            const float CAM_MOVE_AMOUNT = CAM_MOVE_SPEED * dt;
            if(global_keyIsDown[GameActionMoveCamFwd])
                cameraPos += camFwdXZ * CAM_MOVE_AMOUNT;
            if(global_keyIsDown[GameActionMoveCamBack])
                cameraPos -= camFwdXZ * CAM_MOVE_AMOUNT;
            if(global_keyIsDown[GameActionMoveCamLeft])
                cameraPos -= cameraRightXZ * CAM_MOVE_AMOUNT;
            if(global_keyIsDown[GameActionMoveCamRight])
                cameraPos += cameraRightXZ * CAM_MOVE_AMOUNT;
            if(global_keyIsDown[GameActionRaiseCam])
                cameraPos.y += CAM_MOVE_AMOUNT;
            if(global_keyIsDown[GameActionLowerCam])
                cameraPos.y -= CAM_MOVE_AMOUNT;
            
            const float CAM_TURN_SPEED = M_PI; // in radians per second
            const float CAM_TURN_AMOUNT = CAM_TURN_SPEED * dt;
            if(global_keyIsDown[GameActionTurnCamLeft])
                cameraYaw += CAM_TURN_AMOUNT;
            if(global_keyIsDown[GameActionTurnCamRight])
                cameraYaw -= CAM_TURN_AMOUNT;
            if(global_keyIsDown[GameActionLookUp])
                cameraPitch += CAM_TURN_AMOUNT;
            if(global_keyIsDown[GameActionLookDown])
                cameraPitch -= CAM_TURN_AMOUNT;

            // Wrap yaw to avoid floating-point errors if we turn too far
            while(cameraYaw >= 2*M_PI) 
                cameraYaw -= 2*M_PI;
            while(cameraYaw <= -2*M_PI) 
                cameraYaw += 2*M_PI;

            // Clamp pitch to stop camera flipping upside down
            if(cameraPitch > degreesToRadians(85)) 
                cameraPitch = degreesToRadians(85);
            if(cameraPitch < -degreesToRadians(85)) 
                cameraPitch = -degreesToRadians(85);
        }

        // Calculate view matrix from camera data
        // 
        // float4x4 viewMat = inverse(rotateXMat(cameraPitch) * rotateYMat(cameraYaw) * translationMat(cameraPos));
        // NOTE: We can simplify this calculation to avoid inverse()!
        // Applying the rule inverse(A*B) = inverse(B) * inverse(A) gives:
        // float4x4 viewMat = inverse(translationMat(cameraPos)) * inverse(rotateYMat(cameraYaw)) * inverse(rotateXMat(cameraPitch));
        // The inverse of a rotation/translation is a negated rotation/translation:
        float4x4 viewMat = translationMat(-cameraPos) * rotateYMat(-cameraYaw) * rotateXMat(-cameraPitch);
        // Update the forward vector we use for camera movement:
        cameraFwd = {-viewMat.m[2][0], -viewMat.m[2][1], -viewMat.m[2][2]};

        // Spin the cube
        float4x4 modelMat = rotateXMat(-0.2f * (float)(M_PI * currentTimeInSeconds)) * rotateYMat(0.1f * (float)(M_PI * currentTimeInSeconds)) ;
        
        // Calculate model-view-projection matrix to send to shader
        float4x4 modelViewProj = modelMat * viewMat * perspectiveMat;

        // Update constant buffer
        D3D11_MAPPED_SUBRESOURCE mappedSubresource;
        d3d11DeviceContext->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSubresource);
        Constants* constants = (Constants*)(mappedSubresource.pData);
        constants->modelViewProj = modelViewProj;
        d3d11DeviceContext->Unmap(constantBuffer, 0);

        FLOAT backgroundColor[4] = { 0.1f, 0.2f, 0.6f, 1.0f };
        d3d11DeviceContext->ClearRenderTargetView(d3d11FrameBufferView, backgroundColor);
        
        d3d11DeviceContext->ClearDepthStencilView(depthBufferView, D3D11_CLEAR_DEPTH, 1.0f, 0);

        D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (FLOAT)windowWidth, (FLOAT)windowHeight, 0.0f, 1.0f };
        d3d11DeviceContext->RSSetViewports(1, &viewport);

        d3d11DeviceContext->RSSetState(rasterizerState);
        d3d11DeviceContext->OMSetDepthStencilState(depthStencilState, 0);

        d3d11DeviceContext->OMSetRenderTargets(1, &d3d11FrameBufferView, depthBufferView);

        d3d11DeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3d11DeviceContext->IASetInputLayout(inputLayout);

        d3d11DeviceContext->VSSetShader(vertexShader, nullptr, 0);
        d3d11DeviceContext->PSSetShader(pixelShader, nullptr, 0);

        d3d11DeviceContext->VSSetConstantBuffers(0, 1, &constantBuffer);

        d3d11DeviceContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        d3d11DeviceContext->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, 0);

        d3d11DeviceContext->DrawIndexed(numIndices, 0, 0);

        // ---- Text overlay: draw global_mouse_text in top-left corner ----
        {
            // Rasterize text into the dynamic overlay vertex buffer. Each
            // bitmap-font on-pixel becomes one quad (6 verts) in NDC.
            const float pixel_w = 6.0f / (float)windowWidth;   // glyph col in NDC
            const float pixel_h = 6.0f / (float)windowHeight;  // glyph row in NDC
            const float origin_x = -1.0f + pixel_w * 1.0f;     // a little inset
            const float origin_y =  1.0f - pixel_h * 1.0f;
            const float char_step_x = pixel_w * 6.0f;          // 5 cols + 1 gap

            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(d3d11DeviceContext->Map(overlayVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                float* p = (float*)m.pData;
                UINT vert_count = 0;
                const char *s = global_mouse_text;
                int char_idx = 0;
                while (*s && vert_count + 6 * 5 * 7 <= kOverlayMaxVerts)
                {
                    char c = *s++;
                    const GlyphRow *g = glyph_for(c);
                    float gx = origin_x + char_step_x * (float)char_idx;
                    for (int row = 0; row < 7; row++)
                    {
                        unsigned char bits = g->rows[row];
                        for (int col = 0; col < 5; col++)
                        {
                            if (!(bits & (0x10 >> col))) continue;
                            float x0 = gx + pixel_w * (float)col;
                            float y0 = origin_y - pixel_h * (float)row;
                            float x1 = x0 + pixel_w * 0.95f;
                            float y1 = y0 - pixel_h * 0.95f;
                            // Two triangles, 6 verts (pos.xyz only). Cube
                            // uses FrontCounterClockwise=TRUE + cull-back,
                            // so wind these CCW in NDC (y-up).
                            float quad[18] = {
                                x0, y0, 0.0f,  x0, y1, 0.0f,  x1, y0, 0.0f,
                                x1, y0, 0.0f,  x0, y1, 0.0f,  x1, y1, 0.0f,
                            };
                            for (int k = 0; k < 18; k++) p[k] = quad[k];
                            p += 18;
                            vert_count += 6;
                        }
                    }
                    char_idx++;
                }
                d3d11DeviceContext->Unmap(overlayVB, 0);

                if (vert_count > 0)
                {
                    // Write identity MVP so vertex positions pass straight to clip space.
                    D3D11_MAPPED_SUBRESOURCE cm;
                    if (SUCCEEDED(d3d11DeviceContext->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm)))
                    {
                        float4x4 identity = {};
                        identity.m[0][0] = 1.0f; identity.m[1][1] = 1.0f;
                        identity.m[2][2] = 1.0f; identity.m[3][3] = 1.0f;
                        ((Constants*)cm.pData)->modelViewProj = identity;
                        d3d11DeviceContext->Unmap(constantBuffer, 0);
                    }
                    d3d11DeviceContext->OMSetDepthStencilState(overlayDSS, 0);
                    UINT ovStride = sizeof(float) * 3, ovOffset = 0;
                    d3d11DeviceContext->IASetVertexBuffers(0, 1, &overlayVB, &ovStride, &ovOffset);
                    d3d11DeviceContext->Draw(vert_count, 0);
                }
            }
        }

        d3d11SwapChain->Present(1, 0);
    }

    return 0;
}