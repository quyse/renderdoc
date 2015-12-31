/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "vk_replay.h"
#include "vk_core.h"
#include "vk_resources.h"

#include "serialise/string_utils.h"

VulkanReplay::OutputWindow::OutputWindow() : wnd(NULL_WND_HANDLE), width(0), height(0),
	dsimg(VK_NULL_HANDLE), dsmem(VK_NULL_HANDLE)
{
	swap = VK_NULL_HANDLE;
	for(size_t i=0; i < ARRAY_COUNT(colimg); i++)
	{
		colimg[i] = VK_NULL_HANDLE;
		colview[i] = VK_NULL_HANDLE;
		fb[i] = VK_NULL_HANDLE;
		fbdepth[i] = VK_NULL_HANDLE;
	}

	VkImageMemoryBarrier t = {
		VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
		0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,
		0, 0, VK_NULL_HANDLE,
		{ VK_IMAGE_ASPECT_COLOR, 0, 1, 0, 1 }
	};
	for(size_t i=0; i < ARRAY_COUNT(coltrans); i++)
		coltrans[i] = t;

	t.subresourceRange.aspect = VK_IMAGE_ASPECT_DEPTH;
	depthtrans = t;

	t.subresourceRange.aspect = VK_IMAGE_ASPECT_STENCIL;
	stenciltrans = t;
}

void VulkanReplay::OutputWindow::SetCol(VkDeviceMemory mem, VkImage img)
{
}

void VulkanReplay::OutputWindow::SetDS(VkDeviceMemory mem, VkImage img)
{
}

void VulkanReplay::OutputWindow::MakeTargets(const VulkanFunctions &vk, VkDevice device, bool depth)
{
	vk.vkDeviceWaitIdle(device);

	for(size_t i=0; i < ARRAY_COUNT(colimg); i++)
	{
		if(colimg[i] != VK_NULL_HANDLE)
		{
			vk.vkDestroyAttachmentView(device, colview[i]);
			vk.vkDestroyFramebuffer(device, fb[i]);
			if(dsimg != VK_NULL_HANDLE)
				vk.vkDestroyFramebuffer(device, fbdepth[i]);

			fb[i] = VK_NULL_HANDLE;
			fbdepth[i] = VK_NULL_HANDLE;
			colimg[i] = VK_NULL_HANDLE;
			colview[i] = VK_NULL_HANDLE;
		}
	}

	if(dsimg != VK_NULL_HANDLE)
	{
		vk.vkDestroyAttachmentView(device, dsview);
		vk.vkDestroyImage(device, dsimg);
		vk.vkFreeMemory(device, dsmem);

		dsview = VK_NULL_HANDLE;
		dsimg = VK_NULL_HANDLE;
		dsmem = VK_NULL_HANDLE;
	}

	VkSwapChainWSI old = swap;

	void *handleptr = NULL;
	void *wndptr = NULL;
	VkPlatformWSI platform = VK_PLATFORM_MAX_ENUM_WSI;

#if defined(WIN32)
	static int dllLocator=0;

	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,	(const char *)&dllLocator, (HMODULE *)&handleptr);
	wndptr = wnd;
	platform = VK_PLATFORM_WIN32_WSI;
#elif defined(__linux__)
	VkPlatformHandleXcbWSI handle;
	handle.connection = connection;
	handle.root = screen->root;

	handleptr = &handle;
	wndptr = &wnd;
	platform = VK_PLATFORM_X11_WSI;
