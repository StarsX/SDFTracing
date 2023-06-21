//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Renderer.h"
#include "SharedConst.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

#define GRID_SIZE 128

struct CBPerFrame
{
	DirectX::XMFLOAT4X4 ViewProj;
	DirectX::XMFLOAT3X4 VolumeWorld;
	DirectX::XMFLOAT3X4 VolumeWorldI;
	uint32_t SampleIndex;
};

struct PerObject
{
	DirectX::XMFLOAT3X4 World;
	DirectX::XMFLOAT3X4 WorldIT;
};

struct LightSource
{
	DirectX::XMFLOAT4 Min;
	DirectX::XMFLOAT4 Max;
	DirectX::XMFLOAT4 Emissive;
	DirectX::XMFLOAT3X4 World;
};

Renderer::Renderer() :
	m_instances(),
	m_frameIndex(0)
{
	m_shaderLib = ShaderLib::MakeUnique();
}

Renderer::~Renderer()
{
}

bool Renderer::Init(RayTracing::EZ::CommandList* pCommandList, vector<Resource::uptr>& uploaders,
	uint32_t meshCount, GeometryBuffer* pGeometries, const MeshDesc* pMeshDescs)
{
	const auto pDevice = pCommandList->GetRTDevice();

	// Load inputs
	m_meshes.resize(meshCount);
	vector<uint32_t> dynamicMeshIds(meshCount);
	vector<GltfLoader::LightSource> lightSources(0);
	m_dynamicMeshes.clear();
	XMVECTOR sceneAABB[2], meshAABB[2];
	sceneAABB[0] = XMVectorReplicate(FLT_MAX);
	sceneAABB[1] = XMVectorReplicate(-FLT_MAX);
	for (auto i = 0u; i < meshCount; ++i)
	{
		loadMesh(pCommandList, i, uploaders, pMeshDescs, dynamicMeshIds, lightSources);

		calcMeshWorldAABB(meshAABB, i);
		sceneAABB[0] = XMVectorMin(meshAABB[0], sceneAABB[0]);
		sceneAABB[1] = XMVectorMax(meshAABB[1], sceneAABB[1]);
	}

	// Calculate volume world matrix
	XMFLOAT3 volumeExt;
	const auto volumeCenter = (sceneAABB[0] + sceneAABB[1]) * 0.5f;
	XMStoreFloat3(&volumeExt, (sceneAABB[1] - sceneAABB[0]) * 0.5f);
	const auto radius = (max)((max)(volumeExt.x, volumeExt.y), volumeExt.z) * 1.08f;
	const auto volumeWorld = XMMatrixScaling(radius, radius, radius) *
		XMMatrixTranslationFromVector(volumeCenter);
	XMStoreFloat3x4(&m_volumeWorld, volumeWorld);

	// Create constant buffers
	XUSG_N_RETURN(createCBs(pDevice), false);

	// Create buffers
	const auto lightSourceCount = static_cast<uint32_t>(lightSources.size());
	assert(lightSourceCount == m_lightSourceMeshIds.size());
	if (lightSourceCount > 0)
	{
		for (auto& lightSourceBuffer : m_lightSources)
		{
			lightSourceBuffer = StructuredBuffer::MakeUnique();
			XUSG_N_RETURN(lightSourceBuffer->Create(pDevice, lightSourceCount, sizeof(LightSource), ResourceFlag::NONE,
				MemoryType::UPLOAD, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"LightSources"), false); // MemoryType::UPLOAD  CPU 

			for (auto i = 0u; i < lightSourceCount; ++i)
			{
				const auto pData = static_cast<GltfLoader::LightSource*>(lightSourceBuffer->Map());
				*pData = lightSources[i];
			}
		}
	}

	for (auto& matrices : m_matrices)
	{
		matrices = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(matrices->Create(pDevice, meshCount, sizeof(PerObject), ResourceFlag::NONE,
			MemoryType::UPLOAD, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"Matrices"), false);
	}

	m_globalSDF = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_globalSDF->Create(pDevice, GRID_SIZE, GRID_SIZE, GRID_SIZE, Format::R32_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, MemoryFlag::NONE, L"GlobalSDF"), false);

	m_idVolume = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_idVolume->Create(pDevice, GRID_SIZE, GRID_SIZE, GRID_SIZE, Format::R32_UINT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, MemoryFlag::NONE, L"IdVolume"), false);

	m_barycVolume = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_barycVolume->Create(pDevice, GRID_SIZE, GRID_SIZE, GRID_SIZE, Format::R16G16_UNORM,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, MemoryFlag::NONE, L"BarycentricsVolume"), false);

	const uint32_t gridSize = GRID_SIZE;
	const auto mipCount = CalculateMipLevels(gridSize, gridSize, gridSize);
	m_irradiance = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_irradiance->Create(pDevice, gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, mipCount, MemoryFlag::NONE, L"IrradianceVolume"), false);

	{
		m_meshBounds = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_meshBounds->Create(pDevice, meshCount, sizeof(XMFLOAT4), ResourceFlag::NONE,
			MemoryType::DEFAULT, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"MeshBounds"), false);
		uploaders.emplace_back(Resource::MakeUnique());

		vector<XMFLOAT4> bounds(meshCount);
		for (auto i = 0u; i < meshCount; ++i) bounds[i] = m_meshes[i].Bound;

		XUSG_N_RETURN(m_meshBounds->Upload(pCommandList->AsCommandList(),
			uploaders.back().get(), bounds.data(), sizeof(XMFLOAT4) * bounds.size()), false);
	}

	// For dynamic meshes
	{
		// Allocate dynamic mesh buffer
		m_dynamicMeshList = StructuredBuffer::MakeUnique(); // create buffer, upload to GPU
		XUSG_N_RETURN(m_dynamicMeshList->Create(pDevice, static_cast<uint32_t>(m_dynamicMeshes.size()),
			sizeof(DynamicMesh), ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 0, nullptr,
			MemoryFlag::NONE, L"DynamicMeshes"), false); // create dynamic meshes GPU
		uploaders.emplace_back(Resource::MakeUnique());

		XUSG_N_RETURN(m_dynamicMeshList->Upload(pCommandList->AsCommandList(), uploaders.back().get(),
			m_dynamicMeshes.data(), m_dynamicMeshes.size() * sizeof(DynamicMesh)), false);
	}

	{
		m_dynamicMeshIds = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_dynamicMeshIds->Create(pDevice, meshCount, sizeof(uint32_t),
			ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 0, nullptr, MemoryFlag::NONE,
			L"DynamicMeshIds"), false);
		uploaders.emplace_back(Resource::MakeUnique());

		XUSG_N_RETURN(m_dynamicMeshIds->Upload(pCommandList->AsCommandList(), uploaders.back().get(),
			dynamicMeshIds.data(), sizeof(uint32_t) * meshCount), false);
	}

	// Build acceleration structures and create shaders
	XUSG_N_RETURN(buildAccelerationStructures(pCommandList, pGeometries), false);
	XUSG_N_RETURN(createShaders(), false);

	return true;
}

