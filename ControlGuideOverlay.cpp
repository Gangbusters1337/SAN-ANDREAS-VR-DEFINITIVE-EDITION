#include "ControlGuideOverlay.h"

#include "uevr/API.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include <d3dcompiler.h>
#include <wincodec.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

namespace {
	constexpr float GuideAspect = 16.0f / 9.0f;

	std::wstring GetGuideImagePath()
	{
		wchar_t appData[MAX_PATH]{};
		const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
		if (length == 0 || length >= MAX_PATH)
			return {};
		return std::wstring(appData) + L"\\UnrealVRMod\\SanAndreas\\SAVR_ControlGuide.png";
	}

	D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
	{
		D3D12_HEAP_PROPERTIES properties{};
		properties.Type = type;
		properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		properties.CreationNodeMask = 1;
		properties.VisibleNodeMask = 1;
		return properties;
	}
}

void ControlGuideOverlay::SetVisible(bool value)
{
	const bool wasVisible = visible.exchange(value, std::memory_order_acq_rel);
	if (value && !wasVisible)
		dx11WarmupFrames.store(2, std::memory_order_release);
	else if (!value)
		dx11WarmupFrames.store(0, std::memory_order_release);
}

void ControlGuideOverlay::SetOptionsState(int orientation, bool autoHide, bool useR3Dpad, uint32_t diagnostics,
	uint32_t selected)
{
	std::scoped_lock lock(stateMutex);
	selected = (std::min)(selected, 3U);
	if (movementOrientation == orientation && hudAutoHide == autoHide
		&& r3DpadMode == useR3Dpad && diagnosticMode == diagnostics
		&& selectedOption == selected && !basePixels.empty())
		return;
	movementOrientation = orientation;
	hudAutoHide = autoHide;
	r3DpadMode = useR3Dpad;
	diagnosticMode = diagnostics;
	selectedOption = selected;
	if (!basePixels.empty() && ComposeOptionsPanel())
	{
		ReleaseDx12();
		ReleaseDx11();
	}
}

bool ControlGuideOverlay::IsVisible() const
{
	return visible.load(std::memory_order_acquire);
}

void ControlGuideOverlay::ReleaseDx12()
{
	textureUploaded = false;
	activeDevice = nullptr;
	activeTargetFormat = DXGI_FORMAT_UNKNOWN;
	textureUpload.Reset();
	texture.Reset();
	srvHeap.Reset();
	pipelineState.Reset();
	rootSignature.Reset();
}

void ControlGuideOverlay::OnDeviceReset()
{
	std::scoped_lock lock(stateMutex);
	ReleaseDx12();
	ReleaseDx11();
}

void ControlGuideOverlay::ReleaseDx11()
{
	activeDevice11 = nullptr;
	sampler11.Reset();
	textureView11.Reset();
	texture11.Reset();
	pixelShader11.Reset();
	vertexShader11.Reset();
}

bool ControlGuideOverlay::InitializeDx11(ID3D11Device* device)
{
	if (device == nullptr || !LoadImage()) return false;
	if (activeDevice11 == device && vertexShader11 && pixelShader11 && textureView11 && sampler11)
		return true;
	ReleaseDx11();
	activeDevice11 = device;
	static constexpr char vs[] = R"(
struct O { float4 p:SV_Position; float2 uv:TEXCOORD0; };
O main(uint id:SV_VertexID) { float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)}; float2 u[3]={float2(0,1),float2(0,-1),float2(2,1)}; O o; o.p=float4(p[id],0,1); o.uv=u[id]; return o; })";
	static constexpr char ps[] = R"(
