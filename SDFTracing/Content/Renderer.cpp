//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Renderer.h"
#include "SharedConst.h"

using namespace std;
using namespace tiny;
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

struct AABB
{
	DirectX::XMFLOAT3 Min;
	DirectX::XMFLOAT3 Max;
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
	m_textures(1),
	m_frameIndex(0)
{
	m_sceneDesc.Meshes =
	{
		{ "Assets/bunny_uv.gltf", XMFLOAT4(-1.6f, 0.0f, -0.5f, 0.3f), 256, true, true },
		{ "Assets/cornell_box.gltf", XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f), 1024, false, true }
	};
	m_sceneDesc.Name = "CornellBox";
	m_sceneDesc.AmbientBottom = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
	m_sceneDesc.AmbientTop = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);

	m_shaderLib = ShaderLib::MakeUnique();
}

Renderer::~Renderer()
{
}

bool Renderer::Init(RayTracing::EZ::CommandList* pCommandList, vector<Resource::uptr>& uploaders,
	TinyJson& sceneReader, vector<GeometryBuffer>& geometries)
{
	const auto pDevice = pCommandList->GetRTDevice();

	// Load scene
	vector<GltfLoader::LightSource> lightSources(0);
	loadScene(sceneReader, lightSources);

	// create a null texture for place holder
	m_textures[0] = Texture::MakeUnique();
	const uint16_t srvComponentMapping = XUSG_ENCODE_SRV_COMPONENT_MAPPING(SrvCM::FV1, SrvCM::FV1, SrvCM::FV1, SrvCM::FV1);
	XUSG_N_RETURN(m_textures[0]->Create(pCommandList->GetDevice(), 1, 1, Format::R8G8B8A8_UNORM,
		1, ResourceFlag::NONE, 1, 1, false, MemoryFlag::NONE, L"NullTexture", srvComponentMapping), false);

	// Load mesh inputs
	vector<uint32_t> dynamicMeshIds(0);
	m_dynamicMeshes.clear();
	const auto meshDescCount = static_cast<uint32_t>(m_sceneDesc.Meshes.size());
	for (auto i = 0u; i < meshDescCount; ++i)
	{
		const auto startMeshId = static_cast<uint32_t>(m_meshes.size());
		loadMesh(pCommandList, m_sceneDesc.Meshes[i], dynamicMeshIds, uploaders, lightSources);

		for (auto j = 0u; j < m_meshes[startMeshId].MeshRes->NumSubsets; ++j)
		{
			const auto meshId = startMeshId + j;
			auto& mesh = m_meshes[meshId];

			mesh.PrimitiveIdMap = RenderTarget::MakeUnique();
			XUSG_N_RETURN(mesh.PrimitiveIdMap->Create(pDevice, mesh.LightMapWidth, mesh.LightMapHeight,
				Format::R32_UINT, 1, ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE,
				(L"Mesh" + to_wstring(meshId) + L".PrimitiveIdMap").c_str()), false);
		}
	}

	// Create constant buffers
	XUSG_N_RETURN(createCBs(pDevice), false);

	// Create buffers
	// Matrices
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());
	for (auto& matrices : m_matrices)
	{
		matrices = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(matrices->Create(pDevice, meshCount, sizeof(PerObject), ResourceFlag::NONE,
			MemoryType::UPLOAD, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"Matrices"), false);
	}

	// Light sources
	const auto lightSourceCount = static_cast<uint32_t>(lightSources.size());
	assert(lightSourceCount == m_lightSourceMeshIds.size());
	for (auto& lightSourceBuffer : m_lightSources)
	{
		lightSourceBuffer = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(lightSourceBuffer->Create(pDevice, lightSourceCount, sizeof(LightSource), ResourceFlag::NONE,
			MemoryType::UPLOAD, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"LightSources"), false); // MemoryType::UPLOAD CPU

		for (auto i = 0u; i < lightSourceCount; ++i)
		{
			const auto pData = static_cast<GltfLoader::LightSource*>(lightSourceBuffer->Map());
			*pData = lightSources[i];
		}
	}

	// Mesh AABBs
	{
		m_meshAABBs = StructuredBuffer::MakeUnique();
		XUSG_N_RETURN(m_meshAABBs->Create(pDevice, meshCount, sizeof(AABB), ResourceFlag::NONE,
			MemoryType::DEFAULT, 1, nullptr, 0, nullptr, MemoryFlag::NONE, L"MeshAABBs"), false);
		uploaders.emplace_back(Resource::MakeUnique());

		vector<AABB> aabbs(meshCount);
		for (auto i = 0u; i < meshCount; ++i)
		{
			aabbs[i].Min = m_meshes[i].MeshRes->AABBMin;
			aabbs[i].Max = m_meshes[i].MeshRes->AABBMax;
		}

		XUSG_N_RETURN(m_meshAABBs->Upload(pCommandList->AsCommandList(),
			uploaders.back().get(), aabbs.data(), sizeof(AABB) * meshCount), false);
	}

	// For dynamic meshes
	const auto dynamicMeshCount = static_cast<uint32_t>(m_dynamicMeshes.size());
	{
		// Allocate dynamic mesh buffer
		m_dynamicMeshList = StructuredBuffer::MakeUnique(); // create buffer, upload to GPU
		XUSG_N_RETURN(m_dynamicMeshList->Create(pDevice, dynamicMeshCount, sizeof(DynamicMesh),
			ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 0, nullptr, MemoryFlag::NONE,
			L"DynamicMeshes"), false); // create dynamic meshes GPU
		uploaders.emplace_back(Resource::MakeUnique());

		XUSG_N_RETURN(m_dynamicMeshList->Upload(pCommandList->AsCommandList(), uploaders.back().get(),
			m_dynamicMeshes.data(), sizeof(DynamicMesh) * dynamicMeshCount), false);
	}

	{
		m_dynamicMeshIds = StructuredBuffer::MakeUnique(); // create buffer, upload to GPU
		XUSG_N_RETURN(m_dynamicMeshIds->Create(pDevice, meshCount, sizeof(uint32_t),
			ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 0, nullptr, MemoryFlag::NONE,
			L"DynamicMeshIds"), false); // create dynamic meshes GPU
		uploaders.emplace_back(Resource::MakeUnique());

		XUSG_N_RETURN(m_dynamicMeshIds->Upload(pCommandList->AsCommandList(), uploaders.back().get(),
			dynamicMeshIds.data(), sizeof(uint32_t) * meshCount), false);
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
	const auto mipCount = Texture::CalculateMipLevels(gridSize, gridSize, gridSize);
	m_irradiance = Texture3D::MakeUnique();
	XUSG_N_RETURN(m_irradiance->Create(pDevice, gridSize, gridSize, gridSize, Format::R16G16B16A16_FLOAT,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, mipCount, MemoryFlag::NONE, L"IrradianceVolume"), false);

	// Compute scene AABB
	computeSceneAABB();

	// Calculate volume world matrix
	XMFLOAT3 volumeExt;
	const auto sceneAABBMin = XMLoadFloat3(&m_sceneAABBMin);
	const auto sceneAABBMax = XMLoadFloat3(&m_sceneAABBMax);
	const auto volumeCenter = (sceneAABBMin + sceneAABBMax) * 0.5f;
	XMStoreFloat3(&volumeExt, (sceneAABBMax - sceneAABBMin) * 0.5f);
	const auto radius = (max)((max)(volumeExt.x, volumeExt.y), volumeExt.z) * 1.08f;
	const auto volumeWorld = XMMatrixScaling(radius, radius, radius) *
		XMMatrixTranslationFromVector(volumeCenter);
	XMStoreFloat3x4(&m_volumeWorld, volumeWorld);

	// Build acceleration structures
	XUSG_N_RETURN(buildAccelerationStructures(pCommandList, geometries), false);

	const uint32_t maxSrvSpaces[Shader::Stage::NUM_STAGE] = { 4, 2, 0, 0, 0, 3 };
	XUSG_N_RETURN(pCommandList->CreatePipelineLayouts(nullptr, nullptr, nullptr, nullptr, nullptr, maxSrvSpaces), false);

	// Create shaders
	return createShaders();
}