bool Renderer::SetViewport(const XUSG::Device* pDevice, uint32_t width, uint32_t height)
{
	m_viewport.x = width;
	m_viewport.y = height;

	// Create textures
	m_visibility = RenderTarget::MakeUnique();
	XUSG_N_RETURN(m_visibility->Create(pDevice, width, height, Format::R32_UINT, 1,
		ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"Visibility"), false);

	m_outputView = Texture::MakeUnique();
	XUSG_N_RETURN(m_outputView->Create(pDevice, width, height, Format::R8G8B8A8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"OutputView"), false);

	return true;
}

void Renderer::UpdateFrame(double time, uint8_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	m_timeStart = m_frameIndex < VOX_SAMPLE_COUNT ? time : m_timeStart;
	m_time = time;

	const auto volumeWorld = XMLoadFloat3x4(&m_volumeWorld);
	const auto pCbPerFrame = static_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
	XMStoreFloat4x4(&pCbPerFrame->ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat3x4(&pCbPerFrame->VolumeWorldI, XMMatrixInverse(nullptr, volumeWorld));
	pCbPerFrame->VolumeWorld = m_volumeWorld;
	pCbPerFrame->SampleIndex = m_frameIndex;

	const auto meshCount = static_cast<uint32_t>(m_meshes.size());
	const auto pPerObject = static_cast<PerObject*>(m_matrices[frameIndex]->Map());
	for (auto i = 0u; i < meshCount; ++i)
	{
		const auto world = getWorldMatrix(i);
		const auto worldIT = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
		XMStoreFloat3x4(&pPerObject[i].World, world);		// 3x4 store doesn't need matrix transpose
		XMStoreFloat3x4(&pPerObject[i].WorldIT, worldIT);	// 3x4 store doesn't need matrix transpose
	}

	const auto lightSourceCount = static_cast<uint32_t>(m_lightSourceMeshIds.size());
	for (auto i = 0u; i < lightSourceCount; ++i)
	{
		const auto world = getWorldMatrix(m_lightSourceMeshIds[i]);
		const auto pData = static_cast<LightSource*>(m_lightSources[frameIndex]->Map());
		XMStoreFloat3x4(&pData->World, world); // 3x4 store doesn't need matrix transpose
	}
}

void Renderer::Render(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
	RenderTarget* pRenderTarget, DepthStencil* pDepthStencil)
{
	if (m_frameIndex < VOX_SAMPLE_COUNT)
	{
		if (m_frameIndex == 0)
		{
			const float clear[] = { FLT_MAX };
			const auto uav = XUSG::EZ::GetUAV(m_globalSDF.get());
			pCommandList->ClearUnorderedAccessViewFloat(uav, clear);
		}
		buildSDF(pCommandList, frameIndex);
		++m_frameIndex;
	}
	else
	{
		updateAccelerationStructures(pCommandList, frameIndex);
		updateSDF(pCommandList, frameIndex);
	}
	
	visibility(pCommandList, frameIndex, pDepthStencil);
	renderVolume(pCommandList, frameIndex);
	pCommandList->GenerateMips(m_irradiance.get(), LINEAR_CLAMP);
	render(pCommandList, frameIndex);
	antiAlias(pCommandList, pRenderTarget);
}

bool Renderer::loadMesh(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, vector<Resource::uptr>& uploaders,
	const MeshDesc* pMeshDescs, vector<uint32_t>& dynamicMeshIds, vector<GltfLoader::LightSource>& lightSources)
{
	GltfLoader loader;
	if (!loader.Import(pMeshDescs[meshId].FileName.c_str(), true, true, true, true)) return false;

	const auto meshLightSources = loader.GetLightSources();
	lightSources.insert(lightSources.end(), meshLightSources.cbegin(), meshLightSources.cend());
	for (const auto& meshLightSource : meshLightSources)
		m_lightSourceMeshIds.emplace_back(meshId);

	XUSG_N_RETURN(createMeshVB(pCommandList, meshId, loader.GetNumVertices(), loader.GetVertexStride(), loader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createMeshIB(pCommandList, meshId, loader.GetNumIndices(), loader.GetIndices(), uploaders), false);
	XUSG_N_RETURN(createMeshCB(pCommandList, meshId, uploaders), false);

	m_meshes[meshId].PosScale = pMeshDescs[meshId].PosScale;

	const auto center = loader.GetCenter();
	m_meshes[meshId].Bound = XMFLOAT4(center.x, center.y, center.z, loader.GetRadius());

	if (pMeshDescs[meshId].IsDynamic)
	{
		// !!! isDynamic bool variable, set bunny true
		dynamicMeshIds[meshId] = static_cast<uint32_t>(m_dynamicMeshes.size());
		m_dynamicMeshes.push_back({ meshId });
	}
	else dynamicMeshIds[meshId] = UINT32_MAX;

	m_meshes[meshId].Name = pMeshDescs[meshId].FileName.substr(0, pMeshDescs[meshId].FileName.find(".gltf"));
	m_meshes[meshId].IsDynamic = pMeshDescs[meshId].IsDynamic;

	return true;
}

bool Renderer::createMeshVB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_meshes[meshId].VertexBuffer = VertexBuffer::MakeUnique();

	XUSG_N_RETURN(m_meshes[meshId].VertexBuffer->Create(pCommandList->GetDevice(), numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());//!!!

	return m_meshes[meshId].VertexBuffer->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), pData, stride * numVert);
}