#else
#error "unknown platform"
#endif

	VkSurfaceDescriptionWindowWSI surfDesc = { VK_STRUCTURE_TYPE_SURFACE_DESCRIPTION_WINDOW_WSI, NULL, platform, handleptr, wndptr };

	// VKTODOHIGH need to verify which present modes are present
	VkSwapChainCreateInfoWSI swapInfo = {
			VK_STRUCTURE_TYPE_SWAP_CHAIN_CREATE_INFO_WSI, NULL, (VkSurfaceDescriptionWSI *)&surfDesc,
			2, VK_FORMAT_B8G8R8A8_UNORM, { width, height },
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DESTINATION_BIT,
			VK_SURFACE_TRANSFORM_NONE_WSI, 1, VK_PRESENT_MODE_MAILBOX_WSI,
			old, true,
	};

	VkResult res = vk.vkCreateSwapChainWSI(device, &swapInfo, &swap);
	RDCASSERT(res == VK_SUCCESS);

	if(old != VK_NULL_HANDLE)
		vk.vkDestroySwapChainWSI(device, old);

	size_t sz;
	res = vk.vkGetSwapChainInfoWSI(device, swap, VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI, &sz, NULL);
	RDCASSERT(res == VK_SUCCESS);

	numImgs = uint32_t(sz/sizeof(VkSwapChainImagePropertiesWSI));

	VkSwapChainImagePropertiesWSI* imgs = new VkSwapChainImagePropertiesWSI[numImgs];
	res = vk.vkGetSwapChainInfoWSI(device, swap, VK_SWAP_CHAIN_INFO_TYPE_IMAGES_WSI, &sz, imgs);
	RDCASSERT(res == VK_SUCCESS);

	for(size_t i=0; i < numImgs; i++)
	{
		colimg[i] = imgs[i].image;
		coltrans[i].image = imgs[i].image;
		coltrans[i].oldLayout = coltrans[i].newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	}

	if(depth)
	{
		VULKANNOTIMP("Allocating depth-stencil image");

		/*
		dsmem = mem;
		dsimg = img;
		depthtrans.image = stenciltrans.image = img;
		depthtrans.oldLayout = depthtrans.newLayout = 
			stenciltrans.oldLayout = stenciltrans.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		*/
	}

	{
		VkAttachmentDescription attDesc = {
			VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION, NULL,
			VK_FORMAT_B8G8R8A8_UNORM, 1,
			VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
		};

		VkAttachmentReference attRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

		VkSubpassDescription sub = {
			VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION, NULL,
			VK_PIPELINE_BIND_POINT_GRAPHICS, 0,
			0, NULL, // inputs
			1, &attRef, // color
			NULL, // resolve
			{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED }, // depth-stencil
			0, NULL, // preserve
		};

		VkRenderPassCreateInfo rpinfo = {
				VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL,
				1, &attDesc,
				1, &sub,
				0, NULL, // dependencies
		};

		res = vk.vkCreateRenderPass(device, &rpinfo, &renderpass);
		RDCASSERT(res == VK_SUCCESS);
	}

	for(uint32_t i=0; i < numImgs; i++)
	{
		if(colimg[i] != VK_NULL_HANDLE)
		{
			VkAttachmentViewCreateInfo info = {
				VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO, NULL,
				colimg[i], VK_FORMAT_B8G8R8A8_UNORM, 0, 0, 1,
				0 };

			res = vk.vkCreateAttachmentView(device, &info, &colview[i]);
			RDCASSERT(res == VK_SUCCESS);

			VkAttachmentBindInfo attBind = { colview[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

			VkFramebufferCreateInfo fbinfo = {
				VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL,
				renderpass,
				1, &attBind,
				(uint32_t)width, (uint32_t)height, 1,
			};

			res = vk.vkCreateFramebuffer(device, &fbinfo, &fb[i]);
			RDCASSERT(res == VK_SUCCESS);
		}
	}

	if(dsimg != VK_NULL_HANDLE)
	{
		VkAttachmentViewCreateInfo info = {
			VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO, NULL,
			dsimg, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, 0, 1,
			0 };

		vk.vkCreateAttachmentView(device, &info, &dsview);
	}
}

VulkanReplay::VulkanReplay()
{
	m_pDriver = NULL;
	m_Proxy = false;

	m_OutputWinID = 1;
	m_ActiveWinID = 0;
	m_BindDepth = false;
}

void VulkanReplay::InitDebugData()
{
	const VulkanFunctions &vk = m_pDriver->m_Real;
	VkDevice dev = m_pDriver->GetDev();
	
	VkResult vkr = VK_SUCCESS;

	// VKTODOMED all of this is leaking

	VkPipelineCacheCreateInfo cacheInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, NULL, 0 };

	vkr = vk.vkCreatePipelineCache(dev, &cacheInfo, &m_PipelineCache);
	RDCASSERT(vkr == VK_SUCCESS);

	VkDescriptorSetLayoutBinding layoutBinding[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL, }
	};
	
	VkDescriptorSetLayoutCreateInfo descsetLayoutInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
		ARRAY_COUNT(layoutBinding), &layoutBinding[0],
	};
	
	vkr = vk.vkCreateDescriptorSetLayout(dev, &descsetLayoutInfo, &m_TexDisplayDescSetLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VkPipelineLayoutCreateInfo pipeLayoutInfo = {
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL,
		1, &m_TexDisplayDescSetLayout,
		0, NULL, // push constant ranges
	};
	
	vkr = vk.vkCreatePipelineLayout(dev, &pipeLayoutInfo, &m_TexDisplayPipeLayout);
	RDCASSERT(vkr == VK_SUCCESS);

	VkDescriptorTypeCount descPoolTypes[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, },
	};
	
	VkDescriptorPoolCreateInfo descpoolInfo = {
		VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
    ARRAY_COUNT(descPoolTypes), &descPoolTypes[0],
	};
	
	vkr = vk.vkCreateDescriptorPool(dev, VK_DESCRIPTOR_POOL_USAGE_ONE_SHOT, 1, &descpoolInfo, &m_TexDisplayDescPool);
	RDCASSERT(vkr == VK_SUCCESS);
	
	uint32_t count;
	vkr = vk.vkAllocDescriptorSets(dev, m_TexDisplayDescPool, VK_DESCRIPTOR_SET_USAGE_STATIC, 1, &m_TexDisplayDescSetLayout, &m_TexDisplayDescSet, &count);
	RDCASSERT(vkr == VK_SUCCESS);

	VkMemoryAllocInfo allocInfo = {
		VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO, NULL,
		128, 2, // VKTODOHIGH find appropriate memory type index
	};

	vkr = vk.vkAllocMemory(dev, &allocInfo, &m_TexDisplayUBO.mem);
	RDCASSERT(vkr == VK_SUCCESS);

	VkBufferCreateInfo bufInfo = {
		VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL,
		128, VK_BUFFER_USAGE_GENERAL, 0,
		VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
	};

	vkr = vk.vkCreateBuffer(dev, &bufInfo, &m_TexDisplayUBO.buf);
	RDCASSERT(vkr == VK_SUCCESS);

	vkr = vk.vkBindBufferMemory(dev, m_TexDisplayUBO.buf, m_TexDisplayUBO.mem, 0);
	RDCASSERT(vkr == VK_SUCCESS);

	VkBufferViewCreateInfo bufviewInfo = {
		VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL,
		m_TexDisplayUBO.buf, VK_BUFFER_VIEW_TYPE_RAW,
		VK_FORMAT_UNDEFINED, 0, 128,
	};

	vkr = vk.vkCreateBufferView(dev, &bufviewInfo, &m_TexDisplayUBO.view);
	RDCASSERT(vkr == VK_SUCCESS);

	Vec4f *data = NULL;
	vkr = vk.vkMapMemory(dev, m_TexDisplayUBO.mem, 0, 0, 0, (void **)&data);
	data[0] = Vec4f(0.6f, 0.0f, 0.0f, 1.0f);
	data[1] = Vec4f(0.0f, 0.0f, 0.6f, 1.0f);
	vk.vkUnmapMemory(dev, m_TexDisplayUBO.mem);

	VkDescriptorInfo desc = { 0 }; desc.bufferView = m_TexDisplayUBO.view;

	VkWriteDescriptorSet writeSet = {
		VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL,
		m_TexDisplayDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &desc
	};

	vkr = vk.vkUpdateDescriptorSets(dev, 1, &writeSet, 0, NULL);
	RDCASSERT(vkr == VK_SUCCESS);

	VkDynamicRasterStateCreateInfo rsInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_RASTER_STATE_CREATE_INFO, NULL,
		0.0f, 0.0f, 0.0f, 1.0f,
	};

	vkr = vk.vkCreateDynamicRasterState(dev, &rsInfo, &m_DynamicRSState);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VkDynamicColorBlendStateCreateInfo cbInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_COLOR_BLEND_STATE_CREATE_INFO, NULL,
		{ 1.0f, 1.0f, 1.0f, 1.0f },
	};

	vkr = vk.vkCreateDynamicColorBlendState(dev, &cbInfo, &m_DynamicCBStateWhite);
	RDCASSERT(vkr == VK_SUCCESS);
	
	VkDynamicDepthStencilStateCreateInfo dsInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_DEPTH_STENCIL_STATE_CREATE_INFO, NULL,
		0.0f, 1.0f, 0xff, 0xff, 0, 0,
	};

	vkr = vk.vkCreateDynamicDepthStencilState(dev, &dsInfo, &m_DynamicDSStateDisabled);
	RDCASSERT(vkr == VK_SUCCESS);
	
	string shaderSources[] = {
		GetEmbeddedResource(blitvs_spv),
		GetEmbeddedResource(checkerboardfs_spv),
		GetEmbeddedResource(texdisplayfs_spv),
	};
	
	enum shaderIdx
	{
		BLITVS,
		CHECKERBOARDFS,
		TEXDISPLAYFS,
	};

	VkShaderModule module[ARRAY_COUNT(shaderSources)];
	VkShader shader[ARRAY_COUNT(shaderSources)];
	
	for(size_t i=0; i < ARRAY_COUNT(module); i++)
	{
		VkShaderModuleCreateInfo modinfo = {
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL,
			shaderSources[i].size(), (void *)&shaderSources[i][0], 0,
		};

		vkr = vk.vkCreateShaderModule(dev, &modinfo, &module[i]);
		RDCASSERT(vkr == VK_SUCCESS);

		VkShaderCreateInfo shadinfo = {
			VK_STRUCTURE_TYPE_SHADER_CREATE_INFO, NULL,
			module[i], "main", 0,
		};

		vkr = vk.vkCreateShader(dev, &shadinfo, &shader[i]);
		RDCASSERT(vkr == VK_SUCCESS);
	}

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, VK_SHADER_STAGE_VERTEX, shader[0], NULL },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, VK_SHADER_STAGE_FRAGMENT, shader[1], NULL },
	};

	VkPipelineInputAssemblyStateCreateInfo ia = {
		VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL,
		VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false,
	};

	VkPipelineViewportStateCreateInfo vp = {
		VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL,
		1,
	};

	VkPipelineRasterStateCreateInfo rs = {
		VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO, NULL,
		true, false, VK_FILL_MODE_SOLID, VK_CULL_MODE_NONE, VK_FRONT_FACE_CW,
	};

	VkPipelineMultisampleStateCreateInfo msaa = {
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL,
		1, false, 0.0f, 1,
	};

	VkPipelineDepthStencilStateCreateInfo ds = {
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL,
		false, false, VK_COMPARE_OP_ALWAYS, false, false,
		{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS },
		{ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS },
	};

	VkPipelineColorBlendAttachmentState attState = {
		false,
		VK_BLEND_ONE, VK_BLEND_ZERO, VK_BLEND_OP_ADD,
		VK_BLEND_ONE, VK_BLEND_ZERO, VK_BLEND_OP_ADD,
		0xf,
	};

	VkPipelineColorBlendStateCreateInfo cb = {
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL,
		false, false, VK_LOGIC_OP_NOOP,
		1, &attState,
	};

	VkGraphicsPipelineCreateInfo pipeInfo = {
		VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL,
		2, stages,
		NULL, // vertex input
		&ia,
		NULL, // tess
		&vp,
		&rs,
		&msaa,
		&ds,
		&cb,
		0, // flags
		m_TexDisplayPipeLayout,
		VK_NULL_HANDLE, // render pass
		0, // sub pass
		VK_NULL_HANDLE, // base pipeline handle
		0, // base pipeline index
	};

	stages[0].shader = shader[BLITVS];
	stages[1].shader = shader[CHECKERBOARDFS];

	vkr = vk.vkCreateGraphicsPipelines(dev, m_PipelineCache, 1, &pipeInfo, &m_CheckerboardPipeline);
	RDCASSERT(vkr == VK_SUCCESS);
	
	stages[1].shader = shader[TEXDISPLAYFS];

	vkr = vk.vkCreateGraphicsPipelines(dev, m_PipelineCache, 1, &pipeInfo, &m_TexDisplayPipeline);
	RDCASSERT(vkr == VK_SUCCESS);

	for(size_t i=0; i < ARRAY_COUNT(module); i++)
	{
		vkr = vk.vkDestroyShader(dev, shader[i]);
		RDCASSERT(vkr == VK_SUCCESS);

		vkr = vk.vkDestroyShaderModule(dev, module[i]);
		RDCASSERT(vkr == VK_SUCCESS);
	}
}

