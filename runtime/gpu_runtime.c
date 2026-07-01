/* In-binary Vulkan compute dispatcher for `@gpu` maps — the production sibling of tests/gpu/vk_run.c.
 * A `--gpu` build links this object plus a generated shader registry (codegen/gpu_embed.c); codegen
 * emits `arche_gpu_dispatch(name, ...)` for each `run map @gpu`, falling back to the CPU map on any
 * nonzero return. So GPU is a best-effort accelerator: a missing device, driver, or shader degrades
 * to the identical CPU result (and, with `float`=f32 everywhere, bit-for-bit identical).
 *
 * Design: a Vulkan device is created lazily on the first dispatch and cached for the process. Compute
 * pipelines (the expensive part) are cached per shader name. Column buffers use host-visible/coherent
 * memory (no staging copy) and are created+freed per dispatch — correctness first; persistent
 * device-local residency is a later optimization. Submit is synchronous (`vkQueueWaitIdle`).
 *
 * When the build machine has no <vulkan/vulkan.h>, ARCHE_HAVE_VULKAN is undefined and this compiles to
 * a stub that always reports failure, so the executable still links and runs (on the CPU). */

#include "arche_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARCHE_HAVE_VULKAN)

#include <stdint.h>
#include <vulkan/vulkan.h>

#define ARCHE_GPU_MAX_COL 16
#define ARCHE_GPU_MAX_CACHE 64
#define ARCHE_GPU_MAX_RESIDENT 256

/* One-time device state. `ready` is 0 = uninitialized, 1 = up, -1 = init failed (never retry). */
static struct {
	int ready;
	VkInstance inst;
	VkPhysicalDevice pd;
	VkDevice dev;
	VkQueue queue;
	uint32_t qfam;
	VkCommandPool cpool;
	char dev_name[256];
} G = {0};

/* A cached compute pipeline for one shader (keyed by map name). */
typedef struct {
	const char *name;
	VkShaderModule sm;
	VkDescriptorSetLayout dsl;
	VkPipelineLayout playout;
	VkPipeline pipe;
	unsigned ncol;
} PipeEntry;
static PipeEntry g_pipes[ARCHE_GPU_MAX_CACHE];
static int g_pipe_count = 0;

/* Persistent (GPU-resident) column buffers, keyed by the host column pointer. A `@resident` pool's
 * columns are created+uploaded once and reused across dispatches (no per-dispatch alloc/upload/download);
 * `arche_gpu_sync` downloads the dirty ones back to host. Static pools have stable host bases + fixed
 * size, so a host-pointer key with a fixed `bytes` is sufficient. */
typedef struct {
	void *host;
	VkBuffer buf;
	VkDeviceMemory mem;
	void *mapped;
	VkDeviceSize bytes;
	int gpu_dirty; /* 1 = GPU wrote since the last sync; host copy is stale */
} ResidentBuf;
static ResidentBuf g_resident[ARCHE_GPU_MAX_RESIDENT];
static int g_resident_count = 0;

static ResidentBuf *resident_find(void *host) {
	for (int i = 0; i < g_resident_count; i++)
		if (g_resident[i].host == host)
			return &g_resident[i];
	return NULL;
}

static int gpu_debug(void) {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("ARCHE_GPU_DEBUG");
		v = (e && *e && *e != '0') ? 1 : 0;
	}
	return v;
}

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(G.pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return UINT32_MAX;
}

/* Bring up instance + a compute-capable device once. Returns 0 on success. */
static int gpu_init(void) {
	if (G.ready)
		return G.ready == 1 ? 0 : 1;
	G.ready = -1; /* assume failure until proven otherwise — never retried */

	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	if (vkCreateInstance(&ici, NULL, &G.inst) != VK_SUCCESS)
		return 1;

	uint32_t npd = 0;
	vkEnumeratePhysicalDevices(G.inst, &npd, NULL);
	if (npd == 0)
		return 1;
	if (npd > 8)
		npd = 8;
	VkPhysicalDevice pds[8];
	vkEnumeratePhysicalDevices(G.inst, &npd, pds);
	G.pd = pds[0];

	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(G.pd, &nqf, NULL);
	if (nqf > 32)
		nqf = 32;
	VkQueueFamilyProperties qfs[32];
	vkGetPhysicalDeviceQueueFamilyProperties(G.pd, &nqf, qfs);
	int found = 0;
	for (uint32_t i = 0; i < nqf; i++)
		if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
			G.qfam = i;
			found = 1;
			break;
		}
	if (!found)
		return 1;

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	                               .queueFamilyIndex = G.qfam,
	                               .queueCount = 1,
	                               .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
	if (vkCreateDevice(G.pd, &dci, NULL, &G.dev) != VK_SUCCESS)
		return 1;
	vkGetDeviceQueue(G.dev, G.qfam, 0, &G.queue);

	VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	                                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                                .queueFamilyIndex = G.qfam};
	if (vkCreateCommandPool(G.dev, &cpci, NULL, &G.cpool) != VK_SUCCESS)
		return 1;

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(G.pd, &props);
	snprintf(G.dev_name, sizeof(G.dev_name), "%s", props.deviceName);

	G.ready = 1;
	return 0;
}

