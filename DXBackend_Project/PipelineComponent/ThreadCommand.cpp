#include "ThreadCommand.h"
#include "ThreadCommand.h"
//#endif
#include "ThreadCommand.h"
#include "../RenderComponent/RenderTexture.h"
#include "../RenderComponent/StructuredBuffer.h"
#include "../Singleton/Graphics.h"
#include "../RenderComponent/IShader.h"
#include "../RenderComponent/DescriptorHeapRoot.h"
void ThreadCommand::ResetCommand() {
	shaderRootInstanceID = 0;
	descHeapInstanceID = 0;
	pso = nullptr;
	colorHandles.clear();
	depthHandle.ptr = 0;

	
	auto alloc = cmdAllocator->GetAllocator().Get();
	if (managingAllocator) {
		ThrowIfFailed(alloc->Reset());
	} else {
		cmdAllocator->Reset(frameCount);
	}
	ThrowIfFailed(cmdList->Reset(alloc, nullptr));
}
void ThreadCommand::CloseCommand() {
	rtStateMap.IterateAll([&](RenderTexture* const key, ResourceReadWriteState& value) -> void {
		if (value) {
			GFXResourceState writeState, readState;
			writeState = key->GetWriteState();
			readState = key->GetReadState();
			UpdateResState(readState, writeState, key);
		}
	});
	sbufferStateMap.IterateAll([&](StructuredBuffer* const key, ResourceReadWriteState& value) -> void {
		if (value) {
			UpdateResState(GFXResourceState_GenericRead, GFXResourceState_UnorderedAccess, key);
		}
	});
	Clear();
	rtStateMap.Clear();
	sbufferStateMap.Clear();
	cmdList->Close();
}
bool ThreadCommand::UpdateRegisterShader(IShader const* shader) {
	uint64 id = shader->GetInstanceID();
	if (id == shaderRootInstanceID)
		return false;
	shaderRootInstanceID = id;
	return true;
}
bool ThreadCommand::UpdateDescriptorHeap(DescriptorHeapRoot const* descHeap) {
	uint64 id = descHeap->GetInstanceID();
	if (id == descHeapInstanceID)
		return false;
	descHeapInstanceID = id;
	ID3D12DescriptorHeap* heap = descHeap->pDH.Get();
	cmdList->SetDescriptorHeaps(1, &heap);
	return true;
}
bool ThreadCommand::UpdatePSO(void* psoObj) {
	if (pso == psoObj)
		return false;
	pso = psoObj;
	cmdList->SetPipelineState(static_cast<ID3D12PipelineState*>(psoObj));
	return true;
}
bool ThreadCommand::UpdateRenderTarget(
	uint NumRenderTargetDescriptors,
	const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
	const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
	D3D12_CPU_DESCRIPTOR_HANDLE curDepthHandle = {0};
	auto originDepth = pDepthStencilDescriptor;
	if (!pDepthStencilDescriptor) {
		pDepthStencilDescriptor = &curDepthHandle;
	}
	auto Disposer = [&]() -> void {
		colorHandles.clear();
		colorHandles.resize(NumRenderTargetDescriptors);
		memcpy(colorHandles.data(), pRenderTargetDescriptors, sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) * NumRenderTargetDescriptors);
		depthHandle = *pDepthStencilDescriptor;
		cmdList->OMSetRenderTargets(
			NumRenderTargetDescriptors,
			pRenderTargetDescriptors,
			false,
			originDepth);
	};
	if (colorHandles.size() != NumRenderTargetDescriptors) {
		Disposer();
		return true;
	}
	if (pDepthStencilDescriptor->ptr != depthHandle.ptr) {
		Disposer();
		return true;
	}
	for (uint i = 0; i < NumRenderTargetDescriptors; ++i) {
		if (colorHandles[i].ptr != pRenderTargetDescriptors[i].ptr) {
			Disposer();
			return true;
		}
	}

	return false;
}

void ThreadCommand::RegistInitState(GFXResourceState initState, GPUResourceBase const* resource) {
	auto ite = barrierRecorder.Find(resource->GetInstanceID());
	if (!ite) {
		barrierRecorder.Insert(resource->GetInstanceID(), ResourceBarrierCommand(resource, initState, -1));
	}
}