bool Renderer::SetViewport(XUSG::EZ::CommandList* pCommandList, uint32_t width, uint32_t height)
{
	const auto pDevice = pCommandList->GetDevice();
	m_viewport.x = width;
	m_viewport.y = height;

	// Create textures
	m_visibility = RenderTarget::MakeUnique();
	XUSG_N_RETURN(m_visibility->Create(pDevice, width, height, Format::R32_UINT, 1,
		ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"Visibility"), false);

	m_outputView = Texture::MakeUnique();
	XUSG_N_RETURN(m_outputView->Create(pDevice, width, height, Format::R8G8B8A8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"OutputView"), false);

	return createDescriptorTables(pCommandList);
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

bool Renderer::loadMesh(XUSG::EZ::CommandList* pCommandList, const MeshDesc& meshDesc, vector<uint32_t>& dynamicMeshIds,
	vector<Resource::uptr>& uploaders, vector<GltfLoader::LightSource>& lightSources)
{
	GltfLoader loader;
	if (!loader.Import(meshDesc.FileName.c_str(), true, true, true, meshDesc.InvertZ)) return false;

	const auto startMeshId = static_cast<uint32_t>(m_meshes.size());
	const auto numSubSets = loader.GetNumSubSets();
	m_meshes.resize(startMeshId + numSubSets);
	dynamicMeshIds.resize(startMeshId + numSubSets);

	auto meshRes = make_shared<MeshResource>();
	meshRes->PosScale = meshDesc.PosScale;
	meshRes->Rot = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);

	const auto aabb = loader.GetAABB();
	meshRes->AABBMin = XMFLOAT3(aabb.Min.x, aabb.Min.y, aabb.Min.z);
	meshRes->AABBMax = XMFLOAT3(aabb.Max.x, aabb.Max.y, aabb.Max.z);

	meshRes->NumSubsets = numSubSets;
	meshRes->StartMeshId = startMeshId;
	meshRes->IsDynamic = meshDesc.IsDynamic;

	string fileName = meshDesc.FileName;
	for (size_t found = fileName.find('\\'); found != string::npos; found = fileName.find('\\', found + 1))
		fileName.replace(found, 1, "/");
	const auto nameStart = fileName.rfind('/') + 1;
	meshRes->Name = fileName.substr(nameStart, fileName.find(".gltf") - nameStart);

	const auto texIdxOffset = static_cast<uint32_t>(m_textures.size());
	const auto pSubsets = loader.GetSubsets();
	for (auto i = 0u; i < numSubSets; ++i)
	{
		const auto meshId = startMeshId + i;
		XUSG_N_RETURN(createMeshCB(pCommandList, meshId, uploaders), false);
		m_meshes[meshId].MeshRes = meshRes;
		m_meshes[meshId].IndexOffset = pSubsets[i].IndexOffset;
		m_meshes[meshId].NumIndices = pSubsets[i].NumIndices;
		m_meshes[meshId].BaseColorTexIdx = pSubsets[i].BaseColorTexIdx != UINT32_MAX ? texIdxOffset + pSubsets[i].BaseColorTexIdx : 0;
		m_meshes[meshId].NormalTexIdx = pSubsets[i].NormalTexIdx != UINT32_MAX ? texIdxOffset + pSubsets[i].NormalTexIdx : 0;
		m_meshes[meshId].AlphaMode = static_cast<AlphaMode>(pSubsets[i].AlphaMode);

		if (meshDesc.IsDynamic)
		{
			// !!! isDynamic bool variable, set bunny true
			dynamicMeshIds[meshId] = static_cast<uint32_t>(m_dynamicMeshes.size());
			m_dynamicMeshes.push_back({ meshId });
		}
		else dynamicMeshIds[meshId] = UINT32_MAX;

		m_meshes[meshId].LightMapWidth = (min)(static_cast<uint32_t>(meshDesc.LightMapSize * pSubsets[i].LightMapScl.x), 1024u);
		m_meshes[meshId].LightMapHeight = (min)(static_cast<uint32_t>(meshDesc.LightMapSize * pSubsets[i].LightMapScl.y), 1024u);
	}

	XUSG_N_RETURN(createMeshVB(pCommandList, *meshRes, loader.GetNumVertices(), loader.GetVertexStride(), loader.GetVertices(), uploaders), false);
	XUSG_N_RETURN(createMeshIB(pCommandList, *meshRes, loader.GetNumIndices(), loader.GetIndices(), &m_meshes[startMeshId], numSubSets, uploaders), false);
	XUSG_N_RETURN(createMeshTextures(pCommandList, loader.GetTextures(), loader.GetNumTextures(), uploaders), false);

	const auto meshLightSources = loader.GetLightSources();
	lightSources.insert(lightSources.end(), meshLightSources.cbegin(), meshLightSources.cend());
	for (const auto& meshLightSource : meshLightSources) m_lightSourceMeshIds.emplace_back(startMeshId);

	return true;
}