bool Renderer::createMeshIB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_meshes[meshId].IndexBuffer = IndexBuffer::MakeUnique();
	m_meshes[meshId].NumIndices = numIndices;

	XUSG_N_RETURN(m_meshes[meshId].IndexBuffer->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT,
		ResourceFlag::NONE, MemoryType::DEFAULT), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_meshes[meshId].IndexBuffer->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), pData, byteWidth);

	return true;
}

bool Renderer::createMeshCB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, vector<Resource::uptr>& uploaders)
{
	m_meshes[meshId].CbPerObject = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_meshes[meshId].CbPerObject->Create(pCommandList->GetDevice(), sizeof(uint32_t),
		1, nullptr, MemoryType::DEFAULT, MemoryFlag::NONE, (L"CBMeshId" + to_wstring(meshId)).c_str()), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_meshes[meshId].CbPerObject->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), &meshId, sizeof(meshId));
}

bool Renderer::createCBs(const XUSG::Device* pDevice)
{
	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBPerFrame) * FrameCount, FrameCount), false);

	return true;
}

bool Renderer::createShaders()
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSBuildSDF.cso"), false);
	m_shaders[CS_BUILD_SDF] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSUpdateSDF.cso"), false);
	m_shaders[CS_UPDATE_SDF] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSShadeVolume.cso"), false);
	m_shaders[CS_SHADE_VOLUME] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSShade.cso"), false);
	m_shaders[CS_SHADE] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSVisibility.cso"), false);
	m_shaders[VS_VISIBILITY] = m_shaderLib->GetShader(Shader::Stage::VS, vsIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	m_shaders[VS_SCREEN_QUAD] = m_shaderLib->GetShader(Shader::Stage::VS, vsIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSVisibility.cso"), false);
	m_shaders[PS_VISIBILITY] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSFXAA.cso"), false);
	m_shaders[PS_FXAA] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	return true;
}

bool Renderer::buildAccelerationStructures(RayTracing::EZ::CommandList* pCommandList, GeometryBuffer* pGeometries)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	const auto meshCount = static_cast<uint32_t>(m_meshes.size());
	for (auto i = 0u; i < meshCount; ++i)
	{
		// Set geometry
		auto vbv = XUSG::EZ::GetVBV(m_meshes[i].VertexBuffer.get());
		auto ibv = XUSG::EZ::GetIBV(m_meshes[i].IndexBuffer.get());
		pCommandList->SetTriangleGeometries(pGeometries[i], 1, Format::R32G32B32_FLOAT, &vbv, &ibv);

		// Prebuild BLAS
		m_meshes[i].BottomLevelAS = BottomLevelAS::MakeUnique();
		XUSG_N_RETURN(pCommandList->PreBuildBLAS(m_meshes[i].BottomLevelAS.get(), 1, pGeometries[i]), false);
	}

	// Prebuild TLAS
	m_topLevelAS = TopLevelAS::MakeUnique();
	XUSG_N_RETURN(pCommandList->PreBuildTLAS(m_topLevelAS.get(), meshCount, BuildFlag::ALLOW_UPDATE | BuildFlag::PREFER_FAST_TRACE), false);

	// Set instance
	vector<XMFLOAT3X4> matrices(meshCount);
	vector<float*> pTransforms(meshCount);
	vector<const BottomLevelAS*> pBottomLevelASs(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		XMStoreFloat3x4(&matrices[i], getWorldMatrix(i));
		pTransforms[i] = reinterpret_cast<float*>(&matrices[i]);
		pBottomLevelASs[i] = m_meshes[i].BottomLevelAS.get();
	}
	for (auto& instances : m_instances) instances = Resource::MakeUnique();
	TopLevelAS::SetInstances(pCommandList->GetRTDevice(), m_instances->get(), meshCount, pBottomLevelASs.data(), pTransforms.data());

	// Build bottom level ASs
	for (auto i = 0u; i < meshCount; ++i)
		pCommandList->BuildBLAS(m_meshes[i].BottomLevelAS.get());

	// Build top level AS
	pCommandList->BuildTLAS(m_topLevelAS.get(), m_instances->get());

	return true;
}