void VulkanReplay::Shutdown()
{
	delete m_pDriver;
}

APIProperties VulkanReplay::GetAPIProperties()
{
	APIProperties ret;

	ret.pipelineType = ePipelineState_D3D11;
	ret.degraded = false;

	return ret;
}

void VulkanReplay::ReadLogInitialisation()
{
	m_pDriver->ReadLogInitialisation();

	InitDebugData();
}

void VulkanReplay::ReplayLog(uint32_t frameID, uint32_t startEventID, uint32_t endEventID, ReplayLogType replayType)
{
	m_pDriver->ReplayLog(frameID, startEventID, endEventID, replayType);
}

ResourceId VulkanReplay::GetLiveID(ResourceId id)
{
	return m_pDriver->GetResourceManager()->GetLiveID(id);
}

void VulkanReplay::InitCallstackResolver()
{
	m_pDriver->GetSerialiser()->InitCallstackResolver();
}

bool VulkanReplay::HasCallstacks()
{
	return m_pDriver->GetSerialiser()->HasCallstacks();
}

Callstack::StackResolver *VulkanReplay::GetCallstackResolver()
{
	return m_pDriver->GetSerialiser()->GetCallstackResolver();
}

vector<FetchFrameRecord> VulkanReplay::GetFrameRecord()
{
	return m_pDriver->GetFrameRecord();
}

vector<DebugMessage> VulkanReplay::GetDebugMessages()
{
	VULKANNOTIMP("GetDebugMessages");
	return vector<DebugMessage>();
}

vector<ResourceId> VulkanReplay::GetTextures()
{
	VULKANNOTIMP("GetTextures");
	vector<ResourceId> texs;

	ResourceId id;
	VkImage fakeBBIm = VK_NULL_HANDLE;
	VkExtent3D fakeBBext;
	m_pDriver->GetFakeBB(id, fakeBBIm, fakeBBext);

	texs.push_back(id);
	return texs;
}
	
vector<ResourceId> VulkanReplay::GetBuffers()
{
	VULKANNOTIMP("GetBuffers");
	return vector<ResourceId>();
}

void VulkanReplay::PickPixel(ResourceId texture, uint32_t x, uint32_t y, uint32_t sliceFace, uint32_t mip, uint32_t sample, float pixel[4])
{
	//VULKANNOTIMP("PickPixel");

	ResourceId resid;
	VkImage fakeBBIm = VK_NULL_HANDLE;
	VkExtent3D fakeBBext;
	m_pDriver->GetFakeBB(resid, fakeBBIm, fakeBBext);

	if(x >= (uint32_t)fakeBBext.width || y >= (uint32_t)fakeBBext.height)
	{
		RDCEraseEl(pixel);
		return;
	}

	VkDevice dev = m_pDriver->GetDev();
	VkCmdBuffer cmd = m_pDriver->GetCmd();
	VkQueue q = m_pDriver->GetQ();
	const VulkanFunctions &vk = m_pDriver->m_Real;

	VkDeviceMemory readbackmem = VK_NULL_HANDLE;
	VkBuffer destbuf = VK_NULL_HANDLE;

	{
		VkMemoryAllocInfo allocInfo = {
			/*.sType =*/ VK_STRUCTURE_TYPE_MEMORY_ALLOC_INFO,
			/*.pNext =*/ NULL,
			/*.allocationSize =*/ 128,
			/*.memoryTypeIndex =*/ 0, // VKTODOHIGH find appropriate memory type index
		};

		VkResult res = vk.vkAllocMemory(dev, &allocInfo, &readbackmem);
		RDCASSERT(res == VK_SUCCESS);

		VkBufferCreateInfo bufInfo = {
			VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL,
			128, VK_BUFFER_USAGE_GENERAL, 0,
			VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
		};

		res = vk.vkCreateBuffer(dev, &bufInfo, &destbuf);
		RDCASSERT(res == VK_SUCCESS);

		res = vk.vkBindBufferMemory(dev, destbuf, readbackmem, 0);
		RDCASSERT(res == VK_SUCCESS);

		// VKTODOHIGH find out the actual current image state
		VkImageMemoryBarrier fakeTrans = {
			VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
			0, 0, VK_IMAGE_LAYOUT_PRESENT_SOURCE_WSI, VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL,
			0, 0, fakeBBIm,
			{ VK_IMAGE_ASPECT_COLOR, 0, 1, 0, 1 } };

		VkCmdBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO, NULL, VK_CMD_BUFFER_OPTIMIZE_SMALL_BATCH_BIT | VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT };

		res = vk.vkBeginCommandBuffer(cmd, &beginInfo);
		RDCASSERT(res == VK_SUCCESS);

		void *barrier = (void *)&fakeTrans;
		vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, &barrier);
		fakeTrans.oldLayout = fakeTrans.newLayout;

		VkBufferImageCopy region = {
			0, 128, 1,
			{ VK_IMAGE_ASPECT_COLOR, 0, 0}, { (int)x, (int)y, 0 },
			{ 1, 1, 1 },
		};
		vk.vkCmdCopyImageToBuffer(cmd, fakeBBIm, VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL, destbuf, 1, &region);

		fakeTrans.newLayout = VK_IMAGE_LAYOUT_PRESENT_SOURCE_WSI;
		vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, &barrier);

		vk.vkEndCommandBuffer(cmd);

		vk.vkQueueSubmit(q, 1, &cmd, VK_NULL_HANDLE);

		vk.vkQueueWaitIdle(q);
	}

	// VKTODOHIGH ultra cheeky - map memory directly without copying
	// to host-visible memory
	byte *pData = NULL;
	vk.vkMapMemory(dev, readbackmem, 0, 0, 0, (void **)&pData);

	RDCASSERT(pData != NULL);

	// VKTODOHIGH assuming BGRA
	pixel[2] = float(pData[0])/255.0f;
	pixel[1] = float(pData[1])/255.0f;
	pixel[0] = float(pData[2])/255.0f;
	pixel[3] = float(pData[3])/255.0f;

	vk.vkUnmapMemory(dev, readbackmem);

	vk.vkDeviceWaitIdle(dev);

	vk.vkDestroyBuffer(dev, destbuf);
	vk.vkFreeMemory(dev, readbackmem);
}