bool Renderer::createMeshVB(XUSG::EZ::CommandList* pCommandList, MeshResource& meshRes, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	meshRes.VertexBuffer = VertexBuffer::MakeUnique();

	XUSG_N_RETURN(meshRes.VertexBuffer->Create(pCommandList->GetDevice(), numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 0), false);
	uploaders.emplace_back(Resource::MakeUnique());//!!!

	return meshRes.VertexBuffer->Upload(pCommandList->AsCommandList(),
		uploaders.back().get(), pData, stride * numVert);
}

bool Renderer::createMeshIB(XUSG::EZ::CommandList* pCommandList, MeshResource& meshRes, uint32_t numIndices,
	const uint32_t* pData, const MeshSubset* pSubsets, uint32_t numSubsets, vector<Resource::uptr>& uploaders)
{
	vector<uintptr_t> offsets(numSubsets);
	vector<uintptr_t> firstElements(numSubsets);
	for (auto i = 0u; i < numSubsets; ++i)
	{
		offsets[i] = sizeof(uint32_t) * pSubsets[i].IndexOffset;
		firstElements[i] = pSubsets[i].IndexOffset;
	}

	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	meshRes.IndexBuffer = IndexBuffer::MakeUnique();

	XUSG_N_RETURN(meshRes.IndexBuffer->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT,
		ResourceFlag::NONE, MemoryType::DEFAULT, numSubsets, offsets.data(),
		numSubsets, firstElements.data(), 0), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return meshRes.IndexBuffer->Upload(pCommandList->AsCommandList(), uploaders.back().get(), pData, byteWidth);
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

bool Renderer::createMeshTextures(XUSG::EZ::CommandList* pCommandList, const GltfLoader::Texture* pTextures,
	uint32_t numTextures, std::vector<XUSG::Resource::uptr>& uploaders)
{
	const auto texIdxOffset = static_cast<uint32_t>(m_textures.size());
	m_textures.resize(texIdxOffset + numTextures);
	for (auto i = 0u; i < numTextures; ++i)
	{
		Format format;
		switch (pTextures[i].Channels)
		{
		case 1:
			format = Format::R8_UNORM;
			break;
		case 2:
			format = Format::R8G8_UNORM;
			break;
		case 4:
			format = Format::R8G8B8A8_UNORM;
			break;
		default:
			assert(!"Wrong channels, unknown format!");
		}

		const auto j = texIdxOffset + i;
		m_textures[j] = Texture::MakeUnique();
		XUSG_N_RETURN(m_textures[j]->Create(pCommandList->GetDevice(), pTextures[i].Width, pTextures[i].Height,
			format, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS, 0, 1, false, MemoryFlag::NONE), false);
		uploaders.emplace_back(Resource::MakeUnique());

		XUSG_N_RETURN(m_textures[j]->Upload(pCommandList->AsCommandList(), uploaders.back().get(),
			pTextures[i].Data.data(), pTextures[i].Channels), false);
	}

	return true;
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

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSVisibilityA.cso"), false);
	m_shaders[PS_VISIBILITY_A] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSFXAA.cso"), false);
	m_shaders[PS_FXAA] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	return true;
}

bool Renderer::createDescriptorTables(XUSG::EZ::CommandList* pCommandList)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());
	const auto pDescriptorTableLib = pCommandList->GetDescriptorTableLib();
	vector<Descriptor> descriptors(meshCount);

	// Index buffer SRVs
	{
		for (auto i = 0u; i < meshCount; ++i) descriptors[i] = m_meshes[i].MeshRes->IndexBuffer->GetSRV(i - m_meshes[i].MeshRes->StartMeshId);
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(pDescriptorTableLib), false);
	}

	// Vertex buffer SRVs
	{
		for (auto i = 0u; i < meshCount; ++i) descriptors[i] = m_meshes[i].MeshRes->VertexBuffer->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(pDescriptorTableLib), false);
	}

	// Base-color texture SRVs
	{
		for (auto i = 0u; i < meshCount; ++i) descriptors[i] = m_textures[m_meshes[i].BaseColorTexIdx]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_BC], descriptorTable->GetCbvSrvUavTable(pDescriptorTableLib), false);
	}

	// Normal texture SRVs
	{
		for (auto i = 0u; i < meshCount; ++i) descriptors[i] = m_textures[m_meshes[i].NormalTexIdx]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_NM], descriptorTable->GetCbvSrvUavTable(pDescriptorTableLib), false);
	}

	return true;
}