/* Build (or fetch from cache) the compute pipeline for a shader. Returns NULL on failure. */
static PipeEntry *gpu_pipeline(const ArcheGpuShader *sh) {
	for (int i = 0; i < g_pipe_count; i++)
		if (g_pipes[i].name == sh->name || strcmp(g_pipes[i].name, sh->name) == 0)
			return &g_pipes[i];
	if (g_pipe_count >= ARCHE_GPU_MAX_CACHE || sh->ncol == 0 || sh->ncol > ARCHE_GPU_MAX_COL)
		return NULL;

	PipeEntry e = {0};
	e.name = sh->name;
	e.ncol = sh->ncol;

	VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                 .codeSize = sh->spv_len,
	                                 .pCode = (const uint32_t *)sh->spv};
	if (vkCreateShaderModule(G.dev, &smci, NULL, &e.sm) != VK_SUCCESS)
		return NULL;

	VkDescriptorSetLayoutBinding dslb[ARCHE_GPU_MAX_COL];
	for (unsigned b = 0; b < sh->ncol; b++)
		dslb[b] = (VkDescriptorSetLayoutBinding){.binding = b,
		                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		                                         .descriptorCount = 1,
		                                         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
	VkDescriptorSetLayoutCreateInfo dslci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = sh->ncol, .pBindings = dslb};
	if (vkCreateDescriptorSetLayout(G.dev, &dslci, NULL, &e.dsl) != VK_SUCCESS) {
		vkDestroyShaderModule(G.dev, e.sm, NULL);
		return NULL;
	}

	VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(uint32_t)};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                   .setLayoutCount = 1,
	                                   .pSetLayouts = &e.dsl,
	                                   .pushConstantRangeCount = 1,
	                                   .pPushConstantRanges = &pcr};
	if (vkCreatePipelineLayout(G.dev, &plci, NULL, &e.playout) != VK_SUCCESS) {
		vkDestroyDescriptorSetLayout(G.dev, e.dsl, NULL);
		vkDestroyShaderModule(G.dev, e.sm, NULL);
		return NULL;
	}

	VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	                                    .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                              .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	                                              .module = e.sm,
	                                              .pName = "main"},
	                                    .layout = e.playout};
	if (vkCreateComputePipelines(G.dev, VK_NULL_HANDLE, 1, &cpci, NULL, &e.pipe) != VK_SUCCESS) {
		vkDestroyPipelineLayout(G.dev, e.playout, NULL);
		vkDestroyDescriptorSetLayout(G.dev, e.dsl, NULL);
		vkDestroyShaderModule(G.dev, e.sm, NULL);
		return NULL;
	}

	g_pipes[g_pipe_count] = e;
	return &g_pipes[g_pipe_count++];
}

/* Copy between a host column and a DEVICE_LOCAL buffer through a temporary HOST_VISIBLE staging buffer
 * (device-local VRAM is not host-mappable). to_device=1 uploads host->dev; to_device=0 downloads dev->host.
 * Used to populate/read back GPU-resident buffers once, so the kernel runs against VRAM bandwidth (much
 * higher than PCIe-mapped host memory). Returns 0 on success. */