Texture2D t:register(t0); SamplerState s:register(s0);
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target { return t.Sample(s,uv); })";
	ComPtr<ID3DBlob> vb, pb, errors;
	if (FAILED(D3DCompile(vs, sizeof(vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &errors)) ||
		FAILED(D3DCompile(ps, sizeof(ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &errors)) ||
		FAILED(device->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vertexShader11)) ||
		FAILED(device->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &pixelShader11))) return false;
	D3D11_TEXTURE2D_DESC td{}; td.Width=imageWidth; td.Height=imageHeight; td.MipLevels=1; td.ArraySize=1;
	td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_IMMUTABLE; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=pixels.data(); data.SysMemPitch=imageWidth*4;
	if (FAILED(device->CreateTexture2D(&td, &data, &texture11)) ||
		FAILED(device->CreateShaderResourceView(texture11.Get(), nullptr, &textureView11))) return false;
	D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
	return SUCCEEDED(device->CreateSamplerState(&sd, &sampler11));
}

void ControlGuideOverlay::RenderDx11(ID3D11DeviceContext* context, ID3D11Texture2D* renderTarget,
	ID3D11RenderTargetView* renderTargetView)
{
	(void)renderTargetView;
	if (!IsVisible() || !context) return;
	uint32_t warmup = dx11WarmupFrames.load(std::memory_order_acquire);
	while (warmup > 0)
	{
		if (dx11WarmupFrames.compare_exchange_weak(
			warmup, warmup - 1, std::memory_order_acq_rel, std::memory_order_acquire))
			return;
	}
	std::scoped_lock lock(stateMutex);
	ComPtr<ID3D11Device> device; context->GetDevice(&device);
	if (!InitializeDx11(device.Get())) {
		static bool logged=false; if (!logged) { logged=true; uevr::API::get()->log_error("[ControlGuide] D3D11 initialization failed"); }
		return;
	}
	// The post-framework callback texture is the desktop/framework surface, not
	// necessarily the texture UEVR submits as the in-headset UI layer. Draw onto
	// UEVR's own UI render target so the guide follows the normal VR compositor.
	auto uiTextureHandle = uevr::API::StereoHook::get_ui_render_target();
	auto uiTarget = uiTextureHandle != nullptr
		? static_cast<ID3D11Texture2D*>(uiTextureHandle->get_native_resource()) : nullptr;
	if (uiTarget == nullptr)
	{
		static bool loggedMissing = false;
		if (!loggedMissing)
		{
			loggedMissing = true;
			uevr::API::get()->log_error("[ControlGuide] UEVR UI render target unavailable");
		}
		return;
	}
	D3D11_TEXTURE2D_DESC td{};
	uiTarget->GetDesc(&td);
	const DXGI_FORMAT uiViewFormat = td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS
		? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = uiViewFormat;
	if (td.SampleDesc.Count > 1)
	{
		if (td.ArraySize > 1)
		{
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
			rtvDesc.Texture2DMSArray.FirstArraySlice = 0;
			rtvDesc.Texture2DMSArray.ArraySize = td.ArraySize;
		}
		else
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
	}
	else if (td.ArraySize > 1)
	{
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
		rtvDesc.Texture2DArray.MipSlice = 0;
		rtvDesc.Texture2DArray.FirstArraySlice = 0;
		rtvDesc.Texture2DArray.ArraySize = td.ArraySize;
	}
	else
	{
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
	}
	ComPtr<ID3D11RenderTargetView> guideRenderTargetView;
	const HRESULT rtvResult = device->CreateRenderTargetView(
		uiTarget, &rtvDesc, &guideRenderTargetView);
	if (FAILED(rtvResult))
	{
		static bool loggedFailure = false;
		if (!loggedFailure)
		{
			loggedFailure = true;
			uevr::API::get()->log_error(
				"[ControlGuide] typed UI RTV failed hr=0x%08X width=%u height=%u resourceFormat=%u viewFormat=%u bind=0x%X samples=%u",
				static_cast<unsigned int>(rtvResult), td.Width, td.Height,
				static_cast<unsigned int>(td.Format), static_cast<unsigned int>(rtvDesc.Format),
				td.BindFlags, td.SampleDesc.Count);
		}
		return;
	}

	float w=(float)td.Width, h=(float)td.Height, dw=w, dh=h;
	if (w/h > GuideAspect) dw=h*GuideAspect; else dh=w/GuideAspect;
	D3D11_VIEWPORT vp{(w-dw)*0.5f,(h-dh)*0.5f,dw,dh,0,1};
	D3D11_RECT sc{(LONG)vp.TopLeftX,(LONG)vp.TopLeftY,(LONG)std::ceil(vp.TopLeftX+dw),(LONG)std::ceil(vp.TopLeftY+dh)};
	ComPtr<ID3D11RenderTargetView> previousRtv;
	ComPtr<ID3D11DepthStencilView> previousDsv;
	context->OMGetRenderTargets(1, &previousRtv, &previousDsv);
	UINT previousViewportCount = 1;
	D3D11_VIEWPORT previousViewport{};
	context->RSGetViewports(&previousViewportCount, &previousViewport);
	UINT previousScissorCount = 1;
	D3D11_RECT previousScissor{};
	context->RSGetScissorRects(&previousScissorCount, &previousScissor);
	ComPtr<ID3D11InputLayout> previousInputLayout;
	context->IAGetInputLayout(&previousInputLayout);
	D3D11_PRIMITIVE_TOPOLOGY previousTopology{};
	context->IAGetPrimitiveTopology(&previousTopology);
	ComPtr<ID3D11VertexShader> previousVs;
	context->VSGetShader(&previousVs, nullptr, nullptr);
	ComPtr<ID3D11PixelShader> previousPs;
	context->PSGetShader(&previousPs, nullptr, nullptr);
	ComPtr<ID3D11ShaderResourceView> previousSrv;
	context->PSGetShaderResources(0, 1, &previousSrv);
	ComPtr<ID3D11SamplerState> previousSampler;
	context->PSGetSamplers(0, 1, &previousSampler);

	ID3D11RenderTargetView* guideRtv = guideRenderTargetView.Get();
	const float black[4]{0,0,0,1}; context->ClearRenderTargetView(guideRtv, black);
	context->OMSetRenderTargets(1, &guideRtv, nullptr); context->RSSetViewports(1,&vp); context->RSSetScissorRects(1,&sc);
	context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vertexShader11.Get(),nullptr,0); context->PSSetShader(pixelShader11.Get(),nullptr,0);
	ID3D11ShaderResourceView* srv=textureView11.Get(); ID3D11SamplerState* samp=sampler11.Get();
	context->PSSetShaderResources(0,1,&srv); context->PSSetSamplers(0,1,&samp); context->Draw(3,0);

	ID3D11ShaderResourceView* previousSrvRaw = previousSrv.Get();
	ID3D11SamplerState* previousSamplerRaw = previousSampler.Get();
	ID3D11RenderTargetView* previousRtvRaw = previousRtv.Get();
	context->PSSetShaderResources(0, 1, &previousSrvRaw);
	context->PSSetSamplers(0, 1, &previousSamplerRaw);
	context->VSSetShader(previousVs.Get(), nullptr, 0);
	context->PSSetShader(previousPs.Get(), nullptr, 0);
	context->IASetInputLayout(previousInputLayout.Get());
	context->IASetPrimitiveTopology(previousTopology);
	if (previousViewportCount > 0) context->RSSetViewports(1, &previousViewport);
	if (previousScissorCount > 0) context->RSSetScissorRects(1, &previousScissor);
	context->OMSetRenderTargets(1, &previousRtvRaw, previousDsv.Get());
	static bool logged=false; if (!logged) {
		logged=true;
		uevr::API::get()->log_info(
			"[ControlGuide] D3D11 typed UEVR UI overlay rendered path=framework-present callbackTarget=%p uiTarget=%p width=%u height=%u array=%u resourceFormat=%u viewFormat=%u samples=%u",
			renderTarget, uiTarget, td.Width, td.Height, td.ArraySize,
			static_cast<unsigned int>(td.Format), static_cast<unsigned int>(uiViewFormat),
			td.SampleDesc.Count);
	}
}