bool Renderer::buildAccelerationStructures(RayTracing::EZ::CommandList* pCommandList, vector<GeometryBuffer>& geometries)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());
	const auto asCount = 1 + meshCount;

	vector<uintptr_t> dstBufferFirstElements(asCount);
	vector<uintptr_t> dstBufferOffsets(asCount);
	size_t dstBufferSize = 0;

	// Prebuild and allocate TLAS
	{
		m_topLevelAS = TopLevelAS::MakeUnique();
		XUSG_N_RETURN(pCommandList->PrebuildTLAS(m_topLevelAS.get(), meshCount, BuildFlag::ALLOW_UPDATE | BuildFlag::PREFER_FAST_TRACE), false);
		dstBufferFirstElements[0] = dstBufferSize / sizeof(uint32_t);
		dstBufferOffsets[0] = dstBufferSize;
		dstBufferSize += m_topLevelAS->GetResultDataMaxByteSize();
	}

	geometries.resize(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		auto& mesh = m_meshes[i];
		const auto pMeshRes = mesh.MeshRes.get();

		// Set geometry
		auto vbv = XUSG::EZ::GetVBV(pMeshRes->VertexBuffer.get());
		auto ibv = XUSG::EZ::GetIBV(pMeshRes->IndexBuffer.get(), i - pMeshRes->StartMeshId);
		const auto geometryFlag = mesh.AlphaMode == ALPHA_OPAQUE ? GeometryFlag::FULL_OPAQUE : GeometryFlag::NONE;
		pCommandList->SetTriangleGeometries(geometries[i], 1, Format::R32G32B32_FLOAT, &vbv, &ibv, &geometryFlag);

		// Prebuild and allocate BLAS
		mesh.BottomLevelAS = BottomLevelAS::MakeUnique();
		XUSG_N_RETURN(pCommandList->PrebuildBLAS(mesh.BottomLevelAS.get(), 1, geometries[i]), false);
		const auto j = i + 1;
		dstBufferFirstElements[j] = dstBufferSize / sizeof(uint32_t);
		dstBufferOffsets[j] = dstBufferSize;
		dstBufferSize += mesh.BottomLevelAS->GetResultDataMaxByteSize();
	}

	// Create buffer storage
	Buffer::sptr dstBuffer = Buffer::MakeShared();
	XUSG_N_RETURN(AccelerationStructure::AllocateDestBuffer(pCommandList->GetRTDevice(), dstBuffer.get(),
		dstBufferSize, 1, dstBufferFirstElements.data(), asCount, dstBufferFirstElements.data()), false);

	// Set TLAS destination
	pCommandList->SetTLASDestination(m_topLevelAS.get(), dstBuffer, dstBufferOffsets[0], 0, 0);

	// Set BLAS destinations and instances for TLAS
	vector<XMFLOAT3X4> matrices(meshCount);
	vector<float*> pTransforms(meshCount);
	vector<const BottomLevelAS*> pBottomLevelASes(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		const auto j = i + 1;
		pCommandList->SetBLASDestination(m_meshes[i].BottomLevelAS.get(), dstBuffer, dstBufferOffsets[j], j);

		XMStoreFloat3x4(&matrices[i], getWorldMatrix(i));
		pTransforms[i] = reinterpret_cast<float*>(&matrices[i]);
		pBottomLevelASes[i] = m_meshes[i].BottomLevelAS.get();
	}
	for (auto& instances : m_instances) instances = Buffer::MakeUnique();
	TopLevelAS::SetInstances(pCommandList->GetRTDevice(), m_instances->get(), meshCount, pBottomLevelASes.data(), pTransforms.data());

	// Build BLASes
	for (auto i = 0u; i < meshCount; ++i) pCommandList->BuildBLAS(m_meshes[i].BottomLevelAS.get());

	// Build TLAS
	pCommandList->BuildTLAS(m_topLevelAS.get(), m_instances->get());

	return true;
}

