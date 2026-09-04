#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <d3d12.h>
#include <d3d11.h>
#include <wrl/client.h>

class ControlGuideOverlay {
public:
	enum Option : uint32_t { MovementDirection, HudAutoHide, DpadControl, ControlLayout, Diagnostics, Reset3dVr, OptionCount };
	enum ResetState : uint32_t { ResetReady, ResetQueued, ResetDone, ResetResumeGame, ResetFailed };
	void SetVisible(bool visible);
	void SetOptionsState(int movementOrientation, bool hudAutoHide, bool r3DpadMode, uint32_t diagnosticMode,
		bool leftHandedLayout, uint32_t selectedOption, ResetState resetState);
	bool IsVisible() const;
	void OnDeviceReset();
	void RenderDx12(ID3D12GraphicsCommandList* commandList, ID3D12Resource* renderTarget,
		D3D12_CPU_DESCRIPTOR_HANDLE* renderTargetView);
	void RenderDx11(ID3D11DeviceContext* context, ID3D11Texture2D* renderTarget,
		ID3D11RenderTargetView* renderTargetView);

private:
	friend struct ControlGuidePreview; // Offline renderer uses the actual compositor for layout tests.
	bool LoadImage();
	bool ComposeOptionsPanel();
	bool InitializeDx12(ID3D12Device* device, DXGI_FORMAT targetFormat);
	void ReleaseDx12();
	bool InitializeDx11(ID3D11Device* device);
	void ReleaseDx11();

	std::atomic<bool> visible{ false };
	std::atomic<uint32_t> dx11WarmupFrames{ 0 };
	std::mutex stateMutex;
	std::vector<uint8_t> pixels;
	std::vector<uint8_t> basePixels;
	uint32_t imageWidth = 0;
	uint32_t imageHeight = 0;
	int movementOrientation = 0;
	bool hudAutoHide = true;
	bool r3DpadMode = false;
	uint32_t diagnosticMode = 0;
	bool leftHandedLayout = false;
	ResetState reset3dState = ResetReady;
	uint32_t selectedOption = 0;
	bool imageLoadAttempted = false;
	bool textureUploaded = false;
	ID3D12Device* activeDevice = nullptr;
	DXGI_FORMAT activeTargetFormat = DXGI_FORMAT_UNKNOWN;

	Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelineState;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> texture;
	Microsoft::WRL::ComPtr<ID3D12Resource> textureUpload;

	ID3D11Device* activeDevice11 = nullptr;
	Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader11;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader11;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture11;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureView11;
	Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler11;
};