static int gpu_staged_copy(void *host, VkBuffer dev, VkDeviceSize bytes, int to_device) {
	int rc = 1;
	VkBuffer stg = VK_NULL_HANDLE;
	VkDeviceMemory smem = VK_NULL_HANDLE;
	void *mapped = NULL;
	VkCommandBuffer cmd = VK_NULL_HANDLE;

	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                          .size = bytes,
	                          .usage = to_device ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vkCreateBuffer(G.dev, &bci, NULL, &stg) != VK_SUCCESS)
		goto out;
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(G.dev, stg, &mr);
	uint32_t mt = find_mem_type(mr.memoryTypeBits,
	                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mt == UINT32_MAX)
		goto out;
	VkMemoryAllocateInfo mai = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
	if (vkAllocateMemory(G.dev, &mai, NULL, &smem) != VK_SUCCESS)
		goto out;
	if (vkBindBufferMemory(G.dev, stg, smem, 0) != VK_SUCCESS)
		goto out;
	if (vkMapMemory(G.dev, smem, 0, bytes, 0, &mapped) != VK_SUCCESS)
		goto out;
	if (to_device)
		memcpy(mapped, host, (size_t)bytes);

	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = G.cpool,
	                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	if (vkAllocateCommandBuffers(G.dev, &cbai, &cmd) != VK_SUCCESS)
		goto out;
	VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (vkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS)
		goto out;
	VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0, .size = bytes};
	if (to_device)
		vkCmdCopyBuffer(cmd, stg, dev, 1, &region);
	else
		vkCmdCopyBuffer(cmd, dev, stg, 1, &region);
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		goto out;
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	if (vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
		goto out;
	if (vkQueueWaitIdle(G.queue) != VK_SUCCESS)
		goto out;
	if (!to_device)
		memcpy(host, mapped, (size_t)bytes);
	rc = 0;

out:
	if (cmd)
		vkFreeCommandBuffers(G.dev, G.cpool, 1, &cmd);
	if (mapped)
		vkUnmapMemory(G.dev, smem);
	if (stg)
		vkDestroyBuffer(G.dev, stg, NULL);
	if (smem)
		vkFreeMemory(G.dev, smem, NULL);
	return rc;
}