void Renderer::loadScene(TinyJson& sceneReader, vector<GltfLoader::LightSource>& lightSources)
{
	float vecData[4];
	m_sceneDesc.Name = sceneReader.Get<string>("Name");
	auto meshes = sceneReader.Get<xarray>("Meshes");
	const auto meshCount = static_cast<uint32_t>(meshes.Count());
	m_sceneDesc.Meshes.resize(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		if (meshes.Enter(i))
		{
			m_sceneDesc.Meshes[i].FileName = meshes.Get<string>("FileName");

			auto pos = meshes.Get<xarray>("Position");
			assert(pos.Count() == 3);
			for (uint8_t j = 0; j < 3; ++j) if (pos.Enter(j)) vecData[j] = pos.Get<float>();
			m_sceneDesc.Meshes[i].PosScale.x = vecData[0];
			m_sceneDesc.Meshes[i].PosScale.y = vecData[1];
			m_sceneDesc.Meshes[i].PosScale.z = vecData[2];
			m_sceneDesc.Meshes[i].PosScale.w = meshes.Get<float>("Scaling");

			m_sceneDesc.Meshes[i].LightMapSize = meshes.Get<uint32_t>("LightMapSize");
			m_sceneDesc.Meshes[i].IsDynamic = meshes.Get<bool>("IsDynamic");
			m_sceneDesc.Meshes[i].InvertZ = meshes.Get<bool>("InvertZ");
		}
	}

	auto ambientBottom = sceneReader.Get<xarray>("AmbientBottom");
	if (ambientBottom.Count() == 3)
	{
		for (uint8_t i = 0; i < 3; ++i) if (ambientBottom.Enter(i)) vecData[i] = ambientBottom.Get<float>();
		m_sceneDesc.AmbientBottom.x = vecData[0];
		m_sceneDesc.AmbientBottom.y = vecData[1];
		m_sceneDesc.AmbientBottom.z = vecData[2];
		m_sceneDesc.AmbientBottom.w = 1.0f;
	}

	auto ambientTop = sceneReader.Get<xarray>("AmbientTop");
	if (ambientBottom.Count() == 3)
	{
		for (uint8_t i = 0; i < 3; ++i) if (ambientTop.Enter(i)) vecData[i] = ambientTop.Get<float>();
		m_sceneDesc.AmbientTop.x = vecData[0];
		m_sceneDesc.AmbientTop.y = vecData[1];
		m_sceneDesc.AmbientTop.z = vecData[2];
		m_sceneDesc.AmbientTop.w = 1.0f;
	}

	auto lightSrcs = sceneReader.Get<xarray>("LightSources");
	const auto lightSrcCount = static_cast<uint32_t>(lightSrcs.Count());
	for (auto i = 0u; i < lightSrcCount; ++i)
	{
		if (lightSrcs.Enter(i))
		{
			lightSources.emplace_back();
			auto& lightSource = lightSources.back();

			auto aabbMin = lightSrcs.Get<xarray>("AABBMin");
			assert(aabbMin.Count() == 3);
			for (uint8_t j = 0; j < 3; ++j) if (aabbMin.Enter(j)) vecData[j] = aabbMin.Get<float>();
			lightSource.Min.x = vecData[0];
			lightSource.Min.y = vecData[1];
			lightSource.Min.z = vecData[2];
			lightSource.Min.w = lightSrcs.Get<float>("AABBScaling");

			auto aabbMax = lightSrcs.Get<xarray>("AABBMax");
			assert(aabbMax.Count() == 3);
			for (uint8_t j = 0; j < 3; ++j) if (aabbMax.Enter(j)) vecData[j] = aabbMax.Get<float>();
			lightSource.Max.x = vecData[0];
			lightSource.Max.y = vecData[1];
			lightSource.Max.z = vecData[2];
			lightSource.Max.w = 1.0f;

			auto emissive = lightSrcs.Get<xarray>("Emissive");
			assert(emissive.Count() == 4);
			for (uint8_t j = 0; j < 4; ++j) if (emissive.Enter(j)) vecData[j] = emissive.Get<float>();
			lightSource.Emissive.x = vecData[0];
			lightSource.Emissive.y = vecData[1];
			lightSource.Emissive.z = vecData[2];
			lightSource.Emissive.w = vecData[3];

			m_lightSourceMeshIds.emplace_back(UINT32_MAX);
		}
	}
}

