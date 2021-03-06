#include "Graphics.h"
#include "../Singleton/ShaderCompiler.h"
#include "../Singleton/ShaderID.h"
#include "../RenderComponent/Shader.h"
#include "../RenderComponent/ComputeShader.h"
#include "MeshLayout.h"
#include "../RenderComponent/DescriptorHeap.h"
#include "../RenderComponent/PSOContainer.h"
#include "../RenderComponent/StructuredBuffer.h"
#include "../RenderComponent/RenderTexture.h"
#include "../RenderComponent/Texture.h"
#include "../Common/Memory.h"
#include "../Utility/QuickSort.h"
#include "../RenderComponent/DescriptorHeapRoot.h"
#include "../PipelineComponent/ThreadCommand.h"
#include "../RenderComponent/UploadBuffer.h"
#include "../Utility/QuickSort.h"
#define MAXIMUM_HEAP_COUNT 32768
/*
namespace GraphicsGlobalN {
ComputeShader const* copyBufferCS;
uint _ReadBuffer_K;
uint _WriteBuffer_K;
uint _ReadBuffer;
uint _WriteBuffer;
}// namespace GraphicsGlobalN*/
StackObject<Mesh, true> Graphics::fullScreenMesh;
std::unique_ptr<DescriptorHeap> Graphics::globalDescriptorHeap;
BitArray Graphics::usedDescs(MAXIMUM_HEAP_COUNT);
ArrayList<uint, false> Graphics::unusedDescs(MAXIMUM_HEAP_COUNT);
spin_mutex Graphics::mtx;
StackObject<ElementAllocator> Graphics::srvAllocator;
StackObject<ElementAllocator> Graphics::rtvAllocator;
StackObject<ElementAllocator> Graphics::dsvAllocator;
ObjectPtr<Mesh> Graphics::cubeMesh = nullptr;
bool Graphics::enabled = false;
void Graphics::Initialize(GFXDevice* device, ThreadCommand* commandList) {
	//using namespace GraphicsGlobalN;
	/*_ReadBuffer_K = ShaderID::PropertyToID("_ReadBuffer_K");
	_WriteBuffer_K = ShaderID::PropertyToID("_WriteBuffer_K");
	_ReadBuffer = ShaderID::PropertyToID("_ReadBuffer");
	_WriteBuffer = ShaderID::PropertyToID("_WriteBuffer");
	copyBufferCS = ShaderCompiler::GetComputeShader("CopyBufferRegion");*/
	srvAllocator.New(
		65536,
		[=](uint64 size) -> void* {
			return new DescriptorHeapRoot(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, size, true);
		},
		[](void* ptr) -> void {
			delete reinterpret_cast<DescriptorHeapRoot*>(ptr);
		});
	rtvAllocator.New(
		2048,
		[=](uint64 size) -> void* {
			return new DescriptorHeapRoot(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, size, false);
		},
		[](void* ptr) -> void {
			delete reinterpret_cast<DescriptorHeapRoot*>(ptr);
		});
	dsvAllocator.New(
		2048,
		[=](uint64 size) -> void* {
			return new DescriptorHeapRoot(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, size, false);
		},
		[](void* ptr) -> void {
			delete reinterpret_cast<DescriptorHeapRoot*>(ptr);
		});
	enabled = true;
	MeshLayout::Initialize();
	globalDescriptorHeap = std::unique_ptr<DescriptorHeap>(new DescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MAXIMUM_HEAP_COUNT, true));
	static constexpr uint INIT_RTV_SIZE = 2048;