uint32_t VulkanReplay::PickVertex(uint32_t frameID, uint32_t eventID, MeshDisplay cfg, uint32_t x, uint32_t y)
{
	RDCUNIMPLEMENTED("PickVertex");
	return ~0U;
}

bool VulkanReplay::RenderTexture(TextureDisplay cfg)
{
	VULKANNOTIMP("RenderTexture");
	return false;
}
	
void VulkanReplay::RenderCheckerboard(Vec3f light, Vec3f dark)
{
	auto it = m_OutputWindows.find(m_ActiveWinID);
	if(m_ActiveWinID == 0 || it == m_OutputWindows.end())
		return;

	OutputWindow &outw = it->second;

	VULKANNOTIMP("RenderCheckerboard");

	VkDevice dev = m_pDriver->GetDev();
	VkCmdBuffer cmd = m_pDriver->GetCmd();
	VkQueue q = m_pDriver->GetQ();
	const VulkanFunctions &vk = m_pDriver->m_Real;

	VkCmdBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO, NULL, VK_CMD_BUFFER_OPTIMIZE_SMALL_BATCH_BIT | VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT };

	VkResult res = vk.vkBeginCommandBuffer(cmd, &beginInfo);
	
	outw.curcoltrans->newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, (void **)&outw.curcoltrans);
	outw.curcoltrans->oldLayout = outw.curcoltrans->newLayout;
	
	VkClearColorValue clearColor = { { RANDF(0.0f, 1.0f), RANDF(0.0f, 1.0f), RANDF(0.0f, 1.0f), 1.0f, } };
	vk.vkCmdClearColorImage(cmd, outw.colimg[outw.curidx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &clearColor, 1, &outw.curcoltrans->subresourceRange);
	
	res = vk.vkEndCommandBuffer(cmd);

	res = vk.vkQueueSubmit(q, 1, &cmd, VK_NULL_HANDLE);
}
	
void VulkanReplay::RenderHighlightBox(float w, float h, float scale)
{
	VULKANNOTIMP("RenderHighlightBox");
}
	
ResourceId VulkanReplay::RenderOverlay(ResourceId texid, TextureDisplayOverlay overlay, uint32_t frameID, uint32_t eventID, const vector<uint32_t> &passEvents)
{
	RDCUNIMPLEMENTED("RenderOverlay");
	return ResourceId();
}
	
void VulkanReplay::RenderMesh(uint32_t frameID, uint32_t eventID, const vector<MeshFormat> &secondaryDraws, MeshDisplay cfg)
{
	RDCUNIMPLEMENTED("RenderMesh");
}

bool VulkanReplay::CheckResizeOutputWindow(uint64_t id)
{
	if(id == 0 || m_OutputWindows.find(id) == m_OutputWindows.end())
		return false;
	
	OutputWindow &outw = m_OutputWindows[id];

	if(outw.wnd == NULL_WND_HANDLE)
		return false;
	
	int32_t w, h;
	GetOutputWindowDimensions(id, w, h);

	if(w != outw.width || h != outw.height)
	{
		outw.width = w;
		outw.height = h;

		// VKTODOHIGH Currently the resize code crashes - unsure why
		if(outw.width > 0 && outw.height > 0 && 0)
		{
			bool depth = (outw.dsimg != VK_NULL_HANDLE);

			outw.MakeTargets(m_pDriver->m_Real, m_pDriver->GetDev(), depth);
		}

		return true;
	}

	return false;
}

void VulkanReplay::BindOutputWindow(uint64_t id, bool depth)
{
	m_ActiveWinID = id;
	m_BindDepth = depth;
	
	auto it = m_OutputWindows.find(id);
	if(id == 0 || it == m_OutputWindows.end())
		return;

	OutputWindow &outw = it->second;

	VkDevice dev = m_pDriver->GetDev();
	VkCmdBuffer cmd = m_pDriver->GetCmd();
	VkQueue q = m_pDriver->GetQ();
	const VulkanFunctions &vk = m_pDriver->m_Real;
	
	VkSemaphore sem;
	VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT };

	vk.vkCreateSemaphore(dev, &semInfo, &sem);

	vk.vkAcquireNextImageWSI(dev, outw.swap, UINT64_MAX, sem, &outw.curidx);

	outw.curcoltrans = &outw.coltrans[outw.curidx];

	vk.vkQueueWaitSemaphore(q, sem);

	vk.vkDestroySemaphore(dev, sem);
}

void VulkanReplay::ClearOutputWindowColour(uint64_t id, float col[4])
{
	VULKANNOTIMP("ClearOutputWindowColour");

	// VKTODOHIGH: same as FlipOutputWindow but do a colour clear
	// ultimately these functions should push commands into a queue and there should be a
	// more explicit start/end render functions (similar to BindOutputWindow, so it
	// could start the command buffer, and an end function could end it and submit it)
}

void VulkanReplay::ClearOutputWindowDepth(uint64_t id, float depth, uint8_t stencil)
{
	VULKANNOTIMP("ClearOutputWindowDepth");

	// VKTODOHIGH: same as FlipOutputWindow but do a depth clear
}