int arche_gpu_dispatch(const char *name, unsigned ncol, void **cols, unsigned elem_size, unsigned count,
                       int resident) {
	if (!name || ncol == 0 || ncol > ARCHE_GPU_MAX_COL || !cols)
		return 1;
	if (count == 0)
		return 0; /* nothing to do — trivially "ran" */

	const ArcheGpuShader *sh = arche_gpu_lookup(name);
	if (!sh || sh->ncol != ncol)
		return 1; /* no embedded shader (e.g. no glslc at build) → CPU */
	if (gpu_init() != 0)
		return 1;
	PipeEntry *pe = gpu_pipeline(sh);
	if (!pe)
		return 1;

	VkDeviceSize bytes = (VkDeviceSize)elem_size * count;
	VkBuffer buf[ARCHE_GPU_MAX_COL] = {0};
	VkDeviceMemory mem[ARCHE_GPU_MAX_COL] = {0};
	void *mapped[ARCHE_GPU_MAX_COL] = {0};
	int from_cache[ARCHE_GPU_MAX_COL] = {0}; /* 1 = buffer owned by the resident cache (not freed here) */
	VkDescriptorPool dpool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	int rc = 1;

	for (unsigned b = 0; b < ncol; b++) {
		/* Resident path: a DEVICE_LOCAL (VRAM) buffer that persists across dispatches. On a hit, reuse it
		 * with no transfer — the kernel reads/writes VRAM at full device bandwidth. On a miss, create the
		 * VRAM buffer and stage-upload the column once. */
		if (resident && g_resident_count < ARCHE_GPU_MAX_RESIDENT) {
			ResidentBuf *r = resident_find(cols[b]);
			if (r && r->bytes == bytes) {
				buf[b] = r->buf;
				mem[b] = r->mem;
				from_cache[b] = 1;
				if (gpu_debug())
					fprintf(stderr, "arche: gpu resident reuse '%s' col %u (%zu bytes, VRAM)\n", name, b,
					        (size_t)bytes);
				continue;
			}
			VkBufferCreateInfo dbci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			                           .size = bytes,
			                           .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
			                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
			if (vkCreateBuffer(G.dev, &dbci, NULL, &buf[b]) != VK_SUCCESS)
				goto done;
			VkMemoryRequirements dmr;
			vkGetBufferMemoryRequirements(G.dev, buf[b], &dmr);
			uint32_t dmt = find_mem_type(dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (dmt == UINT32_MAX)
				goto done;
			VkMemoryAllocateInfo dmai = {
			    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = dmr.size, .memoryTypeIndex = dmt};
			if (vkAllocateMemory(G.dev, &dmai, NULL, &mem[b]) != VK_SUCCESS)
				goto done;
			if (vkBindBufferMemory(G.dev, buf[b], mem[b], 0) != VK_SUCCESS)
				goto done;
			if (gpu_staged_copy(cols[b], buf[b], bytes, /*to_device=*/1) != 0)
				goto done; /* upload once via staging */
			ResidentBuf *nr = &g_resident[g_resident_count++];
			nr->host = cols[b];
			nr->buf = buf[b];
			nr->mem = mem[b];
			nr->mapped = NULL; /* device-local: not host-mapped */
			nr->bytes = bytes;
			nr->gpu_dirty = 0;
			from_cache[b] = 1;
			if (gpu_debug())
				fprintf(stderr, "arche: gpu resident upload '%s' col %u (%zu bytes, VRAM)\n", name, b, (size_t)bytes);
			continue;
		}
		/* Non-resident (or resident cache full): per-dispatch HOST_VISIBLE buffer, mapped + uploaded now,
		 * downloaded and freed after the dispatch. */
		VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		                          .size = bytes,
		                          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
		if (vkCreateBuffer(G.dev, &bci, NULL, &buf[b]) != VK_SUCCESS)
			goto done;
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(G.dev, buf[b], &mr);
		uint32_t mt = find_mem_type(mr.memoryTypeBits,
		                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mt == UINT32_MAX)
			goto done;
		VkMemoryAllocateInfo mai = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
		if (vkAllocateMemory(G.dev, &mai, NULL, &mem[b]) != VK_SUCCESS)
			goto done;
		if (vkBindBufferMemory(G.dev, buf[b], mem[b], 0) != VK_SUCCESS)
			goto done;
		if (vkMapMemory(G.dev, mem[b], 0, bytes, 0, &mapped[b]) != VK_SUCCESS)
			goto done;
		memcpy(mapped[b], cols[b], (size_t)bytes); /* upload this column */
	}

	VkDescriptorPoolSize dps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = ncol};
	VkDescriptorPoolCreateInfo dpci = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
	if (vkCreateDescriptorPool(G.dev, &dpci, NULL, &dpool) != VK_SUCCESS)
		goto done;
	VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	                                    .descriptorPool = dpool,
	                                    .descriptorSetCount = 1,
	                                    .pSetLayouts = &pe->dsl};
	VkDescriptorSet dset;
	if (vkAllocateDescriptorSets(G.dev, &dsai, &dset) != VK_SUCCESS)
		goto done;
	VkDescriptorBufferInfo dbi[ARCHE_GPU_MAX_COL];
	VkWriteDescriptorSet wds[ARCHE_GPU_MAX_COL];
	for (unsigned b = 0; b < ncol; b++) {
		dbi[b] = (VkDescriptorBufferInfo){.buffer = buf[b], .offset = 0, .range = bytes};
		wds[b] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		                                .dstSet = dset,
		                                .dstBinding = b,
		                                .descriptorCount = 1,
		                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		                                .pBufferInfo = &dbi[b]};
	}
	vkUpdateDescriptorSets(G.dev, ncol, wds, 0, NULL);

	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	                                    .commandPool = G.cpool,
	                                    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = 1};
	if (vkAllocateCommandBuffers(G.dev, &cbai, &cmd) != VK_SUCCESS)
		goto done;
	VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (vkBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS)
		goto done;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pe->pipe);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pe->playout, 0, 1, &dset, 0, NULL);
	vkCmdPushConstants(cmd, pe->playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &count);
	vkCmdDispatch(cmd, (count + 63) / 64, 1, 1); /* local_size_x = 64 (matches the emitted shader) */
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		goto done;
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
	if (vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
		goto done;
	if (vkQueueWaitIdle(G.queue) != VK_SUCCESS)
		goto done;

	for (unsigned b = 0; b < ncol; b++) {
		if (from_cache[b]) {
			/* Resident: leave the result in the device buffer (no download); mark stale-on-host so a later
			 * `arche_gpu_sync` brings it back. */
			ResidentBuf *r = resident_find(cols[b]);
			if (r)
				r->gpu_dirty = 1;
		} else {
			memcpy(cols[b], mapped[b], (size_t)bytes); /* per-dispatch: host-coherent read-back */
		}
	}
	rc = 0;
	if (gpu_debug())
		fprintf(stderr, "arche: gpu dispatch '%s' (%u elems, %u cols%s) on %s\n", name, count, ncol,
		        resident ? ", resident" : "", G.dev_name);

done:
	if (cmd)
		vkFreeCommandBuffers(G.dev, G.cpool, 1, &cmd);
	if (dpool)
		vkDestroyDescriptorPool(G.dev, dpool, NULL);
	/* Free every column resource that was created EXCEPT resident-cache-owned buffers (they persist).
	 * The arrays are zero-initialized and each handle is null-guarded, so iterating the full width frees a
	 * buffer even if its memory alloc/map failed mid-loop (a partial-failure leak the old `created` counter
	 * missed). */
	for (unsigned b = 0; b < ncol; b++) {
		if (from_cache[b])
			continue;
		if (mapped[b])
			vkUnmapMemory(G.dev, mem[b]);
		if (buf[b])
			vkDestroyBuffer(G.dev, buf[b], NULL);
		if (mem[b])
			vkFreeMemory(G.dev, mem[b], NULL);
	}
	return rc;
}

