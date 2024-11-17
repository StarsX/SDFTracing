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
		uint32_t LightMapSize;
		bool IsDynamic;
		bool InvertZ;
	};

	struct SceneDesc
	{
		std::vector<MeshDesc> Meshes;
		std::string Name;
		DirectX::XMFLOAT4 AmbientBottom;
		DirectX::XMFLOAT4 AmbientTop;
	};

	Renderer();
	virtual ~Renderer();

	bool Init(XUSG::RayTracing::EZ::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders,
		tiny::TinyJson& sceneReader, std::vector<XUSG::RayTracing::GeometryBuffer>& geometries);
	bool SetViewport(XUSG::EZ::CommandList* pCommandList, uint32_t width, uint32_t height);

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
		PS_VISIBILITY_A,
		PS_FXAA,

		NUM_SHADER
	};

	enum AlphaMode : uint8_t
	{
		ALPHA_OPAQUE,
		ALPHA_MASK,
		ALPHA_BLEND
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_BC, // Base-color textures
		SRV_TABLE_NM, // Normal textures

		NUM_SRV_TABLE
	};

	struct MeshResource
	{
		XUSG::VertexBuffer::uptr VertexBuffer;
		XUSG::IndexBuffer::uptr IndexBuffer;

		DirectX::XMFLOAT4 PosScale;
		DirectX::XMFLOAT4 Rot;
		DirectX::XMFLOAT3 AABBMin;
		DirectX::XMFLOAT3 AABBMax;

		uint32_t NumSubsets;
		uint32_t StartMeshId;

		std::string Name;

		bool IsDynamic;
	};

	struct MeshSubset
	{
		std::shared_ptr<MeshResource> MeshRes;
		XUSG::ConstantBuffer::uptr CbPerObject;
		XUSG::RenderTarget::uptr PrimitiveIdMap;
		//XUSG::Texture::uptr TileTexelMap;
		//XUSG::Texture::uptr BoundaryWeldMap;
		//XUSG::Texture::uptr LightMaps[2];
		//XUSG::Texture::uptr HistoryMeta;
		/*XUSG::Texture::uptr TileMap;
		XUSG::Texture::uptr TileGeoMap;
		XUSG::Texture::uptr TileBitMapGT;
		XUSG::Texture::uptr TileRaysGT;
		XUSG::Texture::uptr TileRayDirsGT;
		XUSG::Texture::uptr TileImpactsGT;
		XUSG::Buffer::uptr LightMapCache;*/
		XUSG::RayTracing::BottomLevelAS::uptr BottomLevelAS;

		uint32_t IndexOffset;
		uint32_t NumIndices;
		//uint32_t MeshId;
		uint32_t BaseColorTexIdx;
		uint32_t NormalTexIdx;
		uint32_t LightMapWidth;
		uint32_t LightMapHeight;
		uint32_t LightMapRowPitch;
		AlphaMode AlphaMode;
	};

	struct DynamicMesh
	{
		uint32_t MeshId;
	};

	bool loadMesh(XUSG::EZ::CommandList* pCommandList, const MeshDesc& meshDesc,
		std::vector<uint32_t>& dynamicMeshIds, std::vector<XUSG::Resource::uptr>& uploaders,
		std::vector<XUSG::GltfLoader::LightSource>& lightSources);
	bool createMeshVB(XUSG::EZ::CommandList* pCommandList, MeshResource& meshRes, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createMeshIB(XUSG::EZ::CommandList* pCommandList, MeshResource& meshRes, uint32_t numIndices, const uint32_t* pData,
		const MeshSubset* pSubsets, uint32_t numSubsets, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createMeshCB(XUSG::EZ::CommandList* pCommandList, uint32_t meshId, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createMeshTextures(XUSG::EZ::CommandList* pCommandList, const XUSG::GltfLoader::Texture* pTextures,
		uint32_t numTextures, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createCBs(const XUSG::Device* pDevice);
	bool createShaders();
	bool createDescriptorTables(XUSG::EZ::CommandList* pCommandList);
	bool buildAccelerationStructures(XUSG::RayTracing::EZ::CommandList* pCommandList,
		std::vector<XUSG::RayTracing::GeometryBuffer>& geometries);

	void loadScene(tiny::TinyJson& sceneReader, std::vector<XUSG::GltfLoader::LightSource>& lightSources);
	void computeSceneAABB();
	void buildSDF(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void updateSDF(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void updateAccelerationStructures(XUSG::RayTracing::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void visibility(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex, XUSG::DepthStencil* pDepthStencil);
	void renderVolume(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void render(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void antiAlias(XUSG::EZ::CommandList* pCommandList, XUSG::RenderTarget* pRenderTarget);

	DirectX::FXMMATRIX getWorldMatrix(uint32_t meshId) const;

	SceneDesc m_sceneDesc;
	std::vector<MeshSubset> m_meshes;
	std::vector<uint32_t> m_lightSourceMeshIds;
	std::vector<DynamicMesh> m_dynamicMeshes;

	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;
	XUSG::Buffer::uptr			m_instances[FrameCount];
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

	std::vector<XUSG::Texture::uptr> m_textures;

	XUSG::ShaderLib::uptr m_shaderLib;
	XUSG::Blob m_shaders[NUM_SHADER];

	XUSG::DescriptorTable m_srvTables[NUM_SRV_TABLE];

	DirectX::XMFLOAT3 m_sceneAABBMin;
	DirectX::XMFLOAT3 m_sceneAABBMax;

	DirectX::XMFLOAT3X4 m_volumeWorld;
	DirectX::XMUINT2 m_viewport;
	uint32_t m_frameIndex;

	double m_timeStart;
	double m_time;
};