bool ControlGuideOverlay::LoadImage()
{
	if (!pixels.empty())
		return true;
	if (imageLoadAttempted)
		return false;
	imageLoadAttempted = true;

	const std::wstring path = GetGuideImagePath();
	if (path.empty())
		return false;

	const HRESULT initResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool uninitialize = SUCCEEDED(initResult);
	ComPtr<IWICImagingFactory> factory;
	ComPtr<IWICBitmapDecoder> decoder;
	ComPtr<IWICBitmapFrameDecode> frame;
	ComPtr<IWICFormatConverter> converter;

	HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (SUCCEEDED(result))
		result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnLoad, &decoder);
	if (SUCCEEDED(result))
		result = decoder->GetFrame(0, &frame);
	if (SUCCEEDED(result))
		result = factory->CreateFormatConverter(&converter);
	if (SUCCEEDED(result))
		result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
			WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

	UINT width = 0;
	UINT height = 0;
	if (SUCCEEDED(result))
		result = converter->GetSize(&width, &height);
	if (SUCCEEDED(result) && width > 0 && height > 0
		&& width <= 4096 && height <= 4096)
	{
		const size_t byteCount = static_cast<size_t>(width) * height * 4;
		pixels.resize(byteCount);
		result = converter->CopyPixels(nullptr, width * 4,
			static_cast<UINT>(byteCount), pixels.data());
		if (SUCCEEDED(result))
		{
			imageWidth = width;
			imageHeight = height;
		}
	}

	if (uninitialize)
		CoUninitialize();
	if (FAILED(result) || pixels.empty())
	{
		pixels.clear();
		uevr::API::get()->log_error("[ControlGuide] image load failed hr=0x%08X path=%ls",
			static_cast<unsigned int>(result), path.c_str());
		return false;
	}
	basePixels = pixels;
	if (!ComposeOptionsPanel())
		pixels = basePixels;

	uevr::API::get()->log_info("[ControlGuide] image loaded size=%ux%u path=%ls",
		imageWidth, imageHeight, path.c_str());
	return true;
}