void Renderer::buildSDF(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_BUILD_SDF]);

	// Set UAVs
	const XUSG::EZ::ResourceView uavs[] =
	{
		XUSG::EZ::GetUAV(m_globalSDF.get()),
		XUSG::EZ::GetUAV(m_idVolume.get()),
		XUSG::EZ::GetUAV(m_barycVolume.get()),
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, static_cast<uint32_t>(size(uavs)), uavs);

	// Set CBV
	const auto cbv = XUSG::EZ::GetCBV(m_cbPerFrame.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, 1, &cbv);

	// Set SRV
	const auto srv = RayTracing::EZ::GetSRV(m_topLevelAS.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, 1, &srv);

	pCommandList->Dispatch(XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4));
}

void Renderer::updateSDF(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_UPDATE_SDF]);

	// Set UAVs
	const XUSG::EZ::ResourceView uavs[] =
	{
		XUSG::EZ::GetUAV(m_globalSDF.get()),
		XUSG::EZ::GetUAV(m_idVolume.get()),
		XUSG::EZ::GetUAV(m_barycVolume.get()),
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, static_cast<uint32_t>(size(uavs)), uavs);

	// Set CBV
	const auto cbv = XUSG::EZ::GetCBV(m_cbPerFrame.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, 1, &cbv);

	// Set SRVs
	const XUSG::EZ::ResourceView srvs[] =
	{
		RayTracing::EZ::GetSRV(m_topLevelAS.get()),
		XUSG::EZ::GetSRV(m_matrices[frameIndex].get()),
		XUSG::EZ::GetSRV(m_dynamicMeshIds.get()),
		XUSG::EZ::GetSRV(m_dynamicMeshList.get()),
		XUSG::EZ::GetSRV(m_meshBounds.get())
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);

	pCommandList->Dispatch(XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4));
}