void ThreadCommand::UpdateResState(GFXResourceState newState, GPUResourceBase const* resource) {
	containedResources = true;
	auto ite = barrierRecorder.Find(resource->GetInstanceID());
	if (!ite) {
		return;
	}
	auto&& cmd = ite.Value();
	if (cmd.index < 0) {
		if (cmd.targetState != newState) {
			cmd.index = resourceBarrierCommands.size();
			resourceBarrierCommands.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
				resource->GetResource(),
				cmd.targetState,
				newState));
			cmd.targetState = newState;
		}
	} else {
		if (resourceBarrierCommands.empty()) return;
		resourceBarrierCommands[cmd.index].Transition.StateAfter = (D3D12_RESOURCE_STATES)newState;
		cmd.targetState = newState;
	}
}
void ThreadCommand::UAVBarrier(GPUResourceBase const* res) {
	containedResources = true;
	auto ite = uavBarriersDict.Find(res->GetResource());
	if (!ite) {
		uavBarriersDict.Insert(res->GetResource(), true);
		uavBarriers.push_back(res->GetResource());
	}
}
void ThreadCommand::UAVBarriers(const std::initializer_list<GPUResourceBase const*>& args) {
	containedResources = true;
	for (auto i = args.begin(); i != args.end(); ++i) {
		UAVBarrier(*i);
	}
}
void ThreadCommand::AliasBarrier(GPUResourceBase const* before, GPUResourceBase const* after) {
	containedResources = true;
	auto ite = aliasBarriersDict.Find({before->GetResource(), after->GetResource()});
	if (!ite) {
		aliasBarriersDict.Insert({before->GetResource(), after->GetResource()}, true);
		aliasBarriers.push_back({before->GetResource(), after->GetResource()});
	}
}
void ThreadCommand::AliasBarriers(std::initializer_list<std::pair<GPUResourceBase const*, GPUResourceBase const*>> const& lst) {
	containedResources = true;
	for (auto& i : lst) {
		AliasBarrier(i.first, i.second);
	}
}

void ThreadCommand::KillSame() {
	for (size_t i = 0; i < resourceBarrierCommands.size(); ++i) {
		auto& a = resourceBarrierCommands[i];
		if (a.Transition.StateBefore == a.Transition.StateAfter) {
			auto last = resourceBarrierCommands.end() - 1;
			if (i != (resourceBarrierCommands.size() - 1)) {
				a = *last;
			}
			resourceBarrierCommands.erase(last);
			i--;
		} else {
			auto uavIte = uavBarriersDict.Find(a.Transition.pResource);
			if (uavIte) {
				uavIte.Value() = false;
			}
		}
	}
	{
		D3D12_RESOURCE_BARRIER uavBar;
		uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		uavBar.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		for (auto ite = uavBarriers.begin(); ite != uavBarriers.end(); ++ite) {
			uavBar.UAV.pResource = *ite;
			if (uavBarriersDict[uavBar.UAV.pResource]) {
				resourceBarrierCommands.push_back(uavBar);
			}
		}
	}
	D3D12_RESOURCE_BARRIER aliasBarrier;
	aliasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
	aliasBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	for (auto& i : aliasBarriers) {
		aliasBarrier.Aliasing.pResourceBefore = i.first;
		aliasBarrier.Aliasing.pResourceAfter = i.second;
		resourceBarrierCommands.push_back(aliasBarrier);
	}
}
void ThreadCommand::ExecuteResBarrier() {
	if (!containedResources) return;
	containedResources = false;
	KillSame();
	if (!resourceBarrierCommands.empty()) {
		cmdList->ResourceBarrier(resourceBarrierCommands.size(), resourceBarrierCommands.data());
		resourceBarrierCommands.clear();
	}
	uavBarriersDict.Clear();
	uavBarriers.clear();
	aliasBarriers.clear();
	aliasBarriersDict.Clear();
	barrierRecorder.IterateAll([](ResourceBarrierCommand& cmd) -> void {
		cmd.index = -1;
	});
}
void ThreadCommand::Clear() {
	if (!containedResources) return;
	containedResources = false;
	KillSame();
	if (!resourceBarrierCommands.empty()) {
		cmdList->ResourceBarrier(resourceBarrierCommands.size(), resourceBarrierCommands.data());
		resourceBarrierCommands.clear();
	}
	uavBarriersDict.Clear();
	uavBarriers.clear();
	barrierRecorder.Clear();
	aliasBarriers.clear();
	aliasBarriersDict.Clear();
}
void ThreadCommand::UpdateResState(GFXResourceState beforeState, GFXResourceState afterState, GPUResourceBase const* resource) {
	containedResources = true;
	auto ite = barrierRecorder.Find(resource->GetInstanceID());
	if (!ite) {
		barrierRecorder.Insert(resource->GetInstanceID(), ResourceBarrierCommand(resource, afterState, resourceBarrierCommands.size()));
		resourceBarrierCommands.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
			resource->GetResource(),
			beforeState,
			afterState));
	} else if (ite.Value().index < 0) {
		auto&& cmd = ite.Value();
		cmd.index = resourceBarrierCommands.size();
		if (cmd.targetState != beforeState) {
			VEngine_Log("TransitionBarrier add ResourceBarrierCommand error! before state unmatched!\n");
			throw 0;
		} else if (cmd.targetState != afterState) {
			resourceBarrierCommands.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
				resource->GetResource(),
				cmd.targetState,
				afterState));
			cmd.targetState = afterState;
		}
	} else {
		auto&& cmd = ite.Value();
		resourceBarrierCommands[cmd.index].Transition.StateAfter = (D3D12_RESOURCE_STATES)afterState;
		cmd.targetState = afterState;
	}
}

