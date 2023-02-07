#include "pch.h"
#include "SimpleCapture.h"

#define _WIN32_WINNT 0x600
#include <stdio.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")

namespace winrt {
    using namespace Windows::Foundation;
    using namespace Windows::Foundation::Numerics;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
    using namespace Windows::System;
    using namespace Windows::UI;
    using namespace Windows::UI::Composition;
}

namespace util {
    using namespace robmikh::common::uwp;
}

SimpleCapture::SimpleCapture(winrt::IDirect3DDevice const& device, winrt::GraphicsCaptureItem const& item, winrt::DirectXPixelFormat pixelFormat) {
    m_item = item;
    m_device = device;
    m_pixelFormat = pixelFormat;

    auto d3dDevice = GetDXGIInterfaceFromObject<ID3D11Device>(m_device);
    d3dDevice->GetImmediateContext(m_d3dContext.put());

    m_swapChain = util::CreateDXGISwapChain(d3dDevice, static_cast<uint32_t>(m_item.Size().Width), static_cast<uint32_t>(m_item.Size().Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 2);

    // Creating our frame pool with 'Create' instead of 'CreateFreeThreaded'
    // means that the frame pool's FrameArrived event is called on the thread
    // the frame pool was created on. This also means that the creating thread
    // must have a DispatcherQueue. If you use this method, it's best not to do
    // it on the UI thread. 
    m_framePool = winrt::Direct3D11CaptureFramePool::Create(m_device, m_pixelFormat, 2, m_item.Size());
    m_session = m_framePool.CreateCaptureSession(m_item);
    m_lastSize = m_item.Size();
    m_framePool.FrameArrived({ this, &SimpleCapture::OnFrameArrived });
}

void SimpleCapture::StartCapture() {
    CheckClosed();
    m_session.StartCapture();
}

winrt::ICompositionSurface SimpleCapture::CreateSurface(winrt::Compositor const& compositor) {
    CheckClosed();
    return util::CreateCompositionSurfaceForSwapChain(compositor, m_swapChain.get());
}

void SimpleCapture::Close(){
    auto expected = false;
    if (m_closed.compare_exchange_strong(expected, true)) {
        m_session.Close();
        m_framePool.Close();

        m_swapChain = nullptr;
        m_framePool = nullptr;
        m_session = nullptr;
        m_item = nullptr;
    }
}

void SimpleCapture::ResizeSwapChain() {
    winrt::check_hresult(m_swapChain->ResizeBuffers(2, static_cast<uint32_t>(m_lastSize.Width), static_cast<uint32_t>(m_lastSize.Height),
        static_cast<DXGI_FORMAT>(m_pixelFormat), 0));
}

bool SimpleCapture::TryResizeSwapChain(winrt::Direct3D11CaptureFrame const& frame) {
    auto const contentSize = frame.ContentSize();
    if ((contentSize.Width != m_lastSize.Width) || (contentSize.Height != m_lastSize.Height)) {
        // The thing we have been capturing has changed size, resize the swap chain to match.
        m_lastSize = contentSize;
        ResizeSwapChain();
        return true;
    }
    return false;
}

bool SimpleCapture::TryUpdatePixelFormat() {
    auto newFormat = m_pixelFormatUpdate.exchange(std::nullopt);
    if (newFormat.has_value()) {
        auto pixelFormat = newFormat.value();
        if (pixelFormat != m_pixelFormat) {
            m_pixelFormat = pixelFormat;
            ResizeSwapChain();
            return true;
        }
    }
    return false;
}

// copied straight from
// https://learn.microsoft.com/de-de/windows/win32/direct3d11/direct3d-11-advanced-stages-compute-create?source=recommendations
HRESULT SimpleCapture::CompileComputeShader(_In_ LPCWSTR srcFile, _In_ LPCSTR entryPoint, _In_ ID3D11Device* device, _Outptr_ ID3DBlob** blob) {
    if (!srcFile || !entryPoint || !device || !blob) {
        return E_INVALIDARG;
    }

    *blob = nullptr;

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
    
    #if defined( DEBUG ) || defined( _DEBUG )
        flags |= D3DCOMPILE_DEBUG;
    #endif

    // We generally prefer to use the higher CS shader profile when possible as CS 5.0 is better performance on 11-class hardware
    LPCSTR profile = (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0) ? "cs_5_0" : "cs_4_0";

    const D3D_SHADER_MACRO defines[] = {
        "EXAMPLE_DEFINE", "1",
        NULL, NULL
    };

    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompileFromFile(srcFile, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, profile, flags, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }

        if (shaderBlob) {
            shaderBlob->Release();
        }

        return hr;
    }

    *blob = shaderBlob;

    return hr;
}

void SimpleCapture::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::IInspectable const&) {
    auto swapChainResizedToFrame = false;
    
    auto frame = sender.TryGetNextFrame();
    
    swapChainResizedToFrame = TryResizeSwapChain(frame);

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    winrt::check_hresult(m_swapChain->GetBuffer(0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));
    auto surfaceTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
    

    // copy surfaceTexture to backBuffer
    m_d3dContext->CopyResource(backBuffer.get(), surfaceTexture.get());

	// ==================== compute shader ====================

    auto device = GetDXGIInterfaceFromObject<ID3D11Texture2D>(m_device);
    winrt::com_ptr<ID3D11ShaderResourceView> surface_srv;

    winrt::check_hresult(device->CreateShaderResourceView(surfaceTexture, nullptr, srv_desc.put()));

    // Compile shader
    // TODO: I probably only want to do this once?
    winrt::com_ptr<ID3DBlob> csBlob {};
	winrt::check_hresult(CompileComputeShader(L"ExampleCompute.hlsl", "CSMain", device.get(), csBlob.put()));

    // Create shader
    //https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createcomputeshader
    /*
    HRESULT CreateComputeShader(
        [in]            const void          *pShaderBytecode,
        [in]            SIZE_T              BytecodeLength,
        [in, optional]  ID3D11ClassLinkage  *pClassLinkage,
        [out, optional] ID3D11ComputeShader **ppComputeShader
    );
    */
    winrt::com_ptr<ID3D11ComputeShader> computeShader {};
    winrt::check_hresult(device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, computeShader.put()));

    m_d3dContext->CSSetShader(computeShader.get(), nullptr, 0);
    const ID3D11ShaderResourceView* temp = surface_srv.get();
    m_d3dContext->CSSetShaderResources(0, 1, &temp);

    // TODO: UAV texture

    const ID3D11UnorderedAccessView* temp_uav = target_uav.get();
    m_d3dContext->CSSetUnorderedAccessView(0, 1, &temp_uav);

    m_d3dContext->Dispatch()

    

	// TODO: give compute shader access to backBuffer right?
    
    
	// TODO: actually run the shader

    
    
    
	// ==================== compute shader ====================

    // rendering to ui element... I think

    DXGI_PRESENT_PARAMETERS presentParameters{};
    m_swapChain->Present1(1, 0, &presentParameters);

    swapChainResizedToFrame = swapChainResizedToFrame || TryUpdatePixelFormat();

    if (swapChainResizedToFrame) {
        m_framePool.Recreate(m_device, m_pixelFormat, 2, m_lastSize);
    }
}