bool ControlGuideOverlay::ComposeOptionsPanel()
{
	if (basePixels.empty() || imageWidth == 0 || imageHeight == 0
		|| basePixels.size() != static_cast<size_t>(imageWidth) * imageHeight * 4)
		return false;

	BITMAPINFO bitmapInfo{};
	bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(imageWidth);
	bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(imageHeight);
	bitmapInfo.bmiHeader.biPlanes = 1;
	bitmapInfo.bmiHeader.biBitCount = 32;
	bitmapInfo.bmiHeader.biCompression = BI_RGB;
	void* dibBits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bitmap = dc != nullptr
		? CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS, &dibBits, nullptr, 0) : nullptr;
	if (dc == nullptr || bitmap == nullptr || dibBits == nullptr)
	{
		if (bitmap != nullptr) DeleteObject(bitmap);
		if (dc != nullptr) DeleteDC(dc);
		return false;
	}

	auto* bgra = static_cast<uint8_t*>(dibBits);
	for (size_t pixel = 0; pixel < static_cast<size_t>(imageWidth) * imageHeight; ++pixel)
	{
		bgra[pixel * 4 + 0] = basePixels[pixel * 4 + 2];
		bgra[pixel * 4 + 1] = basePixels[pixel * 4 + 1];
		bgra[pixel * 4 + 2] = basePixels[pixel * 4 + 0];
		bgra[pixel * 4 + 3] = 255;
	}

	const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
	SetBkMode(dc, TRANSPARENT);
	// Keep the interactive settings in the unused strip below the title. This
	// avoids covering any of the permanent controller, camera, or VR-tip text.
	const RECT panel{ 610, 176, 1310, 252 };
	HBRUSH panelBrush = CreateSolidBrush(RGB(3, 10, 4));
	FillRect(dc, &panel, panelBrush);
	DeleteObject(panelBrush);
	HPEN borderPen = CreatePen(PS_SOLID, 3, RGB(115, 165, 20));
	const HGDIOBJ oldPen = SelectObject(dc, borderPen);
	const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
	Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);

	HFONT titleFont = CreateFontW(19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_SWISS, L"Arial");
	HFONT optionFont = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_SWISS, L"Arial");
	HFONT footerFont = CreateFontW(19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_SWISS, L"Arial");
	if (titleFont == nullptr || optionFont == nullptr || footerFont == nullptr)
	{
		if (titleFont != nullptr) DeleteObject(titleFont);
		if (optionFont != nullptr) DeleteObject(optionFont);
		if (footerFont != nullptr) DeleteObject(footerFont);
		SelectObject(dc, oldBrush);
		SelectObject(dc, oldPen);
		DeleteObject(borderPen);
		SelectObject(dc, oldBitmap);
		DeleteObject(bitmap);
		DeleteDC(dc);
		return false;
	}

	const HGDIOBJ oldFont = SelectObject(dc, titleFont);
	SetTextColor(dc, RGB(226, 166, 61));
	RECT titleRect{ panel.left + 12, panel.top + 4, panel.right - 12, panel.top + 28 };
	DrawTextW(dc, L"VR OPTIONS   -   LEFT STICK LEFT / RIGHT: SELECT   -   A: CHANGE", -1, &titleRect,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER);

	SelectObject(dc, optionFont);
	for (uint32_t index = 0; index < 4; ++index)
	{
		const LONG columnWidth = (panel.right - panel.left - 50) / 4;
		RECT row{ panel.left + 10 + static_cast<LONG>(index) * (columnWidth + 10), panel.top + 31,
			panel.left + 10 + static_cast<LONG>(index) * (columnWidth + 10) + columnWidth, panel.bottom - 8 };
		if (index == selectedOption)
		{
			HBRUSH selectedBrush = CreateSolidBrush(RGB(35, 58, 12));
			FillRect(dc, &row, selectedBrush);
			DeleteObject(selectedBrush);
		}
		SetTextColor(dc, index == selectedOption ? RGB(245, 245, 220) : RGB(205, 215, 198));
		if (index == 0)
		{
			const wchar_t* state = movementOrientation == 1 ? L"HEAD / HMD"
				: movementOrientation == 0 ? L"STANDARD" : L"CUSTOM";
			std::wstring label = L"BODY ORIENTATION: ";
			label += state;
			RECT labelRect{ row.left + 8, row.top, row.right - 8, row.bottom };
			DrawTextW(dc, label.c_str(), -1, &labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		}
		else if (index == 1)
		{
			std::wstring label = L"HUD AUTO-HIDE: ";
			label += hudAutoHide ? L"ON" : L"OFF";
			SetTextColor(dc, hudAutoHide ? RGB(139, 208, 36) : RGB(210, 85, 65));
			RECT labelRect{ row.left + 8, row.top, row.right - 8, row.bottom };
			DrawTextW(dc, label.c_str(), -1, &labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		}
		else if (index == 2)
		{
			std::wstring label = L"D-PAD:\n";
			label += r3DpadMode ? L"R3 + R-STICK" : L"THUMBREST + R-STICK";
			SetTextColor(dc, RGB(139, 208, 36));
			RECT labelRect{ row.left + 3, row.top + 2, row.right - 3, row.bottom };
			DrawTextW(dc, label.c_str(), -1, &labelRect, DT_CENTER | DT_WORDBREAK);
		}
		else
		{
			const wchar_t* state = diagnosticMode == 1 ? L"VEHICLE"
				: diagnosticMode == 2 ? L"SAVE / LOAD"
				: diagnosticMode == 3 ? L"FULL" : L"OFF";
			std::wstring label = L"DIAGNOSTICS: ";
			label += state;
			SetTextColor(dc, diagnosticMode == 0 ? RGB(205, 215, 198) : RGB(226, 166, 61));
			RECT labelRect{ row.left + 5, row.top, row.right - 5, row.bottom };
			DrawTextW(dc, label.c_str(), -1, &labelRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
		}
	}

	SelectObject(dc, footerFont);
	SetTextColor(dc, RGB(235, 35, 54));
	RECT footerRect{ 730, 371, 1190, 402 };
	DrawTextW(dc, L"AND VR OPTIONS", -1, &footerRect,
		DT_CENTER | DT_SINGLELINE | DT_VCENTER);

	pixels.resize(basePixels.size());
	for (size_t pixel = 0; pixel < static_cast<size_t>(imageWidth) * imageHeight; ++pixel)
	{
		pixels[pixel * 4 + 0] = bgra[pixel * 4 + 2];
		pixels[pixel * 4 + 1] = bgra[pixel * 4 + 1];
		pixels[pixel * 4 + 2] = bgra[pixel * 4 + 0];
		pixels[pixel * 4 + 3] = 255;
	}

	SelectObject(dc, oldFont);
	DeleteObject(titleFont);
	DeleteObject(optionFont);
	DeleteObject(footerFont);
	SelectObject(dc, oldBrush);
	SelectObject(dc, oldPen);
	DeleteObject(borderPen);
	SelectObject(dc, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return true;
}

bool ControlGuideOverlay::InitializeDx12(ID3D12Device* device, DXGI_FORMAT targetFormat)
{
	if (device == nullptr || !LoadImage())
		return false;
	if (activeDevice == device && activeTargetFormat == targetFormat
		&& rootSignature && pipelineState && srvHeap && texture && textureUpload)
		return true;

	ReleaseDx12();
	activeDevice = device;
	activeTargetFormat = targetFormat;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 1;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvHeap))))
		return false;

	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = 1;
	range.BaseShaderRegister = 0;
	range.RegisterSpace = 0;
	range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
	D3D12_ROOT_PARAMETER parameter{};
	parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameter.DescriptorTable.NumDescriptorRanges = 1;
	parameter.DescriptorTable.pDescriptorRanges = &range;
	parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	D3D12_ROOT_SIGNATURE_DESC rootDesc{};
	rootDesc.NumParameters = 1;
	rootDesc.pParameters = &parameter;
	rootDesc.NumStaticSamplers = 1;
	rootDesc.pStaticSamplers = &sampler;
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	ComPtr<ID3DBlob> serializedRoot;
	ComPtr<ID3DBlob> errorBlob;
	if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		&serializedRoot, &errorBlob))
		|| FAILED(device->CreateRootSignature(0, serializedRoot->GetBufferPointer(),
			serializedRoot->GetBufferSize(), IID_PPV_ARGS(&rootSignature))))
		return false;

	static constexpr char VertexShader[] = R"(
struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
    output.uv = uvs[id];
    return output;
})";
	static constexpr char PixelShader[] = R"(
Texture2D guideTexture : register(t0);
SamplerState guideSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return guideTexture.Sample(guideSampler, uv);
})";
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;
	if (FAILED(D3DCompile(VertexShader, sizeof(VertexShader), nullptr, nullptr, nullptr,
		"main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexShader, &errorBlob))
		|| FAILED(D3DCompile(PixelShader, sizeof(PixelShader), nullptr, nullptr, nullptr,
			"main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelShader, &errorBlob)))
		return false;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
	pipelineDesc.pRootSignature = rootSignature.Get();
	pipelineDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
	pipelineDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
	pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pipelineDesc.SampleMask = UINT_MAX;
	pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
	pipelineDesc.DepthStencilState.DepthEnable = FALSE;
	pipelineDesc.DepthStencilState.StencilEnable = FALSE;
	pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pipelineDesc.NumRenderTargets = 1;
	pipelineDesc.RTVFormats[0] = targetFormat;
	pipelineDesc.SampleDesc.Count = 1;
	if (FAILED(device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipelineState))))
		return false;

	D3D12_RESOURCE_DESC textureDesc{};
	textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	textureDesc.Alignment = 0;
	textureDesc.Width = imageWidth;
	textureDesc.Height = imageHeight;
	textureDesc.DepthOrArraySize = 1;
	textureDesc.MipLevels = 1;
	textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
	if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
		D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))))
		return false;

	const UINT64 rowPitch = (static_cast<UINT64>(imageWidth) * 4 + 255) & ~255ULL;
	D3D12_RESOURCE_DESC uploadDesc{};
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = rowPitch * imageHeight;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
	if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&textureUpload))))
		return false;

	uint8_t* mapped = nullptr;
	D3D12_RANGE noRead{ 0, 0 };
	if (FAILED(textureUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped))))
		return false;
	for (uint32_t row = 0; row < imageHeight; ++row)
		std::memcpy(mapped + rowPitch * row,
			pixels.data() + static_cast<size_t>(imageWidth) * 4 * row,
			static_cast<size_t>(imageWidth) * 4);
	textureUpload->Unmap(0, nullptr);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(texture.Get(), &srvDesc,
		srvHeap->GetCPUDescriptorHandleForHeapStart());
	return true;
}