void Renderer::computeSceneAABB()
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	auto sceneMin = XMVectorReplicate(FLT_MAX);
	auto sceneMax = XMVectorReplicate(-FLT_MAX);

	for (auto i = 0u; i < meshCount; ++i)
	{
		const auto pMeshRes = m_meshes[i].MeshRes.get();

		if (pMeshRes->IsDynamic) continue;
		else if (i != pMeshRes->StartMeshId) continue;

		XMVECTOR aabbVertices[8];
		aabbVertices[0] = XMVectorSet(pMeshRes->AABBMin.x, pMeshRes->AABBMin.y, pMeshRes->AABBMin.z, 1.0f);
		aabbVertices[1] = XMVectorSet(pMeshRes->AABBMin.x, pMeshRes->AABBMin.y, pMeshRes->AABBMax.z, 1.0f);
		aabbVertices[2] = XMVectorSet(pMeshRes->AABBMin.x, pMeshRes->AABBMax.y, pMeshRes->AABBMin.z, 1.0f);
		aabbVertices[3] = XMVectorSet(pMeshRes->AABBMin.x, pMeshRes->AABBMax.y, pMeshRes->AABBMax.z, 1.0f);
		aabbVertices[4] = XMVectorSet(pMeshRes->AABBMax.x, pMeshRes->AABBMin.y, pMeshRes->AABBMin.z, 1.0f);
		aabbVertices[5] = XMVectorSet(pMeshRes->AABBMax.x, pMeshRes->AABBMin.y, pMeshRes->AABBMax.z, 1.0f);
		aabbVertices[6] = XMVectorSet(pMeshRes->AABBMax.x, pMeshRes->AABBMax.y, pMeshRes->AABBMin.z, 1.0f);
		aabbVertices[7] = XMVectorSet(pMeshRes->AABBMax.x, pMeshRes->AABBMax.y, pMeshRes->AABBMax.z, 1.0f);

		const auto world = getWorldMatrix(i);
		for (auto& vertex : aabbVertices)
		{
			vertex = XMVector3Transform(vertex, world);
			sceneMin = XMVectorMin(vertex, sceneMin);
			sceneMax = XMVectorMax(vertex, sceneMax);
		}
	}

	XMStoreFloat3(&m_sceneAABBMin, sceneMin);
	XMStoreFloat3(&m_sceneAABBMax, sceneMax);
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
		XUSG::EZ::GetSRV(m_meshAABBs.get())
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
	static vector<const BottomLevelAS*> pBottomLevelASes(meshCount);
	for (auto i = 0u; i < meshCount; ++i)
	{
		XMStoreFloat3x4(&matrices[i], getWorldMatrix(i));
		pTransforms[i] = reinterpret_cast<float*>(&matrices[i]);
		pBottomLevelASes[i] = m_meshes[i].BottomLevelAS.get();
	}
	TopLevelAS::SetInstances(pCommandList->GetRTDevice(), m_instances[frameIndex].get(), meshCount, pBottomLevelASes.data(), pTransforms.data());

	// Update top level AS
	pCommandList->BuildTLAS(m_topLevelAS.get(), m_instances[frameIndex].get(), m_topLevelAS.get());
}

