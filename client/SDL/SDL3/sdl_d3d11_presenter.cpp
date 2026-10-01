/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client, Direct3D 11 presenter
 *
 * Copyright 2026 The FreeRDP Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sdl_d3d11_presenter.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

namespace
{
	/* The mingw headers ship wrl only in newer releases, and all that is
	 * needed is Release() on scope exit. */
	template <typename T> class ComPtr
	{
	  public:
		ComPtr() = default;
		ComPtr(const ComPtr& other) = delete;
		ComPtr& operator=(const ComPtr& other) = delete;
		~ComPtr()
		{
			reset();
		}

		void reset()
		{
			if (_ptr)
				_ptr->Release();
			_ptr = nullptr;
		}

		[[nodiscard]] T* get() const
		{
			return _ptr;
		}

		T* operator->() const
		{
			return _ptr;
		}

		explicit operator bool() const
		{
			return _ptr != nullptr;
		}

		/* For out parameters; drops what is held. */
		T** put()
		{
			reset();
			return &_ptr;
		}

	  private:
		T* _ptr = nullptr;
	};

	bool failed(HRESULT hr, const char* what)
	{
		if (SUCCEEDED(hr))
			return false;
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: %s failed: 0x%08lx", what,
		             static_cast<unsigned long>(hr));
		return true;
	}

	/* One triangle covering the viewport, generated from the vertex id so no
	 * vertex buffer or input layout is needed. */
	const char vertexShader[] =
	    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	    "VSOut main(uint id : SV_VertexID) {\n"
	    "  VSOut o;\n"
	    "  o.uv = float2((id << 1) & 2, id & 2);\n"
	    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	    "  return o;\n"
	    "}\n";

	const char overlayShader[] =
	    "Texture2D tex : register(t0);\n"
	    "SamplerState smp : register(s0);\n"
	    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
	    "  return tex.Sample(smp, uv);\n"
	    "}\n";

	/* Alpha is forced: the X in BGRX is not guaranteed to be 0xff. */
	const char pixelShader[] =
	    "Texture2D tex : register(t0);\n"
	    "SamplerState smp : register(s0);\n"
	    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
	    "  return float4(tex.Sample(smp, uv).rgb, 1);\n"
	    "}\n";

	using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
	                                   ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
	                                   ID3DBlob**);

	/* Loaded at run time rather than linked, so a system without the compiler
	 * DLL falls back to the SDL renderer instead of failing to start. */
	CompileFn loadCompiler()
	{
		static HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
		if (!module)
			return nullptr;
		return reinterpret_cast<CompileFn>(
		    reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
	}

	bool compile(const char* source, size_t length, const char* target, ComPtr<ID3DBlob>& blob)
	{
		const auto fn = loadCompiler();
		if (!fn)
		{
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: no d3dcompiler_47.dll");
			return false;
		}

		ComPtr<ID3DBlob> errors;
		const auto hr = fn(source, length, nullptr, nullptr, nullptr, "main", target, 0, 0,
		                   blob.put(), errors.put());
		if (FAILED(hr) && errors)
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: %s",
			             static_cast<const char*>(errors->GetBufferPointer()));
		return !failed(hr, "D3DCompile");
	}

	DXGI_FORMAT textureFormat(SDL_PixelFormat format)
	{
		switch (format)
		{
			case SDL_PIXELFORMAT_BGRA32:
			case SDL_PIXELFORMAT_BGRX32:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case SDL_PIXELFORMAT_RGBA32:
			case SDL_PIXELFORMAT_RGBX32:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			default:
				return DXGI_FORMAT_UNKNOWN;
		}
	}
} // namespace

struct SdlD3D11Presenter::Impl
{
	/* Enough that the staging texture mapped for a frame is never one the GPU
	 * is still copying out of from an earlier frame. */
	static constexpr size_t stagingCount = 3;