void VulkanReplay::FlipOutputWindow(uint64_t id)
{
	auto it = m_OutputWindows.find(id);
	if(id == 0 || it == m_OutputWindows.end())
		return;

	VULKANNOTIMP("FlipOutputWindow");

	OutputWindow &outw = it->second;

	VkDevice dev = m_pDriver->GetDev();
	VkCmdBuffer cmd = m_pDriver->GetCmd();
	VkQueue q = m_pDriver->GetQ();
	const VulkanFunctions &vk = m_pDriver->m_Real;
	
	// copy fake backbuffer into actual backbuffer

	ResourceId resid;
	VkImage fakeBBIm = VK_NULL_HANDLE;
	VkExtent3D fakeBBext;
	m_pDriver->GetFakeBB(resid, fakeBBIm, fakeBBext);

	// VKTODOHIGH find out the actual current image state
	VkImageMemoryBarrier fakeTrans = {
		VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
		0, 0, VK_IMAGE_LAYOUT_PRESENT_SOURCE_WSI, VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL,
		0, 0, fakeBBIm,
		{ VK_IMAGE_ASPECT_COLOR, 0, 1, 0, 1 } };

	VkCmdBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_CMD_BUFFER_BEGIN_INFO, NULL, VK_CMD_BUFFER_OPTIMIZE_SMALL_BATCH_BIT | VK_CMD_BUFFER_OPTIMIZE_ONE_TIME_SUBMIT_BIT };

	VkResult res = vk.vkBeginCommandBuffer(cmd, &beginInfo);
	RDCASSERT(res == VK_SUCCESS);

	void *barrier = (void *)&fakeTrans;

	vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, &barrier);
	fakeTrans.oldLayout = fakeTrans.newLayout;

	outw.curcoltrans->newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, (void **)&outw.curcoltrans);
	outw.curcoltrans->oldLayout = outw.curcoltrans->newLayout;

	VkViewport vp = { 0.0f, 0.0f, (float)outw.width, (float)outw.height, 0.0f, 1.0f, };
	VkRect2D sc = { { 0, 0 }, { outw.width, outw.height } };

	VkDynamicViewportStateCreateInfo vpInfo = {
		VK_STRUCTURE_TYPE_DYNAMIC_VIEWPORT_STATE_CREATE_INFO, NULL,
		1, &vp, &sc
	};

	VkDynamicViewportState dynVP;

	// VKTODOMED cache this, or create per output window on resize
	VkResult vkr = vk.vkCreateDynamicViewportState(dev, &vpInfo, &dynVP);
	RDCASSERT(vkr == VK_SUCCESS);
	
	{
		VkClearValue clearval = {0};
		clearval.color.f32[2] = clearval.color.f32[3] = 1.0f;
		VkRenderPassBeginInfo rpbegin = {
			VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL,
			outw.renderpass, outw.fb[ outw.curidx ],
			{ { 0, 0, }, { outw.width, outw.height } },
			1, &clearval,
		};
		vk.vkCmdBeginRenderPass(cmd, &rpbegin, VK_RENDER_PASS_CONTENTS_INLINE);

		vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_CheckerboardPipeline);
		vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TexDisplayPipeLayout, 0, 1, &m_TexDisplayDescSet, 0, NULL);

		vk.vkCmdBindDynamicViewportState(cmd, dynVP);
		vk.vkCmdBindDynamicRasterState(cmd, m_DynamicRSState);
		vk.vkCmdBindDynamicColorBlendState(cmd, m_DynamicCBStateWhite);
		vk.vkCmdBindDynamicDepthStencilState(cmd, m_DynamicDSStateDisabled);

		vk.vkCmdDraw(cmd, 0, 4, 0, 1);
		vk.vkCmdEndRenderPass(cmd);
	}
	
	VkImageCopy region = {
		{ VK_IMAGE_ASPECT_COLOR, 0, 0}, { 0, 0, 0 },
		{ VK_IMAGE_ASPECT_COLOR, 0, 0}, { 0, 0, 0 },
		{ RDCMIN(fakeBBext.width, outw.width), RDCMIN(fakeBBext.height, outw.height), 1 },
	};
	vk.vkCmdCopyImage(cmd, fakeBBIm, VK_IMAGE_LAYOUT_TRANSFER_SOURCE_OPTIMAL, outw.colimg[outw.curidx], VK_IMAGE_LAYOUT_TRANSFER_DESTINATION_OPTIMAL, 1, &region);
	
	fakeTrans.newLayout = VK_IMAGE_LAYOUT_PRESENT_SOURCE_WSI;
	vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1, &barrier);
	
	outw.curcoltrans->newLayout = VK_IMAGE_LAYOUT_PRESENT_SOURCE_WSI;
	vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, false, 1,(void **) &outw.curcoltrans);
	outw.curcoltrans->oldLayout = outw.curcoltrans->newLayout;

	vk.vkEndCommandBuffer(cmd);

	vk.vkQueueSubmit(q, 1, &cmd, VK_NULL_HANDLE);
	
	{
		VkPresentInfoWSI presentInfo = { VK_STRUCTURE_TYPE_QUEUE_PRESENT_INFO_WSI, NULL, 1, &outw.swap, &outw.curidx };

		vk.vkQueuePresentWSI(q, &presentInfo);

		vk.vkQueueWaitIdle(q);
	}

	vk.vkDeviceWaitIdle(dev);

	vk.vkDestroyDynamicViewportState(dev, dynVP);
}

void VulkanReplay::DestroyOutputWindow(uint64_t id)
{
	auto it = m_OutputWindows.find(id);
	if(id == 0 || it == m_OutputWindows.end())
		return;

	OutputWindow &outw = it->second;

	const VulkanFunctions &vk = m_pDriver->m_Real;
	VkDevice device = m_pDriver->GetDev();

	for(size_t i=0; i < ARRAY_COUNT(outw.colimg); i++)
	{
		if(outw.colimg[i] != VK_NULL_HANDLE)
		{
			vk.vkDestroyAttachmentView(device, outw.colview[i]);
		}
	}

	if(outw.dsimg != VK_NULL_HANDLE)
	{
		vk.vkDestroyAttachmentView(device, outw.dsview);
		vk.vkDestroyImage(device, outw.dsimg);
		vk.vkFreeMemory(device, outw.dsmem);
	}

	vk.vkDestroySwapChainWSI(device, outw.swap);

	m_OutputWindows.erase(it);
}
	
uint64_t VulkanReplay::MakeOutputWindow(void *wn, bool depth)
{
	uint64_t id = m_OutputWinID;
	m_OutputWinID++;

	m_OutputWindows[id].SetWindowHandle(wn);

	if(wn != NULL)
	{
		int32_t w, h;
		GetOutputWindowDimensions(id, w, h);

		m_OutputWindows[id].width = w;
		m_OutputWindows[id].height = h;
		
		m_OutputWindows[id].MakeTargets(m_pDriver->m_Real, m_pDriver->GetDev(), depth);
	}

	return id;
}

vector<byte> VulkanReplay::GetBufferData(ResourceId buff, uint32_t offset, uint32_t len)
{
	RDCUNIMPLEMENTED("GetBufferData");
	return vector<byte>();
}

bool VulkanReplay::IsRenderOutput(ResourceId id)
{
	RDCUNIMPLEMENTED("IsRenderOutput");
	return false;
}

void VulkanReplay::FileChanged()
{
}

FetchTexture VulkanReplay::GetTexture(ResourceId id)
{
	VULKANNOTIMP("GetTexture");

	FetchTexture ret;
	ret.arraysize = 1;
	ret.byteSize = 1280*720*4;
	ret.creationFlags = eTextureCreate_SwapBuffer|eTextureCreate_SRV|eTextureCreate_RTV;
	ret.cubemap = false;
	ret.customName = false;
	ret.depth = 1;
	ret.width = 1280;
	ret.height = 720;
	ret.dimension = 2;
	ret.ID = id;
	ret.mips = 1;
	ret.msQual = 0;
	ret.msSamp = 1;
	ret.name = "WSI Presentable Image";
	ret.numSubresources = 1;

	ret.format.compByteWidth = 1;
	ret.format.compCount = 4;
	ret.format.compType = eCompType_UNorm;
	ret.format.rawType = 0;
	ret.format.special = false;
	ret.format.specialFormat = eSpecial_Unknown;
	ret.format.srgbCorrected = false;
	ret.format.strname = "B8G8R8A8_UNORM";
	return ret;
}