#pragma loop(hint_parallel(0))
	for (uint i = 0; i < MAXIMUM_HEAP_COUNT; ++i) {
		unusedDescs[i] = i;
	}
	std::array<float3, 3> vertex;
	std::array<float2, 3> uv;
	vertex[0] = {-3, -1, 1};
	vertex[1] = {1, -1, 1};
	vertex[2] = {1, 3, 1};
	uv[0] = {-1, 1};
	uv[1] = {1, 1};
	uv[2] = {1, -1};
	std::array<INT32, 3> indices = {0, 1, 2};
	fullScreenMesh.New(
		3,
		vertex.data(),
		nullptr,
		nullptr,
		nullptr,
		uv.data(),
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		nullptr,
		device,
		GFXFormat_R32_UInt,
		3,
		indices.data());
	cubeMesh = Mesh::LoadMeshFromFile(
		"Resource/Internal/Cube.vmesh",
		device, true, true, false, true, true, false, false, false);
}
uint Graphics::GetDescHeapIndexFromPool() {
	if (!enabled) return -1;
	std::lock_guard lck(mtx);
	if (unusedDescs.empty()) {
		VEngine_Log("No Global Descriptor Index Lefted!\n");
		throw 0;
	}
	uint value = unusedDescs.erase_last();
	usedDescs[value] = true;
	return value;
}
void Graphics::CopyTexture(
	ThreadCommand* commandList,
	RenderTexture const* source, uint sourceSlice, uint sourceMipLevel,
	RenderTexture const* dest, uint destSlice, uint destMipLevel) {
	commandList->ExecuteResBarrier();
	if (source->GetDimension() == TextureDimension::Tex2D) sourceSlice = 0;
	if (dest->GetDimension() == TextureDimension::Tex2D) destSlice = 0;
	D3D12_TEXTURE_COPY_LOCATION sourceLocation;
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	sourceLocation.SubresourceIndex = sourceSlice * source->GetMipCount() + sourceMipLevel;
	D3D12_TEXTURE_COPY_LOCATION destLocation;
	destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destLocation.SubresourceIndex = destSlice * dest->GetMipCount() + destMipLevel;
	sourceLocation.pResource = source->GetResource();
	destLocation.pResource = dest->GetResource();
	commandList->GetCmdList()->CopyTextureRegion(
		&destLocation,
		0, 0, 0,
		&sourceLocation,
		nullptr);
}
void Graphics::CopyBufferToBCTexture(
	ThreadCommand* commandList,
	UploadBuffer* sourceBuffer, size_t sourceBufferOffset,
	GFXResource* textureResource, uint targetMip,
	uint width, uint height, uint depth, GFXFormat targetFormat, uint pixelSize) {
	commandList->ExecuteResBarrier();
	D3D12_TEXTURE_COPY_LOCATION sourceLocation;
	sourceLocation.pResource = sourceBuffer->GetResource();
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sourceLocation.PlacedFootprint.Offset = sourceBufferOffset;
	sourceLocation.PlacedFootprint.Footprint =
		{
			(DXGI_FORMAT)targetFormat,							  //GFXFormat Format;
			width,												  //uint Width;
			height,												  //uint Height;
			depth,												  //uint Depth;
			GFXUtil::CalcConstantBufferByteSize(width * pixelSize)//uint RowPitch;
		};
	D3D12_TEXTURE_COPY_LOCATION destLocation;
	destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destLocation.SubresourceIndex = targetMip;
	destLocation.pResource = textureResource;
	commandList->GetCmdList()->CopyTextureRegion(
		&destLocation,
		0, 0, 0,
		&sourceLocation,
		nullptr);
}
void Graphics::CopyBufferToTexture(
	ThreadCommand* commandList,
	UploadBuffer* sourceBuffer, size_t sourceBufferOffset,
	GFXResource* textureResource, uint targetMip,
	uint width, uint height, uint depth, GFXFormat targetFormat, uint pixelSize) {
	commandList->ExecuteResBarrier();
	D3D12_TEXTURE_COPY_LOCATION sourceLocation;
	sourceLocation.pResource = sourceBuffer->GetResource();
	sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	sourceLocation.PlacedFootprint.Offset = sourceBufferOffset;
	sourceLocation.PlacedFootprint.Footprint =
		{
			(DXGI_FORMAT)targetFormat,							  //DXGI_FORMAT Format;
			width,												  //uint Width;
			height,												  //uint Height;
			depth,												  //uint Depth;
			GFXUtil::CalcConstantBufferByteSize(width * pixelSize)//uint RowPitch;
		};
	D3D12_TEXTURE_COPY_LOCATION destLocation;
	destLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	destLocation.SubresourceIndex = targetMip;
	destLocation.pResource = textureResource;
	commandList->GetCmdList()->CopyTextureRegion(
		&destLocation,
		0, 0, 0,
		&sourceLocation,
		nullptr);
}
void Graphics::Blit(
	ThreadCommand* commandList,
	GFXDevice* device,
	D3D12_CPU_DESCRIPTOR_HANDLE* renderTarget,
	GFXFormat* renderTargetFormats,
	uint renderTargetCount,
	D3D12_CPU_DESCRIPTOR_HANDLE* depthTarget,
	GFXFormat depthFormat,
	uint width, uint height,
	const Shader* shader, uint pass) {
	D3D12_VIEWPORT mViewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
	D3D12_RECT mScissorRect = {0, 0, width, height};
	PSODescriptor psoDesc;
	psoDesc.meshLayoutIndex = fullScreenMesh->GetLayoutIndex();
	psoDesc.shaderPass = pass;
	psoDesc.shaderPtr = shader;
	commandList->psoContainer.rtFormats.rtCount = renderTargetCount;
	memcpy(commandList->psoContainer.rtFormats.rtFormat, renderTargetFormats, sizeof(GFXFormat) * renderTargetCount);
	commandList->psoContainer.rtFormats.depthFormat = depthFormat;
	if (commandList->UpdateRenderTarget(renderTargetCount, renderTarget, depthTarget)) {
		commandList->GetCmdList()->RSSetViewports(1, &mViewport);
		commandList->GetCmdList()->RSSetScissorRects(1, &mScissorRect);
	}
	commandList->psoContainer.UpdateHash();
	auto pso = commandList->psoContainer.GetPSOState(psoDesc, device);
	commandList->UpdatePSO(pso);
	auto vbv = fullScreenMesh->VertexBufferViews();
	auto ibv = fullScreenMesh->IndexBufferView();
	commandList->GetCmdList()->IASetVertexBuffers(0, fullScreenMesh->VertexBufferViewCount(), vbv);
	commandList->GetCmdList()->IASetIndexBuffer(&ibv);
	commandList->GetCmdList()->IASetPrimitiveTopology(GetD3DTopology(psoDesc.topology));
	commandList->ExecuteResBarrier();
	commandList->GetCmdList()->DrawIndexedInstanced(fullScreenMesh->GetIndexCount(), 1, 0, 0, 0);
}
void Graphics::Blit(
	ThreadCommand* commandList,
	GFXDevice* device,
	const std::initializer_list<RenderTarget>& renderTarget,
	const RenderTarget& depthTarget,
	const Shader* shader, uint pass) {
	Blit(
		commandList,
		device,
		renderTarget.begin(),
		renderTarget.size(),
		depthTarget,
		shader,
		pass);
}
void Graphics::Blit(
	ThreadCommand* commandList,
	GFXDevice* device,
	RenderTarget const* rt,
	uint renderTargetCount,
	const RenderTarget& depthTarget,
	const Shader* shader, uint pass) {
	uint width = 0;
	uint height = 0;
	if (renderTargetCount > 0) {
		width = rt->rt->GetWidth() >> rt->mipCount;
		height = rt->rt->GetHeight() >> rt->mipCount;
	} else if (depthTarget.rt) {
		width = depthTarget.rt->GetWidth() >> depthTarget.mipCount;
		height = depthTarget.rt->GetHeight() >> depthTarget.mipCount;
	}
	D3D12_VIEWPORT mViewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
	D3D12_RECT mScissorRect = {0, 0, (int32_t)width, (int32_t)height};
	PSODescriptor psoDesc;
	psoDesc.meshLayoutIndex = fullScreenMesh->GetLayoutIndex();
	psoDesc.shaderPass = pass;
	psoDesc.shaderPtr = shader;
	commandList->SetRenderTarget(rt, renderTargetCount, depthTarget);
	auto pso = commandList->GetPSOState(psoDesc, device);
	commandList->UpdatePSO(pso);
	auto vbv = fullScreenMesh->VertexBufferViews();
	auto ibv = fullScreenMesh->IndexBufferView();
	commandList->GetCmdList()->IASetVertexBuffers(0, fullScreenMesh->VertexBufferViewCount(), vbv);
	commandList->GetCmdList()->IASetIndexBuffer(&ibv);
	commandList->GetCmdList()->IASetPrimitiveTopology(GetD3DTopology(psoDesc.topology));
	commandList->ExecuteResBarrier();
	commandList->GetCmdList()->DrawIndexedInstanced(fullScreenMesh->GetIndexCount(), 1, 0, 0, 0);
}
void Graphics::ReturnDescHeapIndexToPool(uint target) {
	if (!enabled) return;
	std::lock_guard lck(mtx);
	auto ite = usedDescs[target];
	if (ite) {
		unusedDescs.push_back(target);
		ite = false;
	}
}
void Graphics::ForceCollectAllHeapIndex() {
	if (!enabled) return;
	std::lock_guard lck(mtx);
	for (uint i = 0; i < MAXIMUM_HEAP_COUNT; ++i) {
		unusedDescs[i] = i;
	}
	usedDescs.Reset(false);
}
void Graphics::DrawMesh(
	GFXDevice* device,
	ThreadCommand* commandList,
	const IMesh* mesh,
	const Shader* shader, uint pass, uint instanceCount) {
	PSODescriptor desc;
	desc.meshLayoutIndex = mesh->GetLayoutIndex();
	desc.shaderPass = pass;
	desc.shaderPtr = shader;
	GFXPipelineState* pso = commandList->TryGetPSOStateAsync(desc, device);
	if (!pso) return;
	commandList->UpdatePSO(pso);
	auto vbv = mesh->VertexBufferViews();
	auto ibv = mesh->IndexBufferView();
	commandList->GetCmdList()->IASetVertexBuffers(0, mesh->VertexBufferViewCount(), vbv);
	commandList->GetCmdList()->IASetIndexBuffer(&ibv);
	commandList->GetCmdList()->IASetPrimitiveTopology(GetD3DTopology(desc.topology));
	commandList->ExecuteResBarrier();
	commandList->GetCmdList()->DrawIndexedInstanced(mesh->GetIndexCount(), instanceCount, 0, 0, 0);
}
void Graphics::DrawMeshes(
	GFXDevice* device,
	ThreadCommand* commandList,
	IMesh const** mesh, uint meshCount,
	const Shader* shader, uint pass, bool sort) {
	if (meshCount == 0) return;
	if (sort) {
		auto compareFunc = [](IMesh const* a, IMesh const* b) -> int32_t {
			return (int32_t)a->GetLayoutIndex() - (int32_t)b->GetLayoutIndex();
		};
		QuicksortStackCustomCompare<IMesh const*, decltype(compareFunc)>(
			mesh, compareFunc, 0, meshCount - 1);
	}
	PSODescriptor desc;
	desc.meshLayoutIndex = -1;
	desc.shaderPass = pass;
	desc.shaderPtr = shader;
	commandList->GetCmdList()->IASetPrimitiveTopology(GetD3DTopology(desc.topology));
	GFXPipelineState* pso = nullptr;
	commandList->ExecuteResBarrier();
	for (uint i = 0; i < meshCount; ++i) {
		IMesh const* m = mesh[i];
		if (m->GetLayoutIndex() != desc.meshLayoutIndex) {
			desc.meshLayoutIndex = m->GetLayoutIndex();
			pso = commandList->GetPSOState(desc, device);
			commandList->UpdatePSO(pso);
		}
		auto vbv = m->VertexBufferViews();
		auto ibv = m->IndexBufferView();
		commandList->GetCmdList()->IASetVertexBuffers(0, m->VertexBufferViewCount(), vbv);
		commandList->GetCmdList()->IASetIndexBuffer(&ibv);
		commandList->GetCmdList()->DrawIndexedInstanced(m->GetIndexCount(), 1, 0, 0, 0);
	}
}
void Graphics::CopyBufferRegion(
	ThreadCommand* commandList,
	IBuffer const* dest,
	uint64 destOffset,
	IBuffer const* source,
	uint64 sourceOffset,
	uint64 byteSize) {
	commandList->ExecuteResBarrier();
	commandList->GetCmdList()->CopyBufferRegion(
		dest->GetResource(),
		destOffset,
		source->GetResource(),
		sourceOffset,
		byteSize);
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	RenderTexture const* const* renderTargets,
	uint rtCount,
	RenderTexture const* depthTex) {
	D3D12_CPU_DESCRIPTOR_HANDLE* ptr = nullptr;
	if (rtCount > 0) {
		ptr = (D3D12_CPU_DESCRIPTOR_HANDLE*)alloca(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * rtCount);
		for (uint i = 0; i < rtCount; ++i) {
			ptr[i] = renderTargets[i]->GetColorDescriptor(0, 0);
		}
	}
	if (depthTex) {
		auto col = depthTex->GetColorDescriptor(0, 0);
		if (commandList->UpdateRenderTarget(rtCount, ptr, &col)) {
			depthTex->SetViewport(commandList);
		}
	} else if (commandList->UpdateRenderTarget(rtCount, ptr, nullptr)) {
		renderTargets[0]->SetViewport(commandList);
	}
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	const std::initializer_list<RenderTexture const*>& rtLists,
	RenderTexture const* depthTex) {
	RenderTexture const* const* renderTargets = rtLists.begin();
	size_t rtCount = rtLists.size();
	SetRenderTarget(
		commandList, renderTargets, rtCount,
		depthTex);
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	const RenderTarget* renderTargets,
	uint rtCount,
	const RenderTarget& depth) {
	D3D12_CPU_DESCRIPTOR_HANDLE* ptr = nullptr;
	if (rtCount > 0) {
		renderTargets[0].rt->SetViewport(commandList, renderTargets[0].mipCount);
		ptr = (D3D12_CPU_DESCRIPTOR_HANDLE*)alloca(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * rtCount);
		for (uint i = 0; i < rtCount; ++i) {
			const RenderTarget& rt = renderTargets[i];
			ptr[i] = rt.rt->GetColorDescriptor(rt.depthSlice, rt.mipCount);
		}
	} else if (depth.rt) {
		depth.rt->SetViewport(commandList, depth.mipCount);
	}
	if (depth.rt) {
		auto rrt = depth.rt->GetColorDescriptor(depth.depthSlice, depth.mipCount);
		commandList->UpdateRenderTarget(rtCount, ptr, &rrt);
	} else {
		commandList->UpdateRenderTarget(rtCount, ptr, nullptr);
	}
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	const std::initializer_list<RenderTarget>& init,
	const RenderTarget& depth) {
	const RenderTarget* renderTargets = init.begin();
	const size_t rtCount = init.size();
	SetRenderTarget(commandList, renderTargets, rtCount, depth);
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	const RenderTarget* renderTargets,
	uint rtCount) {
	if (rtCount > 0) {
		renderTargets[0].rt->SetViewport(commandList, renderTargets[0].mipCount);
		D3D12_CPU_DESCRIPTOR_HANDLE* ptr = (D3D12_CPU_DESCRIPTOR_HANDLE*)alloca(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * rtCount);
		for (uint i = 0; i < rtCount; ++i) {
			const RenderTarget& rt = renderTargets[i];
			ptr[i] = rt.rt->GetColorDescriptor(rt.depthSlice, rt.mipCount);
		}
		commandList->UpdateRenderTarget(rtCount, ptr, nullptr);
	}
}
void Graphics::SetRenderTarget(
	ThreadCommand* commandList,
	const std::initializer_list<RenderTarget>& init) {
	const RenderTarget* renderTargets = init.begin();
	const size_t rtCount = init.size();
	SetRenderTarget(commandList, renderTargets, rtCount);
}
void Graphics::Dispose() {
	enabled = false;
	cubeMesh.Destroy();
	globalDescriptorHeap = nullptr;
	unusedDescs.dispose();
	MeshLayout::Dispose();
	fullScreenMesh.Delete();
	srvAllocator.Delete();
	rtvAllocator.Delete();
	dsvAllocator.Delete();
}
#undef MAXIMUM_HEAP_COUNT