ThreadCommand::ThreadCommand(GFXDevice* device, GFXCommandListType type, ObjectPtr<CommandAllocator> const& allocator)
	: rtStateMap(23),
	  sbufferStateMap(23),
	  barrierRecorder(32),
	  uavBarriersDict(32),
	  aliasBarriersDict(32) {

	resourceBarrierCommands.reserve(32);
	uavBarriers.reserve(32);
	if (allocator) {
		cmdAllocator = allocator;
		managingAllocator = false;
	} else {
		managingAllocator = true;
		cmdAllocator = MakeObjectPtr(
			new CommandAllocator(device, type));
	}
	ThrowIfFailed(device->CreateCommandList(
		0,
		(D3D12_COMMAND_LIST_TYPE)type,
		cmdAllocator->GetAllocator().Get(),// Associated command allocator
		nullptr,						   // Initial PipelineStateObject
		IID_PPV_ARGS(&cmdList)));
	cmdList->Close();
}
ThreadCommand::~ThreadCommand() {
}
bool ThreadCommand::UpdateResStateLocal(RenderTexture* rt, ResourceReadWriteState state) {
	auto ite = rtStateMap.Find(rt);
	if (state) {
		if (!ite) {
			rtStateMap.Insert(rt, state);
		} else if (ite.Value() != state) {
			ite.Value() = state;
		} else
			return false;
	} else {
		if (ite) {
			rtStateMap.Remove(ite);
		} else
			return false;
	}
	return true;
}
bool ThreadCommand::UpdateResStateLocal(StructuredBuffer* rt, ResourceReadWriteState state) {
	auto ite = sbufferStateMap.Find(rt);
	if (state) {
		if (!ite) {
			sbufferStateMap.Insert(rt, state);
		} else if (ite.Value() != state) {
			ite.Value() = state;
		} else
			return false;
	} else {
		if (ite) {
			sbufferStateMap.Remove(ite);
		} else
			return false;
	}
	return true;
}
void ThreadCommand::SetResourceReadWriteState(RenderTexture* rt, ResourceReadWriteState state) {
	if (!UpdateResStateLocal(rt, state)) return;
	GFXResourceState writeState, readState;
	writeState = rt->GetWriteState();
	readState = rt->GetReadState();
	if (state) {
		UpdateResState(writeState, readState, rt);
	} else {
		UpdateResState(readState, writeState, rt);
	}
}
void ThreadCommand::SetResourceReadWriteState(StructuredBuffer* rt, ResourceReadWriteState state) {
	if (!UpdateResStateLocal(rt, state)) return;
	if (state) {
		UpdateResState(GFXResourceState_UnorderedAccess, GFXResourceState_GenericRead, rt);
	} else {
		UpdateResState(GFXResourceState_GenericRead, GFXResourceState_UnorderedAccess, rt);
	}
}