FetchBuffer VulkanReplay::GetBuffer(ResourceId id)
{
	RDCUNIMPLEMENTED("GetBuffer");
	return FetchBuffer();
}

ShaderReflection *VulkanReplay::GetShader(ResourceId id)
{
	RDCUNIMPLEMENTED("GetShader");
	return NULL;
}

void VulkanReplay::SavePipelineState()
{
	VULKANNOTIMP("SavePipelineState");

	{
		create_array_uninit(m_D3D11PipelineState.m_OM.RenderTargets, 1);

		ResourceId id;
		VkImage fakeBBIm = VK_NULL_HANDLE;
		VkExtent3D fakeBBext;
		m_pDriver->GetFakeBB(id, fakeBBIm, fakeBBext);

		m_D3D11PipelineState.m_OM.RenderTargets[0].Resource = id;
	}

	{
		const WrappedVulkan::PartialReplayData::StateVector &state = m_pDriver->m_PartialReplayData.state;
		VulkanCreationInfo &c = m_pDriver->m_CreationInfo;

		m_VulkanPipelineState = VulkanPipelineState();
		
		// General pipeline properties
		m_VulkanPipelineState.compute.obj = state.compute.pipeline;
		m_VulkanPipelineState.graphics.obj = state.graphics.pipeline;

		if(state.compute.pipeline != ResourceId())
			m_VulkanPipelineState.compute.flags = c.m_Pipeline[state.compute.pipeline].flags;

		if(state.graphics.pipeline != ResourceId())
		{
			const VulkanCreationInfo::Pipeline &p = c.m_Pipeline[state.graphics.pipeline];

			m_VulkanPipelineState.graphics.flags = p.flags;

			// Input Assembly
			m_VulkanPipelineState.IA.ibuffer.buf = state.ibuffer.buf;
			m_VulkanPipelineState.IA.ibuffer.offs = state.ibuffer.offs;
			m_VulkanPipelineState.IA.primitiveRestartEnable = p.primitiveRestartEnable;

			// Vertex Input
			create_array_uninit(m_VulkanPipelineState.VI.attrs, p.vertexAttrs.size());
			for(size_t i=0; i < p.vertexAttrs.size(); i++)
			{
				m_VulkanPipelineState.VI.attrs[i].location = p.vertexAttrs[i].location;
				m_VulkanPipelineState.VI.attrs[i].binding = p.vertexAttrs[i].binding;
				m_VulkanPipelineState.VI.attrs[i].byteoffset = p.vertexAttrs[i].byteoffset;
				m_VulkanPipelineState.VI.attrs[i].format = MakeResourceFormat(p.vertexAttrs[i].format);
			}

			create_array_uninit(m_VulkanPipelineState.VI.binds, p.vertexBindings.size());
			for(size_t i=0; i < p.vertexBindings.size(); i++)
			{
				m_VulkanPipelineState.VI.binds[i].bytestride = p.vertexBindings[i].bytestride;
				m_VulkanPipelineState.VI.binds[i].vbufferBinding = p.vertexBindings[i].vbufferBinding;
				m_VulkanPipelineState.VI.binds[i].perInstance = p.vertexBindings[i].perInstance;
			}

			create_array_uninit(m_VulkanPipelineState.VI.vbuffers, state.vbuffers.size());
			for(size_t i=0; i < state.vbuffers.size(); i++)
			{
				m_VulkanPipelineState.VI.vbuffers[i].buffer = state.vbuffers[i].buf;
				m_VulkanPipelineState.VI.vbuffers[i].offset = state.vbuffers[i].offs;
			}

			// Shader Stages
			VulkanPipelineState::ShaderStage *stages[] = {
				&m_VulkanPipelineState.VS,
				&m_VulkanPipelineState.TCS,
				&m_VulkanPipelineState.TES,
				&m_VulkanPipelineState.GS,
				&m_VulkanPipelineState.FS,
				&m_VulkanPipelineState.CS,
			};

			for(size_t i=0; i < ARRAY_COUNT(stages); i++)
			{
				stages[i]->Shader = p.shaders[i];
				stages[i]->ShaderDetails = NULL;
				stages[i]->customName = false;
				stages[i]->ShaderName = StringFormat::Fmt("Shader %llu", p.shaders[i]);
				stages[i]->stage = ShaderStageType(eShaderStage_Vertex + i);
			}

			// Tessellation
			m_VulkanPipelineState.Tess.numControlPoints = p.patchControlPoints;

			// Viewport/Scissors
			m_VulkanPipelineState.VP.state = state.dynamicVP;
			create_array_uninit(m_VulkanPipelineState.VP.viewportScissors, c.m_VPScissor[state.dynamicVP].viewports.size());
			for(size_t i=0; i < c.m_VPScissor[state.dynamicVP].viewports.size(); i++)
			{
				m_VulkanPipelineState.VP.viewportScissors[i].vp.x = c.m_VPScissor[state.dynamicVP].viewports[i].originX;
				m_VulkanPipelineState.VP.viewportScissors[i].vp.y = c.m_VPScissor[state.dynamicVP].viewports[i].originY;
				m_VulkanPipelineState.VP.viewportScissors[i].vp.width = c.m_VPScissor[state.dynamicVP].viewports[i].width;
				m_VulkanPipelineState.VP.viewportScissors[i].vp.height = c.m_VPScissor[state.dynamicVP].viewports[i].height;
				m_VulkanPipelineState.VP.viewportScissors[i].vp.minDepth = c.m_VPScissor[state.dynamicVP].viewports[i].minDepth;
				m_VulkanPipelineState.VP.viewportScissors[i].vp.maxDepth = c.m_VPScissor[state.dynamicVP].viewports[i].maxDepth;

				m_VulkanPipelineState.VP.viewportScissors[i].scissor.x = c.m_VPScissor[state.dynamicVP].scissors[i].offset.x;
				m_VulkanPipelineState.VP.viewportScissors[i].scissor.y = c.m_VPScissor[state.dynamicVP].scissors[i].offset.y;
				m_VulkanPipelineState.VP.viewportScissors[i].scissor.width = c.m_VPScissor[state.dynamicVP].scissors[i].extent.width;
				m_VulkanPipelineState.VP.viewportScissors[i].scissor.height = c.m_VPScissor[state.dynamicVP].scissors[i].extent.height;
			}

			// Rasterizer
			m_VulkanPipelineState.RS.depthClipEnable = p.depthClipEnable;
			m_VulkanPipelineState.RS.rasterizerDiscardEnable = p.rasterizerDiscardEnable;
			m_VulkanPipelineState.RS.FrontCCW = p.frontFace == VK_FRONT_FACE_CCW;

			switch(p.fillMode)
			{
				case VK_FILL_MODE_POINTS:    m_VulkanPipelineState.RS.FillMode = eFill_Point;     break;
				case VK_FILL_MODE_WIREFRAME: m_VulkanPipelineState.RS.FillMode = eFill_Wireframe; break;
				case VK_FILL_MODE_SOLID:     m_VulkanPipelineState.RS.FillMode = eFill_Solid;     break;
				default:
					m_VulkanPipelineState.RS.FillMode = eFill_Solid;
					RDCERR("Unexpected value for FillMode %x", p.fillMode);
					break;
			}

			switch(p.cullMode)
			{
				case VK_CULL_MODE_NONE:           m_VulkanPipelineState.RS.CullMode = eCull_None;         break;
				case VK_CULL_MODE_FRONT:          m_VulkanPipelineState.RS.CullMode = eCull_Front;        break;
				case VK_CULL_MODE_BACK:           m_VulkanPipelineState.RS.CullMode = eCull_Back;         break;
				case VK_CULL_MODE_FRONT_AND_BACK: m_VulkanPipelineState.RS.CullMode = eCull_FrontAndBack; break;
				default:
					m_VulkanPipelineState.RS.CullMode = eCull_None;
					RDCERR("Unexpected value for CullMode %x", p.cullMode);
					break;
			}

			m_VulkanPipelineState.RS.state = state.dynamicRS;
			m_VulkanPipelineState.RS.depthBias = c.m_Raster[state.dynamicRS].depthBias;
			m_VulkanPipelineState.RS.depthBiasClamp = c.m_Raster[state.dynamicRS].depthBiasClamp;
			m_VulkanPipelineState.RS.slopeScaledDepthBias = c.m_Raster[state.dynamicRS].slopeScaledDepthBias;
			m_VulkanPipelineState.RS.lineWidth = c.m_Raster[state.dynamicRS].lineWidth;

			// MSAA
			m_VulkanPipelineState.MSAA.rasterSamples = p.rasterSamples;
			m_VulkanPipelineState.MSAA.sampleShadingEnable = p.sampleShadingEnable;
			m_VulkanPipelineState.MSAA.minSampleShading = p.minSampleShading;
			m_VulkanPipelineState.MSAA.sampleMask = p.sampleMask;

			// Color Blend
			m_VulkanPipelineState.CB.logicOpEnable = p.logicOpEnable;
			m_VulkanPipelineState.CB.alphaToCoverageEnable = p.alphaToCoverageEnable;
			m_VulkanPipelineState.CB.logicOp = ToStr::Get(p.logicOp);

			create_array_uninit(m_VulkanPipelineState.CB.attachments, p.attachments.size());
			for(size_t i=0; i < p.attachments.size(); i++)
			{
				m_VulkanPipelineState.CB.attachments[i].blendEnable = p.attachments[i].blendEnable;

				m_VulkanPipelineState.CB.attachments[i].blend.Source = ToStr::Get(p.attachments[i].blend.Source);
				m_VulkanPipelineState.CB.attachments[i].blend.Destination = ToStr::Get(p.attachments[i].blend.Destination);
				m_VulkanPipelineState.CB.attachments[i].blend.Operation = ToStr::Get(p.attachments[i].blend.Operation);

				m_VulkanPipelineState.CB.attachments[i].alphaBlend.Source = ToStr::Get(p.attachments[i].alphaBlend.Source);
				m_VulkanPipelineState.CB.attachments[i].alphaBlend.Destination = ToStr::Get(p.attachments[i].alphaBlend.Destination);
				m_VulkanPipelineState.CB.attachments[i].alphaBlend.Operation = ToStr::Get(p.attachments[i].alphaBlend.Operation);

				m_VulkanPipelineState.CB.attachments[i].writeMask = p.attachments[i].channelWriteMask;
			}

			m_VulkanPipelineState.CB.state = state.dynamicCB;
			memcpy(m_VulkanPipelineState.CB.blendConst, c.m_Blend[state.dynamicCB].blendConst, sizeof(float)*4);

			// Depth Stencil
			m_VulkanPipelineState.DS.depthTestEnable = p.depthTestEnable;
			m_VulkanPipelineState.DS.depthWriteEnable = p.depthWriteEnable;
			m_VulkanPipelineState.DS.depthBoundsEnable = p.depthBoundsEnable;
			m_VulkanPipelineState.DS.depthCompareOp = ToStr::Get(p.depthCompareOp);
			m_VulkanPipelineState.DS.stencilTestEnable = p.stencilTestEnable;

			m_VulkanPipelineState.DS.front.passOp = ToStr::Get(p.front.stencilPassOp);
			m_VulkanPipelineState.DS.front.failOp = ToStr::Get(p.front.stencilFailOp);
			m_VulkanPipelineState.DS.front.depthFailOp = ToStr::Get(p.front.stencilDepthFailOp);
			m_VulkanPipelineState.DS.front.func = ToStr::Get(p.front.stencilCompareOp);

			m_VulkanPipelineState.DS.back.passOp = ToStr::Get(p.back.stencilPassOp);
			m_VulkanPipelineState.DS.back.failOp = ToStr::Get(p.back.stencilFailOp);
			m_VulkanPipelineState.DS.back.depthFailOp = ToStr::Get(p.back.stencilDepthFailOp);
			m_VulkanPipelineState.DS.back.func = ToStr::Get(p.back.stencilCompareOp);

			m_VulkanPipelineState.DS.state = state.dynamicDS;
			m_VulkanPipelineState.DS.minDepthBounds = c.m_DepthStencil[state.dynamicDS].minDepthBounds;
			m_VulkanPipelineState.DS.maxDepthBounds = c.m_DepthStencil[state.dynamicDS].maxDepthBounds;

			m_VulkanPipelineState.DS.front.ref = c.m_DepthStencil[state.dynamicDS].stencilFrontRef;
			m_VulkanPipelineState.DS.back.ref = c.m_DepthStencil[state.dynamicDS].stencilBackRef;

			m_VulkanPipelineState.DS.stencilReadMask = c.m_DepthStencil[state.dynamicDS].stencilReadMask;
			m_VulkanPipelineState.DS.stencilWriteMask = c.m_DepthStencil[state.dynamicDS].stencilWriteMask;

			// Renderpass
			m_VulkanPipelineState.Pass.renderpass.obj = state.renderPass;
			m_VulkanPipelineState.Pass.framebuffer.obj = state.framebuffer;

			m_VulkanPipelineState.Pass.framebuffer.width = c.m_Framebuffer[state.framebuffer].width;
			m_VulkanPipelineState.Pass.framebuffer.height = c.m_Framebuffer[state.framebuffer].height;
			m_VulkanPipelineState.Pass.framebuffer.layers = c.m_Framebuffer[state.framebuffer].layers;

			create_array_uninit(m_VulkanPipelineState.Pass.framebuffer.attachments, c.m_Framebuffer[state.framebuffer].attachments.size());
			for(size_t i=0; i < c.m_Framebuffer[state.framebuffer].attachments.size(); i++)
			{
				m_VulkanPipelineState.Pass.framebuffer.attachments[i].view = 
					c.m_Framebuffer[state.framebuffer].attachments[i].view;
			}

			m_VulkanPipelineState.Pass.renderArea.x = state.renderArea.offset.x;
			m_VulkanPipelineState.Pass.renderArea.y = state.renderArea.offset.y;
			m_VulkanPipelineState.Pass.renderArea.width = state.renderArea.extent.width;
			m_VulkanPipelineState.Pass.renderArea.height = state.renderArea.extent.height;
		}
	}
}