void ControlGuideOverlay::RenderDx12(ID3D12GraphicsCommandList* commandList,
	ID3D12Resource* renderTarget, D3D12_CPU_DESCRIPTOR_HANDLE* renderTargetView)
{
	if (!IsVisible() || commandList == nullptr || renderTarget == nullptr || renderTargetView == nullptr)
		return;

	std::scoped_lock lock(stateMutex);
	ComPtr<ID3D12Device> device;
	if (FAILED(renderTarget->GetDevice(IID_PPV_ARGS(&device))))
		return;
	const D3D12_RESOURCE_DESC targetDesc = renderTarget->GetDesc();
	if (!InitializeDx12(device.Get(), targetDesc.Format))
	{
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			uevr::API::get()->log_error("[ControlGuide] D3D12 initialization failed format=%d",
				static_cast<int>(targetDesc.Format));
		}
		return;
	}

	if (!textureUploaded)
	{
		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = texture.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = textureUpload.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		source.PlacedFootprint.Footprint.Width = imageWidth;
		source.PlacedFootprint.Footprint.Height = imageHeight;
		source.PlacedFootprint.Footprint.Depth = 1;
		source.PlacedFootprint.Footprint.RowPitch = (imageWidth * 4 + 255) & ~255U;
		commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		commandList->ResourceBarrier(1, &barrier);
		textureUploaded = true;
		uevr::API::get()->log_info("[ControlGuide] D3D12 texture uploaded");
	}

	const float targetWidth = static_cast<float>(targetDesc.Width);
	const float targetHeight = static_cast<float>(targetDesc.Height);
	const float targetAspect = targetWidth / (targetHeight > 1.0f ? targetHeight : 1.0f);
	float drawWidth = targetWidth;
	float drawHeight = targetHeight;
	if (targetAspect > GuideAspect)
		drawWidth = targetHeight * GuideAspect;
	else
		drawHeight = targetWidth / GuideAspect;
	D3D12_VIEWPORT viewport{};
	viewport.TopLeftX = (targetWidth - drawWidth) * 0.5f;
	viewport.TopLeftY = (targetHeight - drawHeight) * 0.5f;
	viewport.Width = drawWidth;
	viewport.Height = drawHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	D3D12_RECT scissor{
		static_cast<LONG>(viewport.TopLeftX), static_cast<LONG>(viewport.TopLeftY),
		static_cast<LONG>(std::ceil(viewport.TopLeftX + drawWidth)),
		static_cast<LONG>(std::ceil(viewport.TopLeftY + drawHeight)) };

	const float black[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
	commandList->ClearRenderTargetView(*renderTargetView, black, 0, nullptr);
	commandList->OMSetRenderTargets(1, renderTargetView, FALSE, nullptr);
	ID3D12DescriptorHeap* heaps[]{ srvHeap.Get() };
	commandList->SetDescriptorHeaps(1, heaps);
	commandList->SetGraphicsRootSignature(rootSignature.Get());
	commandList->SetPipelineState(pipelineState.Get());
	commandList->RSSetViewports(1, &viewport);
	commandList->RSSetScissorRects(1, &scissor);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
	commandList->DrawInstanced(3, 1, 0, 0);
}