	HWND hwnd = nullptr;
	bool vsync = true;
	bool tearing = false;
	UINT swapFlags = 0;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGISwapChain2> swapChain;
	ComPtr<ID3D11RenderTargetView> backBuffer;
	ComPtr<ID3D11VertexShader> vs;
	ComPtr<ID3D11PixelShader> ps;
	ComPtr<ID3D11PixelShader> overlayPs;
	ComPtr<ID3D11BlendState> overlayBlend;
	ComPtr<ID3D11SamplerState> sampler;
	ComPtr<ID3D11ShaderResourceView> overlayView;
	int overlayW = 0;
	int overlayH = 0;

	/* Set when a call fails because the device is gone. Everything is
	 * released and set up again, at most once a second. */
	bool lost = false;
	bool redraw = false;
	Uint64 lastRecover = 0;
	HANDLE waitable = nullptr;
	UINT backW = 0;
	UINT backH = 0;

	/* The desktop, and the staging ring it is filled through. */
	ComPtr<ID3D11Texture2D> frame;
	ComPtr<ID3D11ShaderResourceView> frameView;
	std::array<ComPtr<ID3D11Texture2D>, stagingCount> staging;
	size_t stagingNext = 0;
	int frameW = 0;
	int frameH = 0;
	DXGI_FORMAT frameFormat = DXGI_FORMAT_UNKNOWN;

	SDL_Point offset{ 0, 0 };
	SDL_FPoint scale{ 1.0f, 1.0f };

	~Impl()
	{
		release();
	}

	void release();
	void checkLost();
	bool recover();
	bool init();
	bool createBackBufferView();
	bool resizeIfNeeded();
	bool ensureFrame(int w, int h, DXGI_FORMAT format, bool& recreated);
};

void SdlD3D11Presenter::Impl::release()
{
	if (context)
		context->ClearState();
	if (waitable)
		CloseHandle(waitable);
	waitable = nullptr;

	overlayView.reset();
	frameView.reset();
	frame.reset();
	for (auto& texture : staging)
		texture.reset();
	frameW = 0;
	frameH = 0;
	sampler.reset();
	overlayBlend.reset();
	overlayPs.reset();
	ps.reset();
	vs.reset();
	backBuffer.reset();
	swapChain.reset();
	context.reset();
	device.reset();
}

/* Called when something failed: only a removed device is worth a new start. */
void SdlD3D11Presenter::Impl::checkLost()
{
	if (!device || lost)
		return;
	const auto reason = device->GetDeviceRemovedReason();
	if (reason == S_OK)
		return;
	SDL_LogWarn(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: device lost (0x%08lx)",
	            static_cast<unsigned long>(reason));
	lost = true;
}

bool SdlD3D11Presenter::Impl::recover()
{
	const auto now = SDL_GetTicksNS();
	if ((lastRecover != 0) && (now - lastRecover < 1000000000ull))
		return false;
	lastRecover = now;

	release();
	if (!init())
	{
		release();
		return false;
	}

	SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: device set up again");
	lost = false;
	redraw = true;
	return true;
}