void VulkanReplay::FillCBufferVariables(ResourceId shader, uint32_t cbufSlot, vector<ShaderVariable> &outvars, const vector<byte> &data)
{
	RDCUNIMPLEMENTED("FillCBufferVariables");
}

bool VulkanReplay::GetMinMax(ResourceId texid, uint32_t sliceFace, uint32_t mip, uint32_t sample, float *minval, float *maxval)
{
	RDCUNIMPLEMENTED("GetMinMax");
	return false;
}

bool VulkanReplay::GetHistogram(ResourceId texid, uint32_t sliceFace, uint32_t mip, uint32_t sample, float minval, float maxval, bool channels[4], vector<uint32_t> &histogram)
{
	RDCUNIMPLEMENTED("GetHistogram");
	return false;
}

void VulkanReplay::InitPostVSBuffers(uint32_t frameID, uint32_t eventID)
{
	VULKANNOTIMP("VulkanReplay::InitPostVSBuffers");
}

vector<EventUsage> VulkanReplay::GetUsage(ResourceId id)
{
	VULKANNOTIMP("GetUsage");
	return vector<EventUsage>();
}

void VulkanReplay::SetContextFilter(ResourceId id, uint32_t firstDefEv, uint32_t lastDefEv)
{
	RDCUNIMPLEMENTED("SetContextFilter");
}