void Renderer::updateAccelerationStructures(RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set instance
	static vector<XMFLOAT3X4> matrices(meshCount);
	static vector<float*> pTransforms(meshCount);
	static vector<const BottomLevelAS*> pBottomLevelASs(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		XMStoreFloat3x4(&matrices[i], getWorldMatrix(i));
		pTransforms[i] = reinterpret_cast<float*>(&matrices[i]);
		pBottomLevelASs[i] = m_meshes[i].BottomLevelAS.get();
	}
	TopLevelAS::SetInstances(pCommandList->GetRTDevice(), m_instances[frameIndex].get(), meshCount, pBottomLevelASs.data(), pTransforms.data());

	// Update top level AS
	pCommandList->BuildTLAS(m_topLevelAS.get(), m_instances[frameIndex].get(), true);
}

void Renderer::visibility(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex, DepthStencil* pDepthStencil)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_VISIBILITY]);
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_VISIBILITY]);
	pCommandList->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS);
	pCommandList->RSSetState(Graphics::RasterizerPreset::CULL_BACK);
	pCommandList->OMSetBlendState(Graphics::BlendPreset::DEFAULT_OPAQUE);

	// Set render target
	const auto rtv = XUSG::EZ::GetRTV(m_visibility.get());
	const auto dsv = XUSG::EZ::GetDSV(pDepthStencil);
	pCommandList->OMSetRenderTargets(1, &rtv, &dsv);

	// Clear render target
	float clearColor[4] = {};
	pCommandList->ClearRenderTargetView(rtv, clearColor);
	pCommandList->ClearDepthStencilView(dsv, ClearFlag::DEPTH, 1.0f);

	// Set Per-frame CBV
	XUSG::EZ::ResourceView cbvs[2];
	cbvs[1] = XUSG::EZ::GetCBV(m_cbPerFrame.get(), frameIndex);

	// Set SRVs
	const auto srv = XUSG::EZ::GetSRV(m_matrices[frameIndex].get());
	pCommandList->SetResources(Shader::Stage::VS, DescriptorType::SRV, 0, 1, &srv);

	static vector<XUSG::EZ::ResourceView> srvs(meshCount);
	for (auto i = 0u; i < meshCount; ++i) srvs[i] = XUSG::EZ::GetSRV(m_meshes[i].VertexBuffer.get());
	pCommandList->SetResources(Shader::Stage::VS, DescriptorType::SRV, 1, meshCount, srvs.data());

	// Set viewport
	const Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	const RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	for (auto i = 0u; i < meshCount; ++i)
	{
		// Set IA
		const auto ibv = XUSG::EZ::GetIBV(m_meshes[i].IndexBuffer.get());
		pCommandList->IASetIndexBuffer(ibv);

		// Set per-object CBVs
		cbvs[0] = XUSG::EZ::GetCBV(m_meshes[i].CbPerObject.get());
		pCommandList->SetResources(Shader::Stage::VS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::CBV, 0, 1, cbvs);

		// Draw command
		pCommandList->DrawIndexed(m_meshes[i].NumIndices, 1, 0, 0, 0);
	}
}

void Renderer::renderVolume(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_SHADE_VOLUME]);
	
	// Set UAV
	const auto uav = XUSG::EZ::GetUAV(m_irradiance.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

	// Set CBV
	const auto cbv = XUSG::EZ::GetCBV(m_cbPerFrame.get(), frameIndex);
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, 1, &cbv);

	// Set SRVs
	{
		const XUSG::EZ::ResourceView srvs[] =
		{
			XUSG::EZ::GetSRV(m_idVolume.get()),
			XUSG::EZ::GetSRV(m_matrices[frameIndex].get()),
			XUSG::EZ::GetSRV(m_globalSDF.get()),
			XUSG::EZ::GetSRV(m_lightSources[frameIndex].get()),
			XUSG::EZ::GetSRV(m_barycVolume.get()),
		};
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);
	}

	static vector<XUSG::EZ::ResourceView> srvs(meshCount);

	// Set index buffers
	for (auto i = 0u; i < meshCount; ++i) srvs[i] = XUSG::EZ::GetSRV(m_meshes[i].IndexBuffer.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, meshCount, srvs.data(), 1);

	// Set vertex buffers
	for (auto i = 0u; i < meshCount; ++i) srvs[i] = XUSG::EZ::GetSRV(m_meshes[i].VertexBuffer.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, meshCount, srvs.data(), 2);

	// Set sampler
	const auto sampler = SamplerPreset::LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::CS, 0, 1, &sampler);

	pCommandList->Dispatch(XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4), XUSG_DIV_UP(GRID_SIZE, 4));
}