bool SdlD3D11Presenter::Impl::init()
{
	const std::array<D3D_FEATURE_LEVEL, 4> levels = { D3D_FEATURE_LEVEL_11_1,
		                                              D3D_FEATURE_LEVEL_11_0,
		                                              D3D_FEATURE_LEVEL_10_1,
		                                              D3D_FEATURE_LEVEL_10_0 };
	auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels.data(),
	                            static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, device.put(),
	                            nullptr, context.put());
	if (hr == E_INVALIDARG)
	{
		/* Systems without the 11.1 runtime reject the whole list. */
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels.data() + 1,
		                       static_cast<UINT>(levels.size() - 1), D3D11_SDK_VERSION,
		                       device.put(), nullptr, context.put());
	}
	if (failed(hr, "D3D11CreateDevice"))
		return false;

	ComPtr<IDXGIDevice1> dxgiDevice;
	if (failed(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())), "IDXGIDevice1"))
		return false;
	ComPtr<IDXGIAdapter> adapter;
	if (failed(dxgiDevice->GetAdapter(adapter.put()), "GetAdapter"))
		return false;
	ComPtr<IDXGIFactory2> factory;
	if (failed(adapter->GetParent(IID_PPV_ARGS(factory.put())), "IDXGIFactory2"))
		return false;

	ComPtr<IDXGIFactory5> factory5;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory5.put()))))
	{
		BOOL allow = FALSE;
		if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
		                                            sizeof(allow))))
			tearing = (allow != FALSE);
	}

	swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	if (tearing)
		swapFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = swapFlags;

	ComPtr<IDXGISwapChain1> swapChain1;
	if (failed(factory->CreateSwapChainForHwnd(device.get(), hwnd, &desc, nullptr, nullptr,
	                                           swapChain1.put()),
	           "CreateSwapChainForHwnd"))
		return false;
	if (failed(swapChain1->QueryInterface(IID_PPV_ARGS(swapChain.put())), "IDXGISwapChain2"))
		return false;

	/* SDL owns the window, including alt+enter. */
	(void)factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);

	/* One queued frame: with more, a frame waits behind older ones. */
	(void)swapChain->SetMaximumFrameLatency(1);
	waitable = swapChain->GetFrameLatencyWaitableObject();
	if (!waitable)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: no frame latency waitable");
		return false;
	}

	if (!createBackBufferView())
		return false;

	ComPtr<ID3DBlob> blob;
	if (!compile(vertexShader, sizeof(vertexShader) - 1, "vs_4_0", blob))
		return false;
	if (failed(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      vs.put()),
	           "CreateVertexShader"))
		return false;
	if (!compile(pixelShader, sizeof(pixelShader) - 1, "ps_4_0", blob))
		return false;
	if (failed(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     ps.put()),
	           "CreatePixelShader"))
		return false;
	if (!compile(overlayShader, sizeof(overlayShader) - 1, "ps_4_0", blob))
		return false;
	if (failed(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     overlayPs.put()),
	           "CreatePixelShader (overlay)"))
		return false;

	/* Premultiplied alpha. */
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (failed(device->CreateBlendState(&blendDesc, overlayBlend.put()), "CreateBlendState"))
		return false;

	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	return !failed(device->CreateSamplerState(&samplerDesc, sampler.put()), "CreateSamplerState");
}

bool SdlD3D11Presenter::Impl::createBackBufferView()
{
	ComPtr<ID3D11Texture2D> texture;
	if (failed(swapChain->GetBuffer(0, IID_PPV_ARGS(texture.put())), "GetBuffer"))
		return false;

	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	backW = desc.Width;
	backH = desc.Height;
	return !failed(device->CreateRenderTargetView(texture.get(), nullptr, backBuffer.put()),
	               "CreateRenderTargetView");
}

bool SdlD3D11Presenter::Impl::resizeIfNeeded()
{
	RECT rc = {};
	if (!GetClientRect(hwnd, &rc))
		return true;

	const auto w = static_cast<UINT>(rc.right - rc.left);
	const auto h = static_cast<UINT>(rc.bottom - rc.top);
	/* Minimized. Keep what there is. */
	if ((w == 0) || (h == 0))
		return true;
	if ((w == backW) && (h == backH))
		return true;

	/* Every reference to the old buffers has to go first. */
	context->OMSetRenderTargets(0, nullptr, nullptr);
	backBuffer.reset();
	if (failed(swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags), "ResizeBuffers"))
		return false;
	return createBackBufferView();
}

