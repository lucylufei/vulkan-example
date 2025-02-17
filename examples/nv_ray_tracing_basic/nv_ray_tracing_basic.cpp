/*
* Vulkan Example - Basic example for ray tracing using VK_NV_ray_tracing
*
* Copyright (C) by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"
#include "VulkanDevice.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanModel.hpp"

// Ray tracing acceleration structure
struct AccelerationStructure {
	VkDeviceMemory memory;
	VkAccelerationStructureNV accelerationStructure;
	uint64_t handle;
};

// Ray tracing geometry instance
struct GeometryInstance {
	glm::mat3x4 transform;
	uint32_t instanceId : 24;
	uint32_t mask : 8;
	uint32_t instanceOffset : 24;
	uint32_t flags : 8;
	uint64_t accelerationStructureHandle;
};

// Indices for the different ray tracing shader types used in this example
#define INDEX_RAYGEN 0
#define INDEX_MISS 1
#define INDEX_CLOSEST_HIT 2

#define NUM_SHADER_GROUPS 3

class VulkanExample : public VulkanExampleBase
{
public:
	PFN_vkCreateAccelerationStructureNV vkCreateAccelerationStructureNV;
	PFN_vkDestroyAccelerationStructureNV vkDestroyAccelerationStructureNV;
	PFN_vkBindAccelerationStructureMemoryNV vkBindAccelerationStructureMemoryNV;
	PFN_vkGetAccelerationStructureHandleNV vkGetAccelerationStructureHandleNV;
	PFN_vkGetAccelerationStructureMemoryRequirementsNV vkGetAccelerationStructureMemoryRequirementsNV;
	PFN_vkCmdBuildAccelerationStructureNV vkCmdBuildAccelerationStructureNV;
	PFN_vkCreateRayTracingPipelinesNV vkCreateRayTracingPipelinesNV;
	PFN_vkGetRayTracingShaderGroupHandlesNV vkGetRayTracingShaderGroupHandlesNV;
	PFN_vkCmdTraceRaysNV vkCmdTraceRaysNV;

	VkPhysicalDeviceRayTracingPropertiesNV rayTracingProperties{};

	AccelerationStructure bottomLevelAS;
	AccelerationStructure topLevelAS;

	vks::Buffer shaderBindingTable;

	struct StorageImage {
		VkDeviceMemory memory;
		VkImage image;
		VkImageView view;
		VkFormat format;
	} storageImage;

	struct UniformData {
		glm::mat4 viewInverse;
		glm::mat4 projInverse;
	} uniformData;
	vks::Buffer ubo;

	VkPipeline pipeline;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSet descriptorSet;
	VkDescriptorSetLayout descriptorSetLayout;
	
	vks::VertexLayout vertexLayout = vks::VertexLayout({
		vks::VERTEX_COMPONENT_POSITION,
		vks::VERTEX_COMPONENT_NORMAL,
		vks::VERTEX_COMPONENT_COLOR,
		vks::VERTEX_COMPONENT_UV,
		vks::VERTEX_COMPONENT_DUMMY_FLOAT
	});
	vks::Model scene;


	VulkanExample() : VulkanExampleBase()
	{
		title = "VK_NV_ray_tracing";
		settings.overlay = true;
		camera.type = Camera::CameraType::lookat;
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 512.0f);
		camera.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
		camera.setTranslation(glm::vec3(0.0f, 0.0f, -2.5f));
		// Enable instance and device extensions required to use VK_NV_ray_tracing
		enabledInstanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
		enabledDeviceExtensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
		enabledDeviceExtensions.push_back(VK_NV_RAY_TRACING_EXTENSION_NAME);
	}

	~VulkanExample()
	{
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		vkDestroyImageView(device, storageImage.view, nullptr);
		vkDestroyImage(device, storageImage.image, nullptr);
		vkFreeMemory(device, storageImage.memory, nullptr);
		vkFreeMemory(device, bottomLevelAS.memory, nullptr);
		vkFreeMemory(device, topLevelAS.memory, nullptr);
		vkDestroyAccelerationStructureNV(device, bottomLevelAS.accelerationStructure, nullptr);
		vkDestroyAccelerationStructureNV(device, topLevelAS.accelerationStructure, nullptr);
		shaderBindingTable.destroy();
		ubo.destroy();
		scene.destroy();
	}

	/*
		Submit command buffer to a queue and wait for fence until queue operations have been finished
	*/
	void submitWork(VkCommandBuffer cmdBuffer, VkQueue queue)
	{
		VkSubmitInfo submitInfo = vks::initializers::submitInfo();
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmdBuffer;
		VkFenceCreateInfo fenceInfo = vks::initializers::fenceCreateInfo();
		VkFence fence;
		VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
		vkDestroyFence(device, fence, nullptr);
	}
	/*
		Set up a storage image that the ray generation shader will be writing to
	*/
	void createStorageImage()
	{
		VkImageCreateInfo image = vks::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = VK_FORMAT_B8G8R8A8_UNORM;
		image.extent.width = width;
		image.extent.height = height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &storageImage.image));

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, storageImage.image, &memReqs);
		VkMemoryAllocateInfo memoryAllocateInfo = vks::initializers::memoryAllocateInfo();
		memoryAllocateInfo.allocationSize = memReqs.size;
		memoryAllocateInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &storageImage.memory));
		VK_CHECK_RESULT(vkBindImageMemory(device, storageImage.image, storageImage.memory, 0));

		VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
		colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		colorImageView.format = VK_FORMAT_B8G8R8A8_UNORM;
		colorImageView.subresourceRange = {};
		colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		colorImageView.subresourceRange.baseMipLevel = 0;
		colorImageView.subresourceRange.levelCount = 1;
		colorImageView.subresourceRange.baseArrayLayer = 0;
		colorImageView.subresourceRange.layerCount = 1;
		colorImageView.image = storageImage.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &storageImage.view));

		VkCommandBuffer cmdBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		vks::tools::setImageLayout(cmdBuffer, storageImage.image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_GENERAL,
			{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
		vulkanDevice->flushCommandBuffer(cmdBuffer, queue);
	}

	/*
		The bottom level acceleration structure contains the scene's geometry (vertices, triangles)
	*/
	void createBottomLevelAccelerationStructure(const VkGeometryNV* geometries)
	{
		std::cout << "Creating bottom level acceleration struction." << std::endl;
		VkAccelerationStructureInfoNV accelerationStructureInfo{};
		accelerationStructureInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
		accelerationStructureInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
		accelerationStructureInfo.instanceCount = 0;
		accelerationStructureInfo.geometryCount = 1;
		accelerationStructureInfo.pGeometries = geometries;

		VkAccelerationStructureCreateInfoNV accelerationStructureCI{};
		accelerationStructureCI.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
		accelerationStructureCI.info = accelerationStructureInfo;
		VK_CHECK_RESULT(vkCreateAccelerationStructureNV(device, &accelerationStructureCI, nullptr, &bottomLevelAS.accelerationStructure));

		VkAccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo{};
		memoryRequirementsInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
		memoryRequirementsInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV;
		memoryRequirementsInfo.accelerationStructure = bottomLevelAS.accelerationStructure;

		VkMemoryRequirements2 memoryRequirements2{};
		vkGetAccelerationStructureMemoryRequirementsNV(device, &memoryRequirementsInfo, &memoryRequirements2);

		VkMemoryAllocateInfo memoryAllocateInfo = vks::initializers::memoryAllocateInfo();
		memoryAllocateInfo.allocationSize = memoryRequirements2.memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memoryRequirements2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &bottomLevelAS.memory));

		VkBindAccelerationStructureMemoryInfoNV accelerationStructureMemoryInfo{};
		accelerationStructureMemoryInfo.sType = VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
		accelerationStructureMemoryInfo.accelerationStructure = bottomLevelAS.accelerationStructure;
		accelerationStructureMemoryInfo.memory = bottomLevelAS.memory;
		VK_CHECK_RESULT(vkBindAccelerationStructureMemoryNV(device, 1, &accelerationStructureMemoryInfo));

		VK_CHECK_RESULT(vkGetAccelerationStructureHandleNV(device, bottomLevelAS.accelerationStructure, sizeof(uint64_t), &bottomLevelAS.handle));
	}

	/*
		The top level acceleration structure contains the scene's object instances
	*/
	void createTopLevelAccelerationStructure()
	{
		std::cout << "Creating top level acceleration struction." << std::endl;
		VkAccelerationStructureInfoNV accelerationStructureInfo{};
		accelerationStructureInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
		accelerationStructureInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
		accelerationStructureInfo.instanceCount = 1;
		accelerationStructureInfo.geometryCount = 0;

		VkAccelerationStructureCreateInfoNV accelerationStructureCI{};
		accelerationStructureCI.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
		accelerationStructureCI.info = accelerationStructureInfo;
		VK_CHECK_RESULT(vkCreateAccelerationStructureNV(device, &accelerationStructureCI, nullptr, &topLevelAS.accelerationStructure));

		VkAccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo{};
		memoryRequirementsInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
		memoryRequirementsInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV;
		memoryRequirementsInfo.accelerationStructure = topLevelAS.accelerationStructure;

		VkMemoryRequirements2 memoryRequirements2{};
		vkGetAccelerationStructureMemoryRequirementsNV(device, &memoryRequirementsInfo, &memoryRequirements2);

		VkMemoryAllocateInfo memoryAllocateInfo = vks::initializers::memoryAllocateInfo();
		memoryAllocateInfo.allocationSize = memoryRequirements2.memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memoryRequirements2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &topLevelAS.memory));

		VkBindAccelerationStructureMemoryInfoNV accelerationStructureMemoryInfo{};
		accelerationStructureMemoryInfo.sType = VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
		accelerationStructureMemoryInfo.accelerationStructure = topLevelAS.accelerationStructure;
		accelerationStructureMemoryInfo.memory = topLevelAS.memory;
		VK_CHECK_RESULT(vkBindAccelerationStructureMemoryNV(device, 1, &accelerationStructureMemoryInfo));

		VK_CHECK_RESULT(vkGetAccelerationStructureHandleNV(device, topLevelAS.accelerationStructure, sizeof(uint64_t), &topLevelAS.handle));
	}

	/*
		Create scene geometry and ray tracing acceleration structures
	*/
	void createScene()
	{
		#ifdef TRIANGLE
		// Setup vertices for a single triangle
		struct Vertex {
			float pos[3];
		};
		std::vector<Vertex> vertices = {
			{ {  1.0f,  1.0f, 0.0f } },
			{ { -1.0f,  1.0f, 0.0f } },
			{ {  0.0f, -1.0f, 0.0f } }
		};

		// Setup indices
		std::vector<uint32_t> indices = { 0, 1, 2 };
		indexCount = static_cast<uint32_t>(indices.size());

		// Create buffers
		// For the sake of simplicity we won't stage the vertex data to the gpu memory
		// Vertex buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&vertexBuffer,
			vertices.size() * sizeof(Vertex),
			vertices.data()));
		// Index buffer
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&indexBuffer,
			indices.size() * sizeof(uint32_t),
			indices.data()));

		/*
			Create the bottom level acceleration structure containing the actual scene geometry
		*/
		VkGeometryNV geometry{};
		geometry.sType = VK_STRUCTURE_TYPE_GEOMETRY_NV;
		geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_NV;
		geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV;
		geometry.geometry.triangles.vertexData = vertexBuffer.buffer;
		geometry.geometry.triangles.vertexOffset = 0;
		geometry.geometry.triangles.vertexCount = static_cast<uint32_t>(vertices.size());
		geometry.geometry.triangles.vertexStride = sizeof(Vertex);
		geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geometry.geometry.triangles.indexData = indexBuffer.buffer;
		geometry.geometry.triangles.indexOffset = 0;
		geometry.geometry.triangles.indexCount = indexCount;
		geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		geometry.geometry.triangles.transformData = VK_NULL_HANDLE;
		geometry.geometry.triangles.transformOffset = 0;
		geometry.geometry.aabbs = {};
		geometry.geometry.aabbs.sType = { VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV };
		geometry.flags = VK_GEOMETRY_OPAQUE_BIT_NV;

		#else
		// Instead of a simple triangle, we'll be loading a more complex scene for this example
		vks::ModelCreateInfo modelCI{};
		modelCI.scale = glm::vec3(0.25f);
		// The shaders are accessing the vertex and index buffers of the scene, so the proper usage flag has to be set
		modelCI.memoryPropertyFlags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		
		std::string model_file_path = "models/" + model_name + ".dae";
		scene.loadFromFile(getAssetPath() + model_file_path, vertexLayout, &modelCI, vulkanDevice, queue);
		std::cout << "Creating scene for " << model_name << std::endl;

		/*
			Create the bottom level acceleration structure containing the actual scene geometry
		*/
		VkGeometryNV geometry{};
		geometry.sType = VK_STRUCTURE_TYPE_GEOMETRY_NV;
		geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_NV;
		geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV;
		geometry.geometry.triangles.vertexData = scene.vertices.buffer;
		geometry.geometry.triangles.vertexOffset = 0;
		geometry.geometry.triangles.vertexCount = static_cast<uint32_t>(scene.vertexCount);
		geometry.geometry.triangles.vertexStride = vertexLayout.stride();
		geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geometry.geometry.triangles.indexData = scene.indices.buffer;
		geometry.geometry.triangles.indexOffset = 0;
		geometry.geometry.triangles.indexCount = scene.indexCount;
		geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		geometry.geometry.triangles.transformData = VK_NULL_HANDLE;
		geometry.geometry.triangles.transformOffset = 0;
		geometry.geometry.aabbs = {};
		geometry.geometry.aabbs.sType = { VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV };
		geometry.flags = VK_GEOMETRY_OPAQUE_BIT_NV;

		std::cout << "Vertex count: " << scene.vertexCount << std::endl;
		#endif
		createBottomLevelAccelerationStructure(&geometry);

		/*
			Create the top-level acceleration structure that contains geometry instance information
		*/

		// Single instance with a 3x4 transform matrix for the ray traced triangle
		vks::Buffer instanceBuffer;

		glm::mat3x4 transform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
		};

		GeometryInstance geometryInstance{};
		geometryInstance.transform = transform;
		geometryInstance.instanceId = 0;
		geometryInstance.mask = 0xff;
		geometryInstance.instanceOffset = 0;
		geometryInstance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_CULL_DISABLE_BIT_NV;
		geometryInstance.accelerationStructureHandle = bottomLevelAS.handle;

		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_RAY_TRACING_BIT_NV,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&instanceBuffer,
			sizeof(GeometryInstance),
			&geometryInstance));

		createTopLevelAccelerationStructure();

		/*
			Build the acceleration structure
		*/

		// Acceleration structure build requires some scratch space to store temporary information
		VkAccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo{};
		memoryRequirementsInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
		memoryRequirementsInfo.type = VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV;

		VkMemoryRequirements2 memReqBottomLevelAS;
		memoryRequirementsInfo.accelerationStructure = bottomLevelAS.accelerationStructure;
		vkGetAccelerationStructureMemoryRequirementsNV(device, &memoryRequirementsInfo, &memReqBottomLevelAS);

		VkMemoryRequirements2 memReqTopLevelAS;
		memoryRequirementsInfo.accelerationStructure = topLevelAS.accelerationStructure;
		vkGetAccelerationStructureMemoryRequirementsNV(device, &memoryRequirementsInfo, &memReqTopLevelAS);

		const VkDeviceSize scratchBufferSize = std::max(memReqBottomLevelAS.memoryRequirements.size, memReqTopLevelAS.memoryRequirements.size);

		vks::Buffer scratchBuffer;
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_RAY_TRACING_BIT_NV,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			&scratchBuffer,
			scratchBufferSize));

		VkCommandBuffer cmdBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

		/*
			Build bottom level acceleration structure
		*/
		VkAccelerationStructureInfoNV buildInfo{};
		buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
		buildInfo.geometryCount = 1;
		buildInfo.pGeometries = &geometry;

		vkCmdBuildAccelerationStructureNV(
			cmdBuffer,
			&buildInfo,
			VK_NULL_HANDLE,
			0,
			VK_FALSE,
			bottomLevelAS.accelerationStructure,
			VK_NULL_HANDLE,
			scratchBuffer.buffer,
			0);

		VkMemoryBarrier memoryBarrier = vks::initializers::memoryBarrier();
		memoryBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
		memoryBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
		vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, 0, 1, &memoryBarrier, 0, 0, 0, 0);

		/*
			Build top-level acceleration structure
		*/
		buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
		buildInfo.pGeometries = 0;
		buildInfo.geometryCount = 0;
		buildInfo.instanceCount = 1;

		vkCmdBuildAccelerationStructureNV(
			cmdBuffer,
			&buildInfo,
			instanceBuffer.buffer,
			0,
			VK_FALSE,
			topLevelAS.accelerationStructure,
			VK_NULL_HANDLE,
			scratchBuffer.buffer,
			0);

		vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, 0, 1, &memoryBarrier, 0, 0, 0, 0);

		vulkanDevice->flushCommandBuffer(cmdBuffer, queue);

		scratchBuffer.destroy();
		instanceBuffer.destroy();
	}

	VkDeviceSize copyShaderIdentifier(uint8_t* data, const uint8_t* shaderHandleStorage, uint32_t groupIndex) {
		const uint32_t shaderGroupHandleSize = rayTracingProperties.shaderGroupHandleSize;
		memcpy(data, shaderHandleStorage + groupIndex * shaderGroupHandleSize, shaderGroupHandleSize);
		return shaderGroupHandleSize;
	}

	/*
		Create the Shader Binding Table that binds the programs and top-level acceleration structure
	*/
	void createShaderBindingTable() {
		std::cout << "Creating shader binding table." << std::endl;
		// Create buffer for the shader binding table
		const uint32_t sbtSize = rayTracingProperties.shaderGroupHandleSize * NUM_SHADER_GROUPS;
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_RAY_TRACING_BIT_NV,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			&shaderBindingTable,
			sbtSize));
		shaderBindingTable.map();

		auto shaderHandleStorage = new uint8_t[sbtSize];
		// Get shader identifiers
		VK_CHECK_RESULT(vkGetRayTracingShaderGroupHandlesNV(device, pipeline, 0, NUM_SHADER_GROUPS, sbtSize, shaderHandleStorage));
		auto* data = static_cast<uint8_t*>(shaderBindingTable.mapped);
		// Copy the shader identifiers to the shader binding table
		data += copyShaderIdentifier(data, shaderHandleStorage, INDEX_RAYGEN);
		data += copyShaderIdentifier(data, shaderHandleStorage, INDEX_MISS);
		data += copyShaderIdentifier(data, shaderHandleStorage, INDEX_CLOSEST_HIT);
		shaderBindingTable.unmap();
	}

	/*
		Create the descriptor sets used for the ray tracing dispatch
	*/
	void createDescriptorSets()
	{
		std::vector<VkDescriptorPoolSize> poolSizes = {
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV, 1 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }
		};
		VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 1);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, nullptr, &descriptorPool));

		VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayout, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &descriptorSet));

		VkWriteDescriptorSetAccelerationStructureNV descriptorAccelerationStructureInfo{};
		descriptorAccelerationStructureInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV;
		descriptorAccelerationStructureInfo.accelerationStructureCount = 1;
		descriptorAccelerationStructureInfo.pAccelerationStructures = &topLevelAS.accelerationStructure;

		VkWriteDescriptorSet accelerationStructureWrite{};
		accelerationStructureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		// The specialized acceleration structure descriptor has to be chained
		accelerationStructureWrite.pNext = &descriptorAccelerationStructureInfo;
		accelerationStructureWrite.dstSet = descriptorSet;
		accelerationStructureWrite.dstBinding = 0;
		accelerationStructureWrite.descriptorCount = 1;
		accelerationStructureWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;

		VkDescriptorImageInfo storageImageDescriptor{};
		storageImageDescriptor.imageView = storageImage.view;
		storageImageDescriptor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorBufferInfo vertexBufferDescriptor{};
		vertexBufferDescriptor.buffer = scene.vertices.buffer;
		vertexBufferDescriptor.range = VK_WHOLE_SIZE;

		VkDescriptorBufferInfo indexBufferDescriptor{};
		indexBufferDescriptor.buffer = scene.indices.buffer;
		indexBufferDescriptor.range = VK_WHOLE_SIZE;

		VkWriteDescriptorSet resultImageWrite = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &storageImageDescriptor);
		VkWriteDescriptorSet uniformBufferWrite = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &ubo.descriptor);
		VkWriteDescriptorSet vertexBufferWrite = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, &vertexBufferDescriptor);
		VkWriteDescriptorSet indexBufferWrite = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, &indexBufferDescriptor);

		std::vector<VkWriteDescriptorSet> writeDescriptorSets = {
			accelerationStructureWrite,
			resultImageWrite,
			uniformBufferWrite,
			vertexBufferWrite,
			indexBufferWrite
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, VK_NULL_HANDLE);
	}

	/*
		Create our ray tracing pipeline
	*/
	void createRayTracingPipeline()
	{
		std::cout << "Creating ray tracing pipeline." << std::endl;
		VkDescriptorSetLayoutBinding accelerationStructureLayoutBinding{};
		accelerationStructureLayoutBinding.binding = 0;
		accelerationStructureLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;
		accelerationStructureLayoutBinding.descriptorCount = 1;
		accelerationStructureLayoutBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

		VkDescriptorSetLayoutBinding resultImageLayoutBinding{};
		resultImageLayoutBinding.binding = 1;
		resultImageLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		resultImageLayoutBinding.descriptorCount = 1;
		resultImageLayoutBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

		VkDescriptorSetLayoutBinding uniformBufferBinding{};
		uniformBufferBinding.binding = 2;
		uniformBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		uniformBufferBinding.descriptorCount = 1;
		uniformBufferBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_NV;

		VkDescriptorSetLayoutBinding vertexBufferBinding{};
		vertexBufferBinding.binding = 3;
		vertexBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		vertexBufferBinding.descriptorCount = 1;
		vertexBufferBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV;

		VkDescriptorSetLayoutBinding indexBufferBinding{};
		indexBufferBinding.binding = 4;
		indexBufferBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		indexBufferBinding.descriptorCount = 1;
		indexBufferBinding.stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV;

		std::vector<VkDescriptorSetLayoutBinding> bindings({
			accelerationStructureLayoutBinding,
			resultImageLayoutBinding,
			uniformBufferBinding,
			vertexBufferBinding,
			indexBufferBinding
			});

		VkDescriptorSetLayoutCreateInfo layoutInfo{};
		layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
		layoutInfo.pBindings = bindings.data();
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout));

		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
		pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayout;

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

		const uint32_t shaderIndexRaygen = 0;
		const uint32_t shaderIndexMiss = 1;
		const uint32_t shaderIndexClosestHit = 2;

		std::array<VkPipelineShaderStageCreateInfo, 3> shaderStages;
		shaderStages[shaderIndexRaygen] = loadShader(getShadersPath() + "nv_ray_tracing_basic/raygen.rgen.spv", VK_SHADER_STAGE_RAYGEN_BIT_NV);
		shaderStages[shaderIndexMiss] = loadShader(getShadersPath() + "nv_ray_tracing_basic/miss.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_NV);
		shaderStages[shaderIndexClosestHit] = loadShader(getShadersPath() + "nv_ray_tracing_basic/closesthit.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV);

		/*
			Setup ray tracing shader groups
		*/
		std::array<VkRayTracingShaderGroupCreateInfoNV, NUM_SHADER_GROUPS> groups{};
		for (auto& group : groups) {
			// Init all groups with some default values
			group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV;
			group.generalShader = VK_SHADER_UNUSED_NV;
			group.closestHitShader = VK_SHADER_UNUSED_NV;
			group.anyHitShader = VK_SHADER_UNUSED_NV;
			group.intersectionShader = VK_SHADER_UNUSED_NV;
		}

		// Links shaders and types to ray tracing shader groups
		// Ray generation shader group
		groups[INDEX_RAYGEN].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
		groups[INDEX_RAYGEN].generalShader = shaderIndexRaygen;
		// Scene miss shader group
		groups[INDEX_MISS].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
		groups[INDEX_MISS].generalShader = shaderIndexMiss;
		// Scene closest hit shader group
		groups[INDEX_CLOSEST_HIT].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_NV;
		groups[INDEX_CLOSEST_HIT].generalShader = VK_SHADER_UNUSED_NV;
		groups[INDEX_CLOSEST_HIT].closestHitShader = shaderIndexClosestHit;

		VkRayTracingPipelineCreateInfoNV rayPipelineInfo{};
		rayPipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_NV;
		rayPipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
		rayPipelineInfo.pStages = shaderStages.data();
		rayPipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
		rayPipelineInfo.pGroups = groups.data();
		rayPipelineInfo.maxRecursionDepth = 1;
		rayPipelineInfo.layout = pipelineLayout;
		VK_CHECK_RESULT(vkCreateRayTracingPipelinesNV(device, VK_NULL_HANDLE, 1, &rayPipelineInfo, nullptr, &pipeline));
	}

	/*
		Create the uniform buffer used to pass matrices to the ray tracing ray generation shader
	*/
	void createUniformBuffer()
	{
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&ubo,
			sizeof(uniformData),
			&uniformData));
		VK_CHECK_RESULT(ubo.map());

		updateUniformBuffers();
	}

	/*
		Command buffer generation
	*/
	void buildCommandBuffers()
	{
		VkCommandBuffer commandBuffer;
		VkCommandBufferAllocateInfo cmdBufAllocateInfo =
			vks::initializers::commandBufferAllocateInfo(cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
		VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &commandBuffer));

		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		// QUESTION: what is this
		VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &cmdBufInfo));

		/*
			Dispatch the ray tracing commands
		*/
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, pipeline);
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, pipelineLayout, 0, 1, &descriptorSet, 0, 0);

		// Calculate shader binding offsets, which is pretty straight forward in our example
		VkDeviceSize bindingOffsetRayGenShader = rayTracingProperties.shaderGroupHandleSize * INDEX_RAYGEN;
		VkDeviceSize bindingOffsetMissShader = rayTracingProperties.shaderGroupHandleSize * INDEX_MISS;
		VkDeviceSize bindingOffsetHitShader = rayTracingProperties.shaderGroupHandleSize * INDEX_CLOSEST_HIT;
		VkDeviceSize bindingStride = rayTracingProperties.shaderGroupHandleSize;

		runtime = 0.0;
		auto tStart = std::chrono::high_resolution_clock::now();

		vkCmdTraceRaysNV(commandBuffer,
			shaderBindingTable.buffer, bindingOffsetRayGenShader,
			shaderBindingTable.buffer, bindingOffsetMissShader, bindingStride,
			shaderBindingTable.buffer, bindingOffsetHitShader, bindingStride,
			VK_NULL_HANDLE, 0, 0,
			width, height, 1);


		VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));
		submitWork(commandBuffer, queue);
		vkDeviceWaitIdle(device);
		runtime = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - tStart).count();
	}

	void updateUniformBuffers()
	{
		uniformData.projInverse = glm::inverse(camera.matrices.perspective);
		uniformData.viewInverse = glm::inverse(camera.matrices.view);
		memcpy(ubo.mapped, &uniformData, sizeof(uniformData));
	}

	void prepare()
	{
		VulkanExampleBase::prepare();

		// Query the ray tracing properties of the current implementation, we will need them later on
		rayTracingProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PROPERTIES_NV;
		VkPhysicalDeviceProperties2 deviceProps2{};
		deviceProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		deviceProps2.pNext = &rayTracingProperties;
		vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProps2);
		std::cout << "GPU: " << deviceProperties.deviceName << std::endl;

		// Get VK_NV_ray_tracing related function pointers
		vkCreateAccelerationStructureNV = reinterpret_cast<PFN_vkCreateAccelerationStructureNV>(vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureNV"));
		vkDestroyAccelerationStructureNV = reinterpret_cast<PFN_vkDestroyAccelerationStructureNV>(vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureNV"));
		vkBindAccelerationStructureMemoryNV = reinterpret_cast<PFN_vkBindAccelerationStructureMemoryNV>(vkGetDeviceProcAddr(device, "vkBindAccelerationStructureMemoryNV"));
		vkGetAccelerationStructureHandleNV = reinterpret_cast<PFN_vkGetAccelerationStructureHandleNV>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureHandleNV"));
		vkGetAccelerationStructureMemoryRequirementsNV = reinterpret_cast<PFN_vkGetAccelerationStructureMemoryRequirementsNV>(vkGetDeviceProcAddr(device, "vkGetAccelerationStructureMemoryRequirementsNV"));
		vkCmdBuildAccelerationStructureNV = reinterpret_cast<PFN_vkCmdBuildAccelerationStructureNV>(vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructureNV"));
		vkCreateRayTracingPipelinesNV = reinterpret_cast<PFN_vkCreateRayTracingPipelinesNV>(vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesNV"));
		vkGetRayTracingShaderGroupHandlesNV = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesNV>(vkGetDeviceProcAddr(device, "vkGetRayTracingShaderGroupHandlesNV"));
		vkCmdTraceRaysNV = reinterpret_cast<PFN_vkCmdTraceRaysNV>(vkGetDeviceProcAddr(device, "vkCmdTraceRaysNV"));

		createScene();
		createStorageImage();
		createUniformBuffer();
		createRayTracingPipeline();
		createShaderBindingTable();
		createDescriptorSets();
		buildCommandBuffers();
		prepared = true;
	}

	void draw()
	{
		std::cout << "Drawing." << std::endl;


		/*
			Copy framebuffer image to host visible image
		*/
		const char* imagedata;
		{
			// Create the linear tiled destination image to copy to and to read the memory from
			VkImageCreateInfo imgCreateInfo(vks::initializers::imageCreateInfo());
			imgCreateInfo.imageType = VK_IMAGE_TYPE_2D;
			imgCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
			imgCreateInfo.extent.width = width;
			imgCreateInfo.extent.height = height;
			imgCreateInfo.extent.depth = 1;
			imgCreateInfo.arrayLayers = 1;
			imgCreateInfo.mipLevels = 1;
			imgCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imgCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imgCreateInfo.tiling = VK_IMAGE_TILING_LINEAR;
			imgCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			// Create the image
			VkImage dstImage;
			VK_CHECK_RESULT(vkCreateImage(device, &imgCreateInfo, nullptr, &dstImage));
			// Create memory to back up the image
			VkMemoryRequirements memRequirements;
			VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
			VkDeviceMemory dstImageMemory;
			vkGetImageMemoryRequirements(device, dstImage, &memRequirements);
			memAllocInfo.allocationSize = memRequirements.size;
			// Memory must be host visible to copy from
			memAllocInfo.memoryTypeIndex = vulkanDevice->getMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &dstImageMemory));
			VK_CHECK_RESULT(vkBindImageMemory(device, dstImage, dstImageMemory, 0));

			// Do the actual blit from the offscreen image to our host visible destination image
			VkCommandBufferAllocateInfo cmdBufAllocateInfo = vks::initializers::commandBufferAllocateInfo(cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			VkCommandBuffer copyCmd;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &copyCmd));
			VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			// Transition destination image to transfer destination layout
			vks::tools::insertImageMemoryBarrier(
				copyCmd,
				dstImage,
				0,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

			// colorAttachment.image is already in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, and does not need to be transitioned

			VkImageCopy imageCopyRegion{};
			imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.srcSubresource.layerCount = 1;
			imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageCopyRegion.dstSubresource.layerCount = 1;
			imageCopyRegion.extent.width = width;
			imageCopyRegion.extent.height = height;
			imageCopyRegion.extent.depth = 1;

			vkCmdCopyImage(
				copyCmd,
				storageImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1,
				&imageCopyRegion);

			// Transition destination image to general layout, which is the required layout for mapping the image memory later on
			vks::tools::insertImageMemoryBarrier(
				copyCmd,
				dstImage,
				VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_ACCESS_MEMORY_READ_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

			submitWork(copyCmd, queue);

			// Get layout of the image (including row pitch)
			VkImageSubresource subResource{};
			subResource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			VkSubresourceLayout subResourceLayout;

			vkGetImageSubresourceLayout(device, dstImage, &subResource, &subResourceLayout);

			// Map image memory so we can start copying from it
			vkMapMemory(device, dstImageMemory, 0, VK_WHOLE_SIZE, 0, (void**)&imagedata);
			imagedata += subResourceLayout.offset;

		/*
			Save host visible framebuffer image to disk (ppm format)
		*/

			std::string filename = "nv_trace_" + model_name + ".ppm";
			std::cout << "Creating " << filename << std::endl;
			std::ofstream file(filename, std::ios::out | std::ios::binary);

			// ppm header
			file << "P6\n" << width << "\n" << height << "\n" << 255 << "\n";

			// If source is BGR (destination is always RGB) and we can't use blit (which does automatic conversion), we'll have to manually swizzle color components
			// Check if source is BGR and needs swizzle
			std::vector<VkFormat> formatsBGR = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM };
			const bool colorSwizzle = (std::find(formatsBGR.begin(), formatsBGR.end(), VK_FORMAT_R8G8B8A8_UNORM) != formatsBGR.end());

			// ppm binary pixel data
			for (int32_t y = 0; y < height; y++) {
				unsigned int *row = (unsigned int*)imagedata;
				for (int32_t x = 0; x < width; x++) {
					if (colorSwizzle) {
						file.write((char*)row + 2, 1);
						file.write((char*)row + 1, 1);
						file.write((char*)row, 1);
					}
					else {
						file.write((char*)row, 3);
					}
					row++;
				}
				imagedata += subResourceLayout.rowPitch;
			}
			file.close();

			// Clean up resources
			vkUnmapMemory(device, dstImageMemory);
			vkFreeMemory(device, dstImageMemory, nullptr);
			vkDestroyImage(device, dstImage, nullptr);
		}

		vkQueueWaitIdle(queue);
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();
		if (camera.updated)
			updateUniformBuffers();
	}
};

VulkanExample *vulkanExample;
static void handleEvent(const xcb_generic_event_t *event)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleEvent(event);
	}
}
int main(const int argc, const char *argv[])
{
	for (size_t i = 0; i < argc; i++) { 
		VulkanExample::args.push_back(argv[i]); 
	}; 
	
	std::cout << "(single non-recursive trace)" << std::endl;
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->setupWindow();
	vulkanExample->prepare();
	

	vulkanExample->draw(); /* vulkanExample->renderLoop();	*/
	
	unsigned total_rays = vulkanExample->width * vulkanExample->height;
	std::cout << "Total rays: " << vulkanExample->width << "x" << vulkanExample->height << "x" << 1 << "= " << total_rays << std::endl;
	std::cout << "Runtime: " << (vulkanExample->runtime / 1000.0) << "s" << std::endl;
	std::cout << "Rays/s: " << total_rays/(vulkanExample->runtime / 1000.0) << std::endl;
	
	delete(vulkanExample);
	
	return 0;
}