void Renderer::visibility(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex, DepthStencil* pDepthStencil)
{
	const auto meshCount = static_cast<uint32_t>(m_meshes.size());

	// set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_VISIBILITY]);
	pCommandList->DSSetState(Graphics::DepthStencilPreset::DEFAULT_LESS);
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

	// Set vertex buffers
	pCommandList->SetGraphicsDescriptorTable(Shader::Stage::VS, DescriptorType::SRV, m_srvTables[SRV_TABLE_VB], 1);

	// Set base-color textures for alpha test
	pCommandList->SetGraphicsDescriptorTable(Shader::Stage::PS, DescriptorType::SRV, m_srvTables[SRV_TABLE_BC], 0);

	// Set sampler
	const auto sampler = SamplerPreset::ANISOTROPIC_WRAP;
	pCommandList->SetSamplerStates(Shader::Stage::PS, 0, 1, &sampler);

	// Set viewport
	const Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	const RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	for (auto i = 0u; i < meshCount; ++i)
	{
		const auto& mesh = m_meshes[i];
		const auto pMeshRes = mesh.MeshRes.get();

		if (mesh.AlphaMode != ALPHA_OPAQUE)
		{
			pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_VISIBILITY_A]);
			pCommandList->RSSetState(Graphics::RasterizerPreset::CULL_NONE);
		}
		else
		{
			pCommandList->RSSetState(Graphics::RasterizerPreset::CULL_BACK);
			pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_VISIBILITY]);
		}

		// Set IA
		const auto ibv = XUSG::EZ::GetIBV(pMeshRes->IndexBuffer.get(), i - pMeshRes->StartMeshId);
		pCommandList->IASetIndexBuffer(ibv);

		// Set per-object CBVs
		cbvs[0] = XUSG::EZ::GetCBV(mesh.CbPerObject.get());
		pCommandList->SetResources(Shader::Stage::VS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::CBV, 0, 1, cbvs);

		// Draw command
		pCommandList->DrawIndexed(mesh.NumIndices, 1, 0, 0, 0);
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

	// Set index buffers and vertex buffers
	pCommandList->SetComputeDescriptorTable(DescriptorType::SRV, m_srvTables[SRV_TABLE_IB], 1);
	pCommandList->SetComputeDescriptorTable(DescriptorType::SRV, m_srvTables[SRV_TABLE_VB], 2);

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

	// Set index buffers and vertex buffers
	pCommandList->SetComputeDescriptorTable(DescriptorType::SRV, m_srvTables[SRV_TABLE_IB], 1);
	pCommandList->SetComputeDescriptorTable(DescriptorType::SRV, m_srvTables[SRV_TABLE_VB], 2);

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

FXMMATRIX Renderer::getWorldMatrix(uint32_t meshId) const
{
	if (meshId == UINT32_MAX) return XMMatrixIdentity();

	const auto& pMeshRes = m_meshes[meshId].MeshRes.get();
	const auto& posScale = pMeshRes->PosScale;
	const auto scl = XMMatrixScaling(posScale.w, posScale.w, posScale.w);
	const auto tsl = XMMatrixTranslation(posScale.x, posScale.y, posScale.z);

	XMVECTOR q;
	if (pMeshRes->IsDynamic)
	{
		const auto angle = static_cast<float>((m_time - m_timeStart) * 0.5) + meshId;
		q = XMQuaternionRotationAxis(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), angle);
		XMStoreFloat4(&pMeshRes->Rot, q);
	}
	else q = XMLoadFloat4(&pMeshRes->Rot);

	const auto rot = XMMatrixRotationQuaternion(q);

	return scl * rot * tsl;
}