bool SdlD3D11Presenter::Impl::ensureFrame(int w, int h, DXGI_FORMAT format, bool& recreated)
{
	recreated = false;
	if (frame && (frameW == w) && (frameH == h) && (frameFormat == format))
		return true;

	frameView.reset();
	frame.reset();
	for (auto& texture : staging)
		texture.reset();
	frameW = 0;
	frameH = 0;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = static_cast<UINT>(w);
	desc.Height = static_cast<UINT>(h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (failed(device->CreateTexture2D(&desc, nullptr, frame.put()), "CreateTexture2D (frame)"))
		return false;
	if (failed(device->CreateShaderResourceView(frame.get(), nullptr, frameView.put()),
	           "CreateShaderResourceView"))
		return false;

	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	for (auto& texture : staging)
	{
		if (failed(device->CreateTexture2D(&desc, nullptr, texture.put()),
		           "CreateTexture2D (staging)"))
			return false;
	}

	frameW = w;
	frameH = h;
	frameFormat = format;
	stagingNext = 0;
	recreated = true;
	return true;
}

SdlD3D11Presenter::SdlD3D11Presenter(std::unique_ptr<Impl> impl) : _impl(std::move(impl))
{
}

SdlD3D11Presenter::~SdlD3D11Presenter() = default;

std::unique_ptr<SdlD3D11Presenter> SdlD3D11Presenter::create(void* hwnd, bool vsync)
{
	if (!hwnd)
		return nullptr;

	auto impl = std::make_unique<Impl>();
	impl->hwnd = static_cast<HWND>(hwnd);
	impl->vsync = vsync;
	if (!impl->init())
		return nullptr;
	return std::unique_ptr<SdlD3D11Presenter>(new SdlD3D11Presenter(std::move(impl)));
}

void SdlD3D11Presenter::setMapping(SDL_Point offset, SDL_FPoint scale)
{
	_impl->offset = offset;
	_impl->scale = scale;
}

bool SdlD3D11Presenter::upload(SDL_Surface* surface, const std::vector<SDL_Rect>& rects,
                               Uint64* bytes)
{
	if (!surface || !surface->pixels)
		return false;

	const auto format = textureFormat(surface->format);
	if (format == DXGI_FORMAT_UNKNOWN)
	{
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: unsupported pixel format %s",
		             SDL_GetPixelFormatName(surface->format));
		return false;
	}

	/* Nothing to draw into until the device is back; not the caller's problem. */
	if (_impl->lost && !_impl->recover())
		return true;

	bool recreated = false;
	if (!_impl->ensureFrame(surface->w, surface->h, format, recreated))
	{
		_impl->checkLost();
		return false;
	}

	/* A new texture has no contents, so partial damage is not enough. */
	const SDL_Rect full{ 0, 0, surface->w, surface->h };
	std::vector<SDL_Rect> clipped;
	if (rects.empty() || recreated)
		clipped.push_back(full);
	else
	{
		for (const auto& rect : rects)
		{
			SDL_Rect r{};
			if (SDL_GetRectIntersection(&rect, &full, &r))
				clipped.push_back(r);
		}
	}
	if (clipped.empty())
		return true;

	auto context = _impl->context.get();
	auto staging = _impl->staging[_impl->stagingNext].get();
	_impl->stagingNext = (_impl->stagingNext + 1) % Impl::stagingCount;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (failed(context->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped), "Map"))
	{
		_impl->checkLost();
		return false;
	}

	const size_t bpp = 4;
	Uint64 copied = 0;
	for (const auto& r : clipped)
	{
		const auto* src = static_cast<const uint8_t*>(surface->pixels) +
		                  (static_cast<size_t>(r.y) * static_cast<size_t>(surface->pitch)) +
		                  (static_cast<size_t>(r.x) * bpp);
		auto* dst = static_cast<uint8_t*>(mapped.pData) +
		            (static_cast<size_t>(r.y) * mapped.RowPitch) + (static_cast<size_t>(r.x) * bpp);
		const auto length = static_cast<size_t>(r.w) * bpp;

		if ((r.w == surface->w) && (mapped.RowPitch == static_cast<UINT>(surface->pitch)))
			memcpy(dst, src, static_cast<size_t>(surface->pitch) * static_cast<size_t>(r.h));
		else
		{
			for (int y = 0; y < r.h; y++)
			{
				memcpy(dst, src, length);
				src += surface->pitch;
				dst += mapped.RowPitch;
			}
		}
		copied += length * static_cast<size_t>(r.h);
	}
	context->Unmap(staging, 0);

	for (const auto& r : clipped)
	{
		const D3D11_BOX box{ static_cast<UINT>(r.x),       static_cast<UINT>(r.y),       0,
			                 static_cast<UINT>(r.x + r.w), static_cast<UINT>(r.y + r.h), 1 };
		context->CopySubresourceRegion(_impl->frame.get(), 0, box.left, box.top, 0, staging, 0,
		                               &box);
	}

	/* Start the copies now: the caller is about to wait for the swap chain. */
	context->Flush();

	if (bytes)
		*bytes = copied;
	return true;
}