void Renderer::render(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_SHADE]);

	// Set UAV
	const auto uav = XUSG::EZ::GetUAV(m_outputView.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

	// Set CBV
	const auto cbv = XUSG::EZ::GetCBV(m_cbPerFrame.get(), frameIndex);
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, 1, &cbv);

	// Set SRVs
	{
		const XUSG::EZ::ResourceView srvs[] =
		{
			XUSG::EZ::GetSRV(m_visibility.get()),
			XUSG::EZ::GetSRV(m_matrices[frameIndex].get()),
			XUSG::EZ::GetSRV(m_globalSDF.get()),
			XUSG::EZ::GetSRV(m_lightSources[frameIndex].get()),
			XUSG::EZ::GetSRV(m_irradiance.get())
		};
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);
	}
	
	static vector<XUSG::EZ::ResourceView> srvs(meshCount);

	// Set index buffers
	for (auto i = 0u; i < meshCount; ++i) srvs[i] = XUSG::EZ::GetSRV(m_meshes[i].IndexBuffer.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, meshCount, srvs.data(), 1);

	// Set vertex buffers
	for (auto i = 0u; i < meshCount; ++i) srvs[i] = XUSG::EZ::GetSRV(m_meshes[i].VertexBuffer.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, meshCount, srvs.data(), 2);

	// Set sampler
	const auto sampler = SamplerPreset::LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::CS, 0, 1, &sampler);

	pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
}

void Renderer::antiAlias(XUSG::EZ::CommandList* pCommandList, RenderTarget* pRenderTarget)
{
	// Set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_SCREEN_QUAD]);
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_FXAA]);
	pCommandList->DSSetState(Graphics::DepthStencilPreset::DEPTH_STENCIL_NONE);
	pCommandList->RSSetState(Graphics::RasterizerPreset::CULL_BACK);
	pCommandList->OMSetBlendState(Graphics::BlendPreset::DEFAULT_OPAQUE);

	// Set render target
	const auto rtv = XUSG::EZ::GetRTV(pRenderTarget);
	pCommandList->OMSetRenderTargets(1, &rtv, nullptr);

	// Set SRVs
	{
		const XUSG::EZ::ResourceView srvs[] =
		{
			XUSG::EZ::GetSRV(m_outputView.get())
		};
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);
	}

	// Set sampler
	const auto sampler = SamplerPreset::LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::PS, 0, 1, &sampler);

	// Set viewport
	const Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	const RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Draw command
	pCommandList->Draw(3, 1, 0, 0);
}

void Renderer::calcMeshWorldAABB(XMVECTOR pAABB[2], uint32_t meshId) const
{
	auto pos = XMLoadFloat4(&m_meshes[meshId].Bound);
	pos = XMVectorSetW(pos, 1.0f);

	const auto radius = XMVectorReplicate(m_meshes[meshId].Bound.w);

	pAABB[0] = pos - radius;
	pAABB[1] = pos + radius;

	const auto world = getWorldMatrix(meshId);
	pAABB[0] = XMVector3TransformCoord(pAABB[0], world);
	pAABB[1] = XMVector3TransformCoord(pAABB[1], world);
}

FXMMATRIX Renderer::getWorldMatrix(uint32_t meshId) const
{
	const auto& posScale = m_meshes[meshId].PosScale;
	const auto scl = XMMatrixScaling(posScale.w, posScale.w, posScale.w);
	const auto tsl = XMMatrixTranslation(posScale.x, posScale.y, posScale.z);

	if (m_meshes[meshId].IsDynamic)
	{
		const auto rot = XMMatrixRotationY(static_cast<float>((m_time - m_timeStart) * 0.2));

		return scl * rot * tsl;
	}

	return scl * tsl;
}