/* Download dirty resident buffers back to host (the `gpu.sync(Pool)` leaf). */
void arche_gpu_sync(void **cols, unsigned ncol, unsigned elem_size, unsigned count) {
	(void)elem_size;
	(void)count;
	if (!cols)
		return;
	for (unsigned b = 0; b < ncol; b++) {
		ResidentBuf *r = resident_find(cols[b]);
		if (r && r->gpu_dirty) {
			gpu_staged_copy(cols[b], r->buf, r->bytes, /*to_device=*/0); /* download VRAM -> host via staging */
			r->gpu_dirty = 0;
			if (gpu_debug())
				fprintf(stderr, "arche: gpu sync download col (%zu bytes, VRAM)\n", (size_t)r->bytes);
		}
	}
}

/* Refresh resident buffers FROM host — the mirror of arche_gpu_sync, for a pool the host wrote after it went
 * resident (the runtime uploads a resident buffer only once, at its first dispatch, so a later host write
 * would otherwise be invisible to the GPU). No-op for non-resident columns (they auto-upload at dispatch). */
void arche_gpu_upload(void **cols, unsigned ncol, unsigned elem_size, unsigned count) {
	(void)elem_size;
	(void)count;
	if (!cols)
		return;
	for (unsigned b = 0; b < ncol; b++) {
		ResidentBuf *r = resident_find(cols[b]);
		if (r) {
			gpu_staged_copy(cols[b], r->buf, r->bytes, /*to_device=*/1); /* upload host -> VRAM via staging */
			r->gpu_dirty = 0;                                            /* device now current with host */
			if (gpu_debug())
				fprintf(stderr, "arche: gpu upload col (%zu bytes, VRAM)\n", (size_t)r->bytes);
		}
	}
}

#else /* no <vulkan/vulkan.h> at build time: always fall back to CPU */

int arche_gpu_dispatch(const char *name, unsigned ncol, void **cols, unsigned elem_size, unsigned count,
                       int resident) {
	/* No <vulkan/vulkan.h> at build time: always report failure so every dispatch runs the CPU path.
	 * The `0 *` terms reference each argument (keeping -Wunused-parameter quiet) without altering the
	 * result, which stays a constant nonzero. */
	return 1 + 0 * (name != NULL) + 0 * (int)ncol + 0 * (cols != NULL) + 0 * (int)elem_size + 0 * (int)count +
	       0 * resident;
}

void arche_gpu_sync(void **cols, unsigned ncol, unsigned elem_size, unsigned count) {
	/* No GPU: the CPU-fallback dispatch already wrote host columns, so there is nothing to download. */
	(void)cols;
	(void)ncol;
	(void)elem_size;
	(void)count;
}

void arche_gpu_upload(void **cols, unsigned ncol, unsigned elem_size, unsigned count) {
	/* No GPU: nothing is resident, so there is nothing to refresh on the device. */
	(void)cols;
	(void)ncol;
	(void)elem_size;
	(void)count;
}

#endif
