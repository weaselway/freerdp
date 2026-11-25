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
#include <d3d11_4.h>
#include <d3d12.h>
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

	/* Premultiplied, so fading scales all four channels. */
	const char overlayShader[] =
	    "Texture2D tex : register(t0);\n"
	    "SamplerState smp : register(s0);\n"
	    "cbuffer fade : register(b0) { float alpha; };\n"
	    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
	    "  return tex.Sample(smp, uv) * alpha;\n"
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
	/* The overlay shader's alpha. */
	ComPtr<ID3D11Buffer> overlayFade;
	ComPtr<ID3D11SamplerState> sampler;
	struct Overlay
	{
		ComPtr<ID3D11ShaderResourceView> view;
		int w = 0;
		int h = 0;
	};
	Overlay overlay;
	Overlay notification;
	bool notificationBottom = false;
	/* When the notification began to fade in, or out with fadingOut. */
	Uint64 notificationSince = 0;
	bool notificationFadingOut = false;
	/* The last draw() was in the middle of a fade. */
	bool animating = false;
	/* One opaque pixel in the damage colour, stretched over each edge. */
	ComPtr<ID3D11ShaderResourceView> damageView;
	std::vector<SDL_Rect> damage;
	bool setOverlay(Overlay& target, SDL_Surface* surface);

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

	/* Zero copy: a Direct3D 12 device on the same adapter copies from the
	 * shared memory section into frame, which it opens through an NT handle.
	 * One fence, shared by both devices, orders their work on frame. Set up
	 * in init() when wanted; zeroCopy is false when that or a later copy
	 * failed. */
	bool wantZeroCopy = false;
	bool zeroCopy = false;
	ComPtr<ID3D11DeviceContext4> context4;
	ComPtr<ID3D12Device3> d12;
	ComPtr<ID3D12CommandQueue> queue;
	/* One per copy in flight; slotValue is the fence value that ends its
	 * copy. */
	static constexpr size_t allocatorCount = 3;
	std::array<ComPtr<ID3D12CommandAllocator>, allocatorCount> allocators;
	std::array<UINT64, allocatorCount> slotValue{};
	size_t slotNext = 0;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	ComPtr<ID3D11Fence> fence11;
	UINT64 fenceValue = 0;
	UINT64 copyValue = 0;          /* fenceValue once the newest copy is done */
	HANDLE fenceEvent = nullptr;   /* set when copyValue is reached */
	ComPtr<ID3D12Resource> frame12;
	/* The section as a heap, and a buffer over all of it to copy from. */
	ComPtr<ID3D12Heap> poolHeap;
	ComPtr<ID3D12Resource> poolBuffer;
	void* poolSection = nullptr;
	Uint64 poolSerial = 0;
	bool warnedLayout = false;

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

	bool initZeroCopy(IDXGIAdapter* adapter);
	void releaseZeroCopy();
	void waitCopies();
	void disableZeroCopy(HRESULT hr, const char* what);
	bool copyFromPool(void* section, Uint64 serial, Uint64 offset, int pitch,
	                  const std::vector<SDL_Rect>& rects);
};

