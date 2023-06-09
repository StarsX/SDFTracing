//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "Helper/XUSGRayTracing-EZ.h"
#include "RayTracing/XUSGRayTracing.h"
#include "Optional/XUSGGltfLoader.h"

class Renderer
{
public:
	struct MeshDesc
	{
		std::string FileName;
		DirectX::XMFLOAT4 PosScale;
		bool IsDynamic;
	};

	Renderer();
	virtual ~Renderer();

	bool Init(XUSG::RayTracing::EZ::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders,
		uint32_t meshCount, XUSG::RayTracing::GeometryBuffer* pGeometries, const MeshDesc* pMeshDescs);
	bool SetViewport(const XUSG::Device* pDevice, uint32_t width, uint32_t height);

	void UpdateFrame(double time, uint8_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj);
	void Render(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex,
		XUSG::RenderTarget* pRenderTarget, XUSG::DepthStencil* pDepthStencil);

	static const uint8_t FrameCount = 3;

protected:
	enum ShaderIndex : uint8_t
	{
		CS_BUILD_SDF,
		CS_UPDATE_SDF,
		CS_SHADE_VOLUME,
		CS_SHADE,

		VS_VISIBILITY,
		VS_SCREEN_QUAD,

		PS_VISIBILITY,
		PS_FXAA,

		NUM_SHADER
	};

	struct MeshResource
	{
		XUSG::ConstantBuffer::uptr CbPerObject;
		XUSG::VertexBuffer::uptr VertexBuffer;
		XUSG::IndexBuffer::uptr IndexBuffer;
		XUSG::RayTracing::BottomLevelAS::uptr BottomLevelAS;

		uint32_t NumIndices;
		uint32_t MeshId;

		DirectX::XMFLOAT4 PosScale;
		DirectX::XMFLOAT3 MinAABB;
		DirectX::XMFLOAT3 MaxAABB;

		std::string Name;
		bool IsDynamic;
	};

	struct DynamicMesh
	{
		uint32_t MeshId;
	};

	bool loadMesh(XUSG::EZ::CommandList* pCommandList, uint32_t meshId,
		std::vector<XUSG::Resource::uptr>& uploaders, const MeshDesc* pMeshDescs,
		std::vector<uint32_t>& dynamicMeshIds, std::vector<XUSG::GltfLoader::LightSource>& lightSources);
	bool createMeshVB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createMeshIB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createMeshCB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createCBs(const XUSG::Device* pDevice);
	bool createShaders();
	bool buildAccelerationStructures(XUSG::RayTracing::EZ::CommandList* pCommandList,
		XUSG::RayTracing::GeometryBuffer* pGeometries);

	void buildSDF(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void updateSDF(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void updateAccelerationStructures(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void visibility(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex, XUSG::DepthStencil* pDepthStencil);
	void renderVolume(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void render(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void antiAlias(XUSG::EZ::CommandList* pCommandList, XUSG::RenderTarget* pRenderTarget);

	void calcMeshWorldAABB(DirectX::XMVECTOR pAABB[2], uint32_t meshId) const;

	DirectX::FXMMATRIX getWorldMatrix(uint32_t meshId) const;

	std::vector<MeshResource> m_meshes;
	std::vector<uint32_t> m_lightSourceMeshIds;
	std::vector<DynamicMesh> m_dynamicMeshes;

	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;
	XUSG::Resource::uptr		m_instances[FrameCount];
	XUSG::Texture3D::uptr		m_globalSDF;
	XUSG::Texture3D::uptr		m_idVolume;
	XUSG::Texture3D::uptr		m_barycVolume;
	XUSG::Texture3D::uptr		m_irradiance;
	XUSG::RenderTarget::uptr	m_visibility;
	XUSG::Texture::uptr			m_outputView;
	XUSG::ConstantBuffer::uptr	m_cbPerFrame;
	XUSG::StructuredBuffer::uptr m_matrices[FrameCount];
	XUSG::StructuredBuffer::uptr m_meshAABBs;
	XUSG::StructuredBuffer::uptr m_lightSources[FrameCount];
	XUSG::StructuredBuffer::uptr m_dynamicMeshList;
	XUSG::StructuredBuffer::uptr m_dynamicMeshIds;

	XUSG::ShaderLib::uptr m_shaderLib;
	XUSG::Blob m_shaders[NUM_SHADER];

	DirectX::XMFLOAT3X4 m_volumeWorld;
	DirectX::XMUINT2 m_viewport;
	uint32_t m_frameIndex;

	double m_timeStart;
	double m_time;
};