void* SdlD3D11Presenter::waitHandle() const
{
	return _impl->waitable;
}

bool SdlD3D11Presenter::setOverlay(SDL_Surface* surface)
{
	_impl->overlayView.reset();
	if (!surface || _impl->lost)
		return true;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = static_cast<UINT>(surface->w);
	desc.Height = static_cast<UINT>(surface->h);
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data = {};
	data.pSysMem = surface->pixels;
	data.SysMemPitch = static_cast<UINT>(surface->pitch);

	ComPtr<ID3D11Texture2D> texture;
	if (failed(_impl->device->CreateTexture2D(&desc, &data, texture.put()),
	           "CreateTexture2D (overlay)"))
		return false;
	if (failed(_impl->device->CreateShaderResourceView(texture.get(), nullptr,
	                                                   _impl->overlayView.put()),
	           "CreateShaderResourceView (overlay)"))
		return false;
	_impl->overlayW = surface->w;
	_impl->overlayH = surface->h;
	return true;
}

bool SdlD3D11Presenter::takeRedrawRequest()
{
	const auto redraw = _impl->redraw;
	_impl->redraw = false;
	return redraw;
}

bool SdlD3D11Presenter::draw()
{
	if (_impl->lost)
		return false;
	if (!_impl->resizeIfNeeded())
	{
		_impl->checkLost();
		return false;
	}

	auto context = _impl->context.get();
	auto target = _impl->backBuffer.get();
	if (!target)
		return false;

	/* The flip model unbinds the back buffer on every present. */
	context->OMSetRenderTargets(1, &target, nullptr);
	const std::array<float, 4> black = { 0.0f, 0.0f, 0.0f, 1.0f };
	context->ClearRenderTargetView(target, black.data());

	if (!_impl->frameView)
		return true;

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(_impl->offset.x);
	viewport.TopLeftY = static_cast<float>(_impl->offset.y);
	viewport.Width = static_cast<float>(_impl->frameW) * _impl->scale.x;
	viewport.Height = static_cast<float>(_impl->frameH) * _impl->scale.y;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);

	auto view = _impl->frameView.get();
	auto sampler = _impl->sampler.get();
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(_impl->vs.get(), nullptr, 0);
	context->PSSetShader(_impl->ps.get(), nullptr, 0);
	context->PSSetShaderResources(0, 1, &view);
	context->PSSetSamplers(0, 1, &sampler);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->Draw(3, 0);

	if (_impl->overlayView)
	{
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = static_cast<float>(_impl->overlayW);
		viewport.Height = static_cast<float>(_impl->overlayH);
		context->RSSetViewports(1, &viewport);

		view = _impl->overlayView.get();
		context->PSSetShader(_impl->overlayPs.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &view);
		context->OMSetBlendState(_impl->overlayBlend.get(), nullptr, 0xffffffff);
		context->Draw(3, 0);
	}
	return true;
}

bool SdlD3D11Presenter::present()
{
	const UINT interval = _impl->vsync ? 1 : 0;
	const UINT flags = (!_impl->vsync && _impl->tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
	if (_impl->lost)
		return false;
	const auto hr = _impl->swapChain->Present(interval, flags);
	/* Occluded and friends are success codes. */
	if (!failed(hr, "Present"))
		return true;
	_impl->checkLost();
	return false;
}