bool SdlD3D11Presenter::Impl::initZeroCopy(IDXGIAdapter* adapter)
{
	ComPtr<ID3D11Device5> device5;
	if (failed(device->QueryInterface(IID_PPV_ARGS(device5.put())), "ID3D11Device5") ||
	    failed(context->QueryInterface(IID_PPV_ARGS(context4.put())), "ID3D11DeviceContext4") ||
	    failed(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(d12.put())),
	           "D3D12CreateDevice"))
		return false;

	D3D12_FEATURE_DATA_EXISTING_HEAPS heaps = {};
	if (FAILED(d12->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &heaps, sizeof(heaps))) ||
	    !heaps.Supported)
	{
		SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: no existing heaps, no zero copy");
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (failed(d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.put())),
	           "CreateCommandQueue") ||
	    failed(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.put())),
	           "CreateFence"))
		return false;

	HANDLE shared = nullptr;
	if (failed(d12->CreateSharedHandle(fence.get(), nullptr, GENERIC_ALL, nullptr, &shared),
	           "CreateSharedHandle (fence)"))
		return false;
	const auto hr = device5->OpenSharedFence(shared, IID_PPV_ARGS(fence11.put()));
	CloseHandle(shared);
	if (failed(hr, "OpenSharedFence"))
		return false;

	for (auto& allocator : allocators)
	{
		if (failed(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		                                       IID_PPV_ARGS(allocator.put())),
		           "CreateCommandAllocator"))
			return false;
	}
	if (failed(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].get(),
	                                  nullptr, IID_PPV_ARGS(list.put())),
	           "CreateCommandList") ||
	    failed(list->Close(), "Close"))
		return false;

	fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	fenceValue = 0;
	copyValue = 0;
	slotValue.fill(0);
	slotNext = 0;
	return fenceEvent != nullptr;
}

/* Resources may only go once the GPU is done with them. A removed device
 * completes every value, so this does not hang on one. */
void SdlD3D11Presenter::Impl::waitCopies()
{
	if (fence && (fence->GetCompletedValue() < fenceValue))
		(void)fence->SetEventOnCompletion(fenceValue, nullptr);
}

void SdlD3D11Presenter::Impl::releaseZeroCopy()
{
	waitCopies();
	zeroCopy = false;
	poolBuffer.reset();
	poolHeap.reset();
	poolSection = nullptr;
	poolSerial = 0;
	frame12.reset();
	if (fenceEvent)
		CloseHandle(fenceEvent);
	fenceEvent = nullptr;
	fence11.reset();
	fence.reset();
	list.reset();
	for (auto& allocator : allocators)
		allocator.reset();
	queue.reset();
	d12.reset();
	context4.reset();
}

/* Zero copy is off from here on; uploads go through the staging ring. */
void SdlD3D11Presenter::Impl::disableZeroCopy(HRESULT hr, const char* what)
{
	SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
	            "d3d11 presenter: %s failed (0x%08lx), zero copy off", what,
	            static_cast<unsigned long>(hr));
	if (d12 && FAILED(d12->GetDeviceRemovedReason()))
		checkLost();
	releaseZeroCopy();
}