void VulkanReplay::FreeTargetResource(ResourceId id)
{
	RDCUNIMPLEMENTED("FreeTargetResource");
}

void VulkanReplay::FreeCustomShader(ResourceId id)
{
	RDCUNIMPLEMENTED("FreeCustomShader");
}

MeshFormat VulkanReplay::GetPostVSBuffers(uint32_t frameID, uint32_t eventID, uint32_t instID, MeshDataStage stage)
{
	MeshFormat ret;
	RDCEraseEl(ret);

	VULKANNOTIMP("VulkanReplay::GetPostVSBuffers");

	return ret;
}

byte *VulkanReplay::GetTextureData(ResourceId tex, uint32_t arrayIdx, uint32_t mip, bool resolve, bool forceRGBA8unorm, float blackPoint, float whitePoint, size_t &dataSize)
{
	RDCUNIMPLEMENTED("GetTextureData");
	return NULL;
}

void VulkanReplay::ReplaceResource(ResourceId from, ResourceId to)
{
	RDCUNIMPLEMENTED("ReplaceResource");
}

void VulkanReplay::RemoveReplacement(ResourceId id)
{
	RDCUNIMPLEMENTED("RemoveReplacement");
}

vector<uint32_t> VulkanReplay::EnumerateCounters()
{
	VULKANNOTIMP("EnumerateCounters");
	return vector<uint32_t>();
}

void VulkanReplay::DescribeCounter(uint32_t counterID, CounterDescription &desc)
{
	RDCUNIMPLEMENTED("DescribeCounter");
}

vector<CounterResult> VulkanReplay::FetchCounters(uint32_t frameID, uint32_t minEventID, uint32_t maxEventID, const vector<uint32_t> &counters)
{
	RDCUNIMPLEMENTED("FetchCounters");
	return vector<CounterResult>();
}

void VulkanReplay::BuildTargetShader(string source, string entry, const uint32_t compileFlags, ShaderStageType type, ResourceId *id, string *errors)
{
	RDCUNIMPLEMENTED("BuildTargetShader");
}

void VulkanReplay::BuildCustomShader(string source, string entry, const uint32_t compileFlags, ShaderStageType type, ResourceId *id, string *errors)
{
	VULKANNOTIMP("BuildCustomShader");
}

vector<PixelModification> VulkanReplay::PixelHistory(uint32_t frameID, vector<EventUsage> events, ResourceId target, uint32_t x, uint32_t y, uint32_t slice, uint32_t mip, uint32_t sampleIdx)
{
	RDCUNIMPLEMENTED("VulkanReplay::PixelHistory");
	return vector<PixelModification>();
}

ShaderDebugTrace VulkanReplay::DebugVertex(uint32_t frameID, uint32_t eventID, uint32_t vertid, uint32_t instid, uint32_t idx, uint32_t instOffset, uint32_t vertOffset)
{
	RDCUNIMPLEMENTED("DebugVertex");
	return ShaderDebugTrace();
}

ShaderDebugTrace VulkanReplay::DebugPixel(uint32_t frameID, uint32_t eventID, uint32_t x, uint32_t y, uint32_t sample, uint32_t primitive)
{
	RDCUNIMPLEMENTED("DebugPixel");
	return ShaderDebugTrace();
}

ShaderDebugTrace VulkanReplay::DebugThread(uint32_t frameID, uint32_t eventID, uint32_t groupid[3], uint32_t threadid[3])
{
	RDCUNIMPLEMENTED("DebugThread");
	return ShaderDebugTrace();
}

ResourceId VulkanReplay::ApplyCustomShader(ResourceId shader, ResourceId texid, uint32_t mip)
{
	RDCUNIMPLEMENTED("ApplyCustomShader");
	return ResourceId();
}

ResourceId VulkanReplay::CreateProxyTexture( FetchTexture templateTex )
{
	RDCUNIMPLEMENTED("CreateProxyTexture");
	return ResourceId();
}

void VulkanReplay::SetProxyTextureData(ResourceId texid, uint32_t arrayIdx, uint32_t mip, byte *data, size_t dataSize)
{
	RDCUNIMPLEMENTED("SetProxyTextureData");
}

ResourceId VulkanReplay::CreateProxyBuffer(FetchBuffer templateBuf)
{
	RDCUNIMPLEMENTED("CreateProxyBuffer");
	return ResourceId();
}

void VulkanReplay::SetProxyBufferData(ResourceId bufid, byte *data, size_t dataSize)
{
	RDCUNIMPLEMENTED("SetProxyTextureData");
}

const VulkanFunctions &GetRealVKFunctions();

ReplayCreateStatus Vulkan_CreateReplayDevice(const char *logfile, IReplayDriver **driver)
{
	RDCDEBUG("Creating a VulkanReplay replay device");
	
#if defined(WIN32)
	bool loaded = Process::LoadModule("vulkan.0.dll");
#elif defined(__linux__)
	bool loaded = Process::LoadModule("libvulkan.so");
#else
#error "Unknown platform"
#endif
	if(!loaded)
	{
		RDCERR("Failed to load vulkan library");
		return eReplayCreate_APIInitFailed;
	}
	
	VkInitParams initParams;
	RDCDriver driverType = RDC_Vulkan;
	string driverName = "VulkanReplay";
	if(logfile)
		RenderDoc::Inst().FillInitParams(logfile, driverType, driverName, (RDCInitParams *)&initParams);
	
	if(initParams.SerialiseVersion != VkInitParams::VK_SERIALISE_VERSION)
	{
		RDCERR("Incompatible VulkanReplay serialise version, expected %d got %d", VkInitParams::VK_SERIALISE_VERSION, initParams.SerialiseVersion);
		return eReplayCreate_APIIncompatibleVersion;
	}
	
	WrappedVulkan *vk = new WrappedVulkan(GetRealVKFunctions(), logfile);
	vk->Initialise(initParams);
	
	RDCLOG("Created device.");
	VulkanReplay *replay = vk->GetReplay();
	replay->SetProxy(logfile == NULL);

	*driver = (IReplayDriver *)replay;

	return eReplayCreate_Success;
}

static DriverRegistration VkDriverRegistration(RDC_Vulkan, "Vulkan", &Vulkan_CreateReplayDevice);