bool SdlD3D11Presenter::Impl::copyFromPool(void* section, Uint64 serial, Uint64 offset, int pitch,
                                           const std::vector<SDL_Rect>& rects)
{
	if (!poolHeap || (poolSection != section) || (poolSerial != serial))
	{
		poolBuffer.reset();
		poolHeap.reset();
		poolSection = nullptr;
		auto hr = d12->OpenExistingHeapFromFileMapping(static_cast<HANDLE>(section),
		                                               IID_PPV_ARGS(poolHeap.put()));
		if (FAILED(hr))
		{
			disableZeroCopy(hr, "OpenExistingHeapFromFileMapping");
			return false;
		}

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = poolHeap->GetDesc().SizeInBytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		/* The heap is SHARED_CROSS_ADAPTER; E_INVALIDARG without this. */
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
		hr = d12->CreatePlacedResource(poolHeap.get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE,
		                               nullptr, IID_PPV_ARGS(poolBuffer.put()));
		if (FAILED(hr))
		{
			disableZeroCopy(hr, "CreatePlacedResource (pool)");
			return false;
		}
		poolSection = section;
		poolSerial = serial;
	}

	/* Only waits if the GPU is three copies behind. */
	const auto slot = slotNext;
	slotNext = (slotNext + 1) % allocatorCount;
	if (fence->GetCompletedValue() < slotValue[slot])
		(void)fence->SetEventOnCompletion(slotValue[slot], nullptr);
	auto hr = allocators[slot]->Reset();
	if (SUCCEEDED(hr))
		hr = list->Reset(allocators[slot].get(), nullptr);
	if (FAILED(hr))
	{
		disableZeroCopy(hr, "command list Reset");
		return false;
	}

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = poolBuffer.get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = offset;
	src.PlacedFootprint.Footprint = { frameFormat, static_cast<UINT>(frameW),
		                              static_cast<UINT>(frameH), 1, static_cast<UINT>(pitch) };
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = frame12.get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	for (const auto& r : rects)
	{
		const D3D12_BOX box{ static_cast<UINT>(r.x),       static_cast<UINT>(r.y),       0,
			                 static_cast<UINT>(r.x + r.w), static_cast<UINT>(r.y + r.h), 1 };
		list->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
	}

	/* Back to COMMON for Direct3D 11; the copy promoted it to COPY_DEST. */
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = frame12.get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	list->ResourceBarrier(1, &barrier);
	hr = list->Close();
	if (FAILED(hr))
	{
		disableZeroCopy(hr, "command list Close");
		return false;
	}

	/* The copy waits for Direct3D 11 to be done with frame, and Direct3D 11
	 * work from here on waits for the copy. */
	context4->Signal(fence11.get(), ++fenceValue);
	context->Flush();
	queue->Wait(fence.get(), fenceValue);
	ID3D12CommandList* lists[] = { list.get() };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence.get(), ++fenceValue);
	context4->Wait(fence11.get(), fenceValue);

	slotValue[slot] = fenceValue;
	copyValue = fenceValue;

	/* The caller must not ack the buffer to the server before this fires:
	 * the server may write it again from then on. Reset first, it may still
	 * be set from a copy nobody waited for. */
	(void)ResetEvent(fenceEvent);
	hr = fence->SetEventOnCompletion(copyValue, fenceEvent);
	if (FAILED(hr))
	{
		disableZeroCopy(hr, "SetEventOnCompletion");
		return false;
	}
	return true;
}

void SdlD3D11Presenter::Impl::release()
{
	if (context)
		context->ClearState();
	if (waitable)
		CloseHandle(waitable);
	waitable = nullptr;

	releaseZeroCopy();
	overlay.view.reset();
	notification.view.reset();
	damageView.reset();
	frameView.reset();
	frame.reset();
	for (auto& texture : staging)
		texture.reset();
	frameW = 0;
	frameH = 0;
	sampler.reset();
	overlayFade.reset();
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

	if (wantZeroCopy)
	{
		zeroCopy = initZeroCopy(adapter.get());
		if (!zeroCopy)
			releaseZeroCopy();
		SDL_LogInfo(SDL_LOG_CATEGORY_RENDER, "d3d11 presenter: zero copy %s",
		            zeroCopy ? "on" : "unavailable");
	}

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

	D3D11_BUFFER_DESC fadeDesc = {};
	fadeDesc.ByteWidth = 16;
	fadeDesc.Usage = D3D11_USAGE_DEFAULT;
	fadeDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (failed(device->CreateBuffer(&fadeDesc, nullptr, overlayFade.put()), "CreateBuffer (fade)"))
		return false;

	{
		const UINT32 pink = 0xffff0080; /* BGRA in memory: 80 00 ff ff */
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = 1;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data = {};
		data.pSysMem = &pink;
		data.SysMemPitch = sizeof(pink);
		ComPtr<ID3D11Texture2D> texture;
		if (failed(device->CreateTexture2D(&desc, &data, texture.put()),
		           "CreateTexture2D (damage)") ||
		    failed(device->CreateShaderResourceView(texture.get(), nullptr, damageView.put()),
		           "CreateShaderResourceView (damage)"))
			return false;
	}

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

	waitCopies();
	frame12.reset();
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
	if (zeroCopy)
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	if (failed(device->CreateTexture2D(&desc, nullptr, frame.put()), "CreateTexture2D (frame)"))
		return false;
	if (zeroCopy)
	{
		ComPtr<IDXGIResource1> resource;
		HANDLE shared = nullptr;
		auto hr = frame->QueryInterface(IID_PPV_ARGS(resource.put()));
		if (SUCCEEDED(hr))
			hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared);
		if (SUCCEEDED(hr))
		{
			hr = d12->OpenSharedHandle(shared, IID_PPV_ARGS(frame12.put()));
			CloseHandle(shared);
		}
		if (FAILED(hr))
			disableZeroCopy(hr, "sharing the frame texture");
	}
	desc.MiscFlags = 0;
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

std::unique_ptr<SdlD3D11Presenter> SdlD3D11Presenter::create(void* hwnd, bool vsync,
                                                             bool zeroCopy)
{
	if (!hwnd)
		return nullptr;

	auto impl = std::make_unique<Impl>();
	impl->hwnd = static_cast<HWND>(hwnd);
	impl->vsync = vsync;
	impl->wantZeroCopy = zeroCopy;
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
                               UploadInfo* info)
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

	const size_t bpp = 4;
	Uint64 copied = 0;
	for (const auto& r : clipped)
		copied += static_cast<Uint64>(r.w) * static_cast<Uint64>(r.h) * bpp;
	if (info)
		*info = { copied, false };

	if (_impl->zeroCopy)
	{
		const auto props = SDL_GetSurfaceProperties(surface);
		auto section = SDL_GetPointerProperty(props, poolSectionProperty, nullptr);
		const auto offset = static_cast<Uint64>(SDL_GetNumberProperty(props, poolOffsetProperty, 0));
		const auto serial = static_cast<Uint64>(SDL_GetNumberProperty(props, poolSerialProperty, 0));
		const bool aligned = (surface->pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0) &&
		                     (offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);
		if (section && !aligned && !_impl->warnedLayout)
		{
			SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
			            "d3d11 presenter: pitch %d or offset %" SDL_PRIu64
			            " not aligned for Direct3D 12, copying with the CPU",
			            surface->pitch, offset);
			_impl->warnedLayout = true;
		}

		if (section && aligned &&
		    _impl->copyFromPool(section, serial, offset, surface->pitch, clipped))
		{
			if (info)
				*info = { copied, true };
			return true;
		}
		/* The device may be gone; otherwise copy this frame the old way. */
		if (_impl->lost)
			return false;
	}

	auto context = _impl->context.get();
	auto staging = _impl->staging[_impl->stagingNext].get();
	_impl->stagingNext = (_impl->stagingNext + 1) % Impl::stagingCount;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (failed(context->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped), "Map"))
	{
		_impl->checkLost();
		return false;
	}

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
	return true;
}

void* SdlD3D11Presenter::waitHandle() const
{
	return _impl->waitable;
}

bool SdlD3D11Presenter::copyPending() const
{
	return _impl->zeroCopy && (_impl->fence->GetCompletedValue() < _impl->copyValue);
}

void* SdlD3D11Presenter::copyHandle() const
{
	return _impl->zeroCopy ? _impl->fenceEvent : nullptr;
}

bool SdlD3D11Presenter::setOverlay(SDL_Surface* surface)
{
	return _impl->setOverlay(_impl->overlay, surface);
}

bool SdlD3D11Presenter::setNotification(SDL_Surface* surface, bool bottom)
{
	/* Hiding fades the one shown out; draw() drops it after. */
	if (!surface)
	{
		if (_impl->notification.view && !_impl->notificationFadingOut)
		{
			_impl->notificationFadingOut = true;
			_impl->notificationSince = SDL_GetTicksNS();
		}
		return true;
	}
	_impl->notificationBottom = bottom;
	_impl->notificationFadingOut = false;
	_impl->notificationSince = SDL_GetTicksNS();
	return _impl->setOverlay(_impl->notification, surface);
}

bool SdlD3D11Presenter::animating() const
{
	return _impl->animating;
}

bool SdlD3D11Presenter::Impl::setOverlay(Overlay& target, SDL_Surface* surface)
{
	target.view.reset();
	if (!surface || lost)
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
	if (failed(device->CreateTexture2D(&desc, &data, texture.put()), "CreateTexture2D (overlay)"))
		return false;
	if (failed(device->CreateShaderResourceView(texture.get(), nullptr, target.view.put()),
	           "CreateShaderResourceView (overlay)"))
		return false;
	target.w = surface->w;
	target.h = surface->h;
	return true;
}

void SdlD3D11Presenter::setDamage(const std::vector<SDL_Rect>& rects)
{
	_impl->damage = rects;
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

	auto fade = _impl->overlayFade.get();
	const auto setFade = [&](float alpha)
	{
		const std::array<float, 4> data = { alpha, 0.0f, 0.0f, 0.0f };
		context->UpdateSubresource(fade, 0, nullptr, data.data(), 0, 0);
		context->PSSetConstantBuffers(0, 1, &fade);
	};
	const auto drawOverlay = [&](const Impl::Overlay& overlay, int x, int y, float alpha)
	{
		if (!overlay.view)
			return;
		setFade(alpha);
		viewport.TopLeftX = static_cast<float>(x);
		viewport.TopLeftY = static_cast<float>(y);
		viewport.Width = static_cast<float>(overlay.w);
		viewport.Height = static_cast<float>(overlay.h);
		context->RSSetViewports(1, &viewport);

		view = overlay.view.get();
		context->PSSetShader(_impl->overlayPs.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &view);
		context->OMSetBlendState(_impl->overlayBlend.get(), nullptr, 0xffffffff);
		context->Draw(3, 0);
	};
	if (!_impl->damage.empty() && _impl->damageView)
	{
		setFade(1.0f);
		view = _impl->damageView.get();
		context->PSSetShader(_impl->overlayPs.get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &view);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		const auto edge = [&](int x, int y, int w, int h)
		{
			viewport.TopLeftX = static_cast<float>(x);
			viewport.TopLeftY = static_cast<float>(y);
			viewport.Width = static_cast<float>(std::max(w, 1));
			viewport.Height = static_cast<float>(std::max(h, 1));
			context->RSSetViewports(1, &viewport);
			context->Draw(3, 0);
		};
		/* Two pixels wide, inside the rect. */
		for (const auto& r : _impl->damage)
		{
			const int t = std::min({ 2, r.w, r.h });
			if (t <= 0)
				continue;
			edge(r.x, r.y, r.w, t);
			edge(r.x, r.y + r.h - t, r.w, t);
			edge(r.x, r.y, t, r.h);
			edge(r.x + r.w - t, r.y, t, r.h);
		}
	}
	drawOverlay(_impl->overlay, 0, 0, 1.0f);

	/* Fades in over 200 ms and out over 150 ms, quick at first. */
	const auto fadeMs = _impl->notificationFadingOut ? 150.0f : 200.0f;
	const auto elapsedMs =
	    static_cast<float>(SDL_GetTicksNS() - _impl->notificationSince) / 1000000.0f;
	const auto progress = std::clamp(elapsedMs / fadeMs, 0.0f, 1.0f);
	const auto eased = 1.0f - ((1.0f - progress) * (1.0f - progress) * (1.0f - progress));
	const auto shown = _impl->notificationFadingOut ? 1.0f - eased : eased;
	_impl->animating = _impl->notification.view && (progress < 1.0f);
	if (_impl->notificationFadingOut && (progress >= 1.0f))
		_impl->notification.view.reset();
	drawOverlay(_impl->notification,
	            std::max(0, (static_cast<int>(_impl->backW) - _impl->notification.w) / 2),
	            _impl->notificationBottom
	                ? std::max(0, static_cast<int>(_impl->backH) - _impl->notification.h)
	                : 0,
	            shown);
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
