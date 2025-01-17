#include "mantle_internal.h"
#include "amdilc.h"

#define SETS_PER_POOL   (32)

typedef enum _DirtyFlags {
    FLAG_DIRTY_GRAPHICS_DESCRIPTOR_SETS = 1,
    FLAG_DIRTY_COMPUTE_DESCRIPTOR_SETS = 2,
    FLAG_DIRTY_FRAMEBUFFER = 4,
    FLAG_DIRTY_PIPELINE = 8,
} DirtyFlags;

static VkFramebuffer getVkFramebuffer(
    const GrDevice* grDevice,
    VkRenderPass renderPass,
    unsigned attachmentCount,
    const VkImageView* attachments,
    VkExtent3D extent)
{
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    const VkFramebufferCreateInfo framebufferCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .renderPass = renderPass,
        .attachmentCount = attachmentCount,
        .pAttachments = attachments,
        .width = extent.width,
        .height = extent.height,
        .layers = extent.depth,
    };

    if (VKD.vkCreateFramebuffer(grDevice->device, &framebufferCreateInfo, NULL,
                                &framebuffer) != VK_SUCCESS) {
        LOGE("vkCreateFramebuffer failed\n");
        return VK_NULL_HANDLE;
    }

    return framebuffer;
}

static VkDescriptorPool getVkDescriptorPool(
    const GrDevice* grDevice,
    unsigned stageCount,
    unsigned descriptorTypeCountSize,
    const unsigned* descriptorTypeCounts)
{
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Create pool sizes
    unsigned descriptorPoolSizeCount = 0;
    VkDescriptorPoolSize* descriptorPoolSizes = NULL;
    for (unsigned i = 0; i < descriptorTypeCountSize; i++) {
        if (descriptorTypeCounts[i] > 0) {
            descriptorPoolSizeCount++;
            descriptorPoolSizes = realloc(descriptorPoolSizes,
                                          descriptorPoolSizeCount * sizeof(VkDescriptorPoolSize));
            descriptorPoolSizes[descriptorPoolSizeCount - 1] = (VkDescriptorPoolSize) {
                .type = i,
                .descriptorCount = SETS_PER_POOL * descriptorTypeCounts[i],
            };
        }
    }

    const VkDescriptorPoolCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .maxSets = SETS_PER_POOL * stageCount,
        .poolSizeCount = descriptorPoolSizeCount,
        .pPoolSizes = descriptorPoolSizes,
    };

    VkResult res = VKD.vkCreateDescriptorPool(grDevice->device, &createInfo, NULL, &descriptorPool);
    if (res != VK_SUCCESS) {
        LOGE("vkCreateDescriptorPool failed (%d)\n", res);
        assert(false);
    }

    free(descriptorPoolSizes);
    return descriptorPool;
}

static const DescriptorSetSlot* getDescriptorSetSlot(
    const GrDescriptorSet* grDescriptorSet,
    unsigned slotOffset,
    const GR_DESCRIPTOR_SET_MAPPING* mapping,
    uint32_t bindingIndex)
{
    for (unsigned i = 0; i < mapping->descriptorCount; i++) {
        const GR_DESCRIPTOR_SLOT_INFO* slotInfo = &mapping->pDescriptorInfo[i];
        const DescriptorSetSlot* slot = &grDescriptorSet->slots[slotOffset + i];

        if (slotInfo->slotObjectType == GR_SLOT_UNUSED) {
            continue;
        } else if (slotInfo->slotObjectType == GR_SLOT_NEXT_DESCRIPTOR_SET) {
            if (slot->type == SLOT_TYPE_NONE) {
                continue;
            } else if (slot->type != SLOT_TYPE_NESTED) {
                LOGE("unexpected slot type %d (should be nested)\n", slot->type);
                assert(false);
            }

            const DescriptorSetSlot* nestedSlot =
                getDescriptorSetSlot(slot->nested.nextSet, slot->nested.slotOffset,
                                     slotInfo->pNextLevelSet, bindingIndex);
            if (nestedSlot != NULL) {
                return nestedSlot;
            } else {
                continue;
            }
        }

        uint32_t slotBinding = slotInfo->shaderEntityIndex;
        if (slotInfo->slotObjectType != GR_SLOT_SHADER_SAMPLER) {
            slotBinding += ILC_BASE_RESOURCE_ID;
        }

        if (slotBinding == bindingIndex) {
            return slot;
        }
    }

    return NULL;
}

static void updateVkDescriptorSet(
    const GrDevice* grDevice,
    GrCmdBuffer* grCmdBuffer,
    VkDescriptorSet vkDescriptorSet,
    unsigned slotOffset,
    const GR_PIPELINE_SHADER* shaderInfo,
    const GrDescriptorSet* grDescriptorSet,
    const DescriptorSetSlot* dynamicMemoryView)
{
    const GrShader* grShader = (GrShader*)shaderInfo->shader;
    const GR_DYNAMIC_MEMORY_VIEW_SLOT_INFO* dynamicMapping = &shaderInfo->dynamicMemoryViewMapping;
    VkResult vkRes;

    if (grShader == NULL) {
        // Nothing to update
        return;
    }

    VkDescriptorImageInfo* imageInfos = malloc(grShader->bindingCount *
                                               sizeof(VkDescriptorImageInfo));
    VkDescriptorBufferInfo* bufferInfos = malloc(grShader->bindingCount *
                                                 sizeof(VkDescriptorBufferInfo));
    VkBufferView* bufferViews = malloc(grShader->bindingCount * sizeof(VkBufferView));
    VkWriteDescriptorSet* writes = malloc(grShader->bindingCount * sizeof(VkWriteDescriptorSet));

    for (unsigned i = 0; i < grShader->bindingCount; i++) {
        const IlcBinding* binding = &grShader->bindings[i];
        const DescriptorSetSlot* slot;

        if (dynamicMapping->slotObjectType != GR_SLOT_UNUSED &&
            (binding->index == (ILC_BASE_RESOURCE_ID + dynamicMapping->shaderEntityIndex))) {
            slot = dynamicMemoryView;
        } else {
            slot = getDescriptorSetSlot(grDescriptorSet, slotOffset,
                                        &shaderInfo->descriptorSetMapping[0], binding->index);
        }

        if (slot == NULL) {
            LOGE("can't find slot for binding %d\n", binding->index);
            assert(false);
        }

        writes[i] = (VkWriteDescriptorSet) {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = NULL,
            .dstSet = vkDescriptorSet,
            .dstBinding = binding->index,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = binding->descriptorType,
            .pImageInfo = NULL, // Set below
            .pBufferInfo = NULL, // Set below
            .pTexelBufferView = NULL, // Set below
        };

        if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
            if (slot->type != SLOT_TYPE_SAMPLER) {
                LOGE("unexpected slot type %d for descriptor type %d\n",
                     slot->type, binding->descriptorType);
                assert(false);
            }

            imageInfos[i] = (VkDescriptorImageInfo) {
                .sampler = slot->sampler.vkSampler,
                .imageView = VK_NULL_HANDLE,
                .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };
            writes[i].pImageInfo = &imageInfos[i];
        } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                   binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            if (slot->type != SLOT_TYPE_IMAGE_VIEW) {
                LOGE("unexpected slot type %d for descriptor type %d\n",
                     slot->type, binding->descriptorType);
                assert(false);
            }

            imageInfos[i] = (VkDescriptorImageInfo) {
                .sampler = VK_NULL_HANDLE,
                .imageView = slot->imageView.vkImageView,
                .imageLayout = slot->imageView.vkImageLayout,
            };
            writes[i].pImageInfo = &imageInfos[i];
        } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                   binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            if (slot->type != SLOT_TYPE_MEMORY_VIEW) {
                LOGE("unexpected slot type %d for descriptor type %d\n",
                     slot->type, binding->descriptorType);
                assert(false);
            }

            VkBufferView bufferView = VK_NULL_HANDLE;

            const VkBufferViewCreateInfo createInfo = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                .pNext = NULL,
                .flags = 0,
                .buffer = slot->memoryView.vkBuffer,
                .format = slot->memoryView.vkFormat,
                .offset = slot->memoryView.offset,
                .range = slot->memoryView.range,
            };

            vkRes = VKD.vkCreateBufferView(grDevice->device, &createInfo, NULL, &bufferView);
            if (vkRes != VK_SUCCESS) {
                LOGE("vkCreateBufferView failed (%d)\n", vkRes);
            }

            // Track buffer view
            grCmdBuffer->bufferViewCount++;
            grCmdBuffer->bufferViews = realloc(grCmdBuffer->bufferViews,
                                               grCmdBuffer->bufferViewCount * sizeof(VkBufferView));
            grCmdBuffer->bufferViews[grCmdBuffer->bufferViewCount - 1] = bufferView;

            bufferViews[i] = bufferView;
            writes[i].pTexelBufferView = &bufferViews[i];
        } else if (binding->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (slot->type != SLOT_TYPE_MEMORY_VIEW) {
                LOGE("unexpected slot type %d for descriptor type %d\n",
                     slot->type, binding->descriptorType);
                assert(false);
            }

            bufferInfos[i] = (VkDescriptorBufferInfo) {
                .buffer = slot->memoryView.vkBuffer,
                .offset = slot->memoryView.offset,
                .range = slot->memoryView.range,
            };
            writes[i].pBufferInfo = &bufferInfos[i];
        } else {
            LOGE("unhandled descriptor type %d\n", binding->descriptorType);
            assert(false);
        }
    }

    VKD.vkUpdateDescriptorSets(grDevice->device, grShader->bindingCount, writes, 0, NULL);

    free(imageInfos);
    free(bufferInfos);
    free(bufferViews);
    free(writes);
}

static void grCmdBufferBeginRenderPass(
    GrCmdBuffer* grCmdBuffer)
{
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    const GrPipeline* grPipeline =
        grCmdBuffer->bindPoint[VK_PIPELINE_BIND_POINT_GRAPHICS].grPipeline;

    if (grCmdBuffer->hasActiveRenderPass) {
        return;
    }

    const VkRenderPassBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = NULL,
        .renderPass = grPipeline->renderPass,
        .framebuffer = grCmdBuffer->framebuffer,
        .renderArea = (VkRect2D) {
            .offset = { 0, 0 },
            .extent = { grCmdBuffer->minExtent.width, grCmdBuffer->minExtent.height },
        },
        .clearValueCount = 0,
        .pClearValues = NULL,
    };

    VKD.vkCmdBeginRenderPass(grCmdBuffer->commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    grCmdBuffer->hasActiveRenderPass = true;
}

void grCmdBufferEndRenderPass(
    GrCmdBuffer* grCmdBuffer)
{
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    if (!grCmdBuffer->hasActiveRenderPass) {
        return;
    }

    VKD.vkCmdEndRenderPass(grCmdBuffer->commandBuffer);
    grCmdBuffer->hasActiveRenderPass = false;
}

static void grCmdBufferUpdateDescriptorSets(
    GrCmdBuffer* grCmdBuffer,
    VkPipelineBindPoint bindPoint)
{
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrPipeline* grPipeline = grCmdBuffer->bindPoint[bindPoint].grPipeline;
    VkResult vkRes;

    for (unsigned i = 0; i < 2; i++) {
        if (grCmdBuffer->bindPoint[bindPoint].descriptorPool != VK_NULL_HANDLE) {
            const VkDescriptorSetAllocateInfo descSetAllocateInfo = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .pNext = NULL,
                .descriptorPool = grCmdBuffer->bindPoint[bindPoint].descriptorPool,
                .descriptorSetCount = grPipeline->stageCount, // TODO optimize
                .pSetLayouts = grPipeline->descriptorSetLayouts,
            };

            vkRes = VKD.vkAllocateDescriptorSets(grDevice->device, &descSetAllocateInfo,
                                                 grCmdBuffer->bindPoint[bindPoint].descriptorSets);
            if (vkRes == VK_SUCCESS) {
                break;
            } else if (vkRes != VK_ERROR_OUT_OF_POOL_MEMORY) {
                LOGE("vkAllocateDescriptorSets failed (%d)\n", vkRes);
                break;
            } else if (i > 0) {
                LOGE("descriptor set allocation failed with a new pool\n");
                assert(false);
            }
        }

        // Need a new pool
        grCmdBuffer->bindPoint[bindPoint].descriptorPool =
            getVkDescriptorPool(grDevice, grPipeline->stageCount,
                                COUNT_OF(grPipeline->descriptorTypeCounts),
                                grPipeline->descriptorTypeCounts);

        // Track descriptor pool
        grCmdBuffer->descriptorPoolCount++;
        grCmdBuffer->descriptorPools = realloc(grCmdBuffer->descriptorPools,
                                               grCmdBuffer->descriptorPoolCount *
                                               sizeof(VkDescriptorPool));
        grCmdBuffer->descriptorPools[grCmdBuffer->descriptorPoolCount - 1] =
            grCmdBuffer->bindPoint[bindPoint].descriptorPool;
    }

    for (unsigned i = 0; i < grPipeline->stageCount; i++) {
        updateVkDescriptorSet(grDevice, grCmdBuffer,
                              grCmdBuffer->bindPoint[bindPoint].descriptorSets[i],
                              grCmdBuffer->bindPoint[bindPoint].slotOffset,
                              &grPipeline->shaderInfos[i],
                              grCmdBuffer->bindPoint[bindPoint].grDescriptorSet,
                              &grCmdBuffer->bindPoint[bindPoint].dynamicMemoryView);
    }

    VKD.vkCmdBindDescriptorSets(grCmdBuffer->commandBuffer, bindPoint, grPipeline->pipelineLayout,
                                0, grPipeline->stageCount,
                                grCmdBuffer->bindPoint[bindPoint].descriptorSets, 0, NULL);
}

static void grCmdBufferUpdateResources(
    GrCmdBuffer* grCmdBuffer)
{
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrPipeline* grGraphicsPipeline =
        grCmdBuffer->bindPoint[VK_PIPELINE_BIND_POINT_GRAPHICS].grPipeline;

    if (grCmdBuffer->dirtyFlags & FLAG_DIRTY_GRAPHICS_DESCRIPTOR_SETS) {
        grCmdBufferUpdateDescriptorSets(grCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }

    if (grCmdBuffer->dirtyFlags & FLAG_DIRTY_COMPUTE_DESCRIPTOR_SETS) {
        grCmdBufferUpdateDescriptorSets(grCmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);
    }

    if (grCmdBuffer->dirtyFlags & FLAG_DIRTY_FRAMEBUFFER) {
        grCmdBufferEndRenderPass(grCmdBuffer);

        grCmdBuffer->framebuffer = getVkFramebuffer(grDevice, grGraphicsPipeline->renderPass,
                                                    grCmdBuffer->attachmentCount,
                                                    grCmdBuffer->attachments,
                                                    grCmdBuffer->minExtent);

        // Track framebuffer
        grCmdBuffer->framebufferCount++;
        grCmdBuffer->framebuffers = realloc(grCmdBuffer->framebuffers,
                                            grCmdBuffer->framebufferCount * sizeof(VkFramebuffer));
        grCmdBuffer->framebuffers[grCmdBuffer->framebufferCount - 1] = grCmdBuffer->framebuffer;
    }

    if (grCmdBuffer->dirtyFlags & FLAG_DIRTY_PIPELINE) {
        VkPipeline vkPipeline =
            grPipelineFindOrCreateVkPipeline(grGraphicsPipeline,
                                             grCmdBuffer->grColorBlendState,
                                             grCmdBuffer->grRasterState);

        VKD.vkCmdBindPipeline(grCmdBuffer->commandBuffer,
                              VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeline);
    }

    grCmdBuffer->dirtyFlags = 0;
}

// Command Buffer Building Functions

GR_VOID grCmdBindPipeline(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_PIPELINE pipeline)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, pipelineBindPoint, pipeline);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrPipeline* grPipeline = (GrPipeline*)pipeline;
    VkPipelineBindPoint vkBindPoint = getVkPipelineBindPoint(pipelineBindPoint);

    if (grPipeline == grCmdBuffer->bindPoint[vkBindPoint].grPipeline) {
        return;
    }

    grCmdBuffer->bindPoint[vkBindPoint].grPipeline = grPipeline;

    if (pipelineBindPoint == GR_PIPELINE_BIND_POINT_GRAPHICS) {
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_GRAPHICS_DESCRIPTOR_SETS |
                                   FLAG_DIRTY_FRAMEBUFFER |
                                   FLAG_DIRTY_PIPELINE;
    } else {
        // Pipeline creation isn't deferred for compute, bind now
        VKD.vkCmdBindPipeline(grCmdBuffer->commandBuffer, vkBindPoint,
                              grPipelineFindOrCreateVkPipeline(grPipeline, NULL, NULL));

        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_COMPUTE_DESCRIPTOR_SETS;
    }
}

GR_VOID grCmdBindStateObject(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM stateBindPoint,
    GR_STATE_OBJECT state)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, stateBindPoint, state);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    switch ((GR_STATE_BIND_POINT)stateBindPoint) {
    case GR_STATE_BIND_VIEWPORT: {
        GrViewportStateObject* viewportState = (GrViewportStateObject*)state;
        if (viewportState == grCmdBuffer->grViewportState) {
            break;
        }

        VKD.vkCmdSetViewportWithCountEXT(grCmdBuffer->commandBuffer,
                                         viewportState->viewportCount, viewportState->viewports);
        VKD.vkCmdSetScissorWithCountEXT(grCmdBuffer->commandBuffer, viewportState->scissorCount,
                                        viewportState->scissors);
        grCmdBuffer->grViewportState = viewportState;
    }   break;
    case GR_STATE_BIND_RASTER: {
        GrRasterStateObject* rasterState = (GrRasterStateObject*)state;
        if (rasterState == grCmdBuffer->grRasterState) {
            break;
        }

        VKD.vkCmdSetCullModeEXT(grCmdBuffer->commandBuffer, rasterState->cullMode);
        VKD.vkCmdSetFrontFaceEXT(grCmdBuffer->commandBuffer, rasterState->frontFace);
        VKD.vkCmdSetDepthBias(grCmdBuffer->commandBuffer, rasterState->depthBiasConstantFactor,
                              rasterState->depthBiasClamp, rasterState->depthBiasSlopeFactor);
        grCmdBuffer->grRasterState = rasterState;
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_PIPELINE;
    }   break;
    case GR_STATE_BIND_DEPTH_STENCIL: {
        GrDepthStencilStateObject* depthStencilState = (GrDepthStencilStateObject*)state;
        if (depthStencilState == grCmdBuffer->grDepthStencilState) {
            break;
        }

        VKD.vkCmdSetDepthTestEnableEXT(grCmdBuffer->commandBuffer,
                                       depthStencilState->depthTestEnable);
        VKD.vkCmdSetDepthWriteEnableEXT(grCmdBuffer->commandBuffer,
                                        depthStencilState->depthWriteEnable);
        VKD.vkCmdSetDepthCompareOpEXT(grCmdBuffer->commandBuffer,
                                      depthStencilState->depthCompareOp);
        VKD.vkCmdSetDepthBoundsTestEnableEXT(grCmdBuffer->commandBuffer,
                                             depthStencilState->depthBoundsTestEnable);
        VKD.vkCmdSetStencilTestEnableEXT(grCmdBuffer->commandBuffer,
                                         depthStencilState->stencilTestEnable);
        VKD.vkCmdSetStencilOpEXT(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                 depthStencilState->front.failOp,
                                 depthStencilState->front.passOp,
                                 depthStencilState->front.depthFailOp,
                                 depthStencilState->front.compareOp);
        VKD.vkCmdSetStencilCompareMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                       depthStencilState->front.compareMask);
        VKD.vkCmdSetStencilWriteMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                     depthStencilState->front.writeMask);
        VKD.vkCmdSetStencilReference(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_FRONT_BIT,
                                     depthStencilState->front.reference);
        VKD.vkCmdSetStencilOpEXT(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                 depthStencilState->back.failOp,
                                 depthStencilState->back.passOp,
                                 depthStencilState->back.depthFailOp,
                                 depthStencilState->back.compareOp);
        VKD.vkCmdSetStencilCompareMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                       depthStencilState->back.compareMask);
        VKD.vkCmdSetStencilWriteMask(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                     depthStencilState->back.writeMask);
        VKD.vkCmdSetStencilReference(grCmdBuffer->commandBuffer, VK_STENCIL_FACE_BACK_BIT,
                                     depthStencilState->back.reference);
        VKD.vkCmdSetDepthBounds(grCmdBuffer->commandBuffer,
                                depthStencilState->minDepthBounds, depthStencilState->maxDepthBounds);
        grCmdBuffer->grDepthStencilState = depthStencilState;
    }   break;
    case GR_STATE_BIND_COLOR_BLEND: {
        GrColorBlendStateObject* colorBlendState = (GrColorBlendStateObject*)state;
        if (colorBlendState == grCmdBuffer->grColorBlendState) {
            break;
        }

        VKD.vkCmdSetBlendConstants(grCmdBuffer->commandBuffer, colorBlendState->blendConstants);
        grCmdBuffer->grColorBlendState = colorBlendState;
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_PIPELINE;
    }   break;
    case GR_STATE_BIND_MSAA:
        // TODO
        break;
    }
}

GR_VOID grCmdBindDescriptorSet(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_UINT index,
    GR_DESCRIPTOR_SET descriptorSet,
    GR_UINT slotOffset)
{
    LOGT("%p 0x%X %u %p %u\n", cmdBuffer, pipelineBindPoint, index,  descriptorSet, slotOffset);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    GrDescriptorSet* grDescriptorSet = (GrDescriptorSet*)descriptorSet;
    VkPipelineBindPoint vkBindPoint = getVkPipelineBindPoint(pipelineBindPoint);

    if (index != 0) {
        LOGW("unsupported index %u\n", index);
    }

    grCmdBuffer->bindPoint[vkBindPoint].grDescriptorSet = grDescriptorSet;
    grCmdBuffer->bindPoint[vkBindPoint].slotOffset = slotOffset;
    if (pipelineBindPoint == GR_PIPELINE_BIND_POINT_GRAPHICS) {
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_GRAPHICS_DESCRIPTOR_SETS;
    } else {
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_COMPUTE_DESCRIPTOR_SETS;
    }
}

GR_VOID grCmdBindDynamicMemoryView(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    const GR_MEMORY_VIEW_ATTACH_INFO* pMemView)
{
    LOGT("%p 0x%X %p\n", cmdBuffer, pipelineBindPoint, pMemView);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrGpuMemory* grGpuMemory = (GrGpuMemory*)pMemView->mem;
    VkPipelineBindPoint vkBindPoint = getVkPipelineBindPoint(pipelineBindPoint);

    // FIXME what is pMemView->state for?

    grCmdBuffer->bindPoint[vkBindPoint].dynamicMemoryView = (DescriptorSetSlot) {
        .type = SLOT_TYPE_MEMORY_VIEW,
        .memoryView = {
            .vkBuffer = grGpuMemory->buffer,
            .vkFormat = getVkFormat(pMemView->format),
            .offset = pMemView->offset,
            .range = pMemView->range,
        },
    };

    if (pipelineBindPoint == GR_PIPELINE_BIND_POINT_GRAPHICS) {
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_GRAPHICS_DESCRIPTOR_SETS;
    } else {
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_COMPUTE_DESCRIPTOR_SETS;
    }
}

GR_VOID grCmdBindIndexData(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY mem,
    GR_GPU_SIZE offset,
    GR_ENUM indexType)
{
    LOGT("%p %p %u 0x%X\n", cmdBuffer, mem, offset, indexType);
    const GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    const GrGpuMemory* grGpuMemory = (GrGpuMemory*)mem;

    VKD.vkCmdBindIndexBuffer(grCmdBuffer->commandBuffer, grGpuMemory->buffer, offset,
                             getVkIndexType(indexType));
}

GR_VOID grCmdPrepareMemoryRegions(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT transitionCount,
    const GR_MEMORY_STATE_TRANSITION* pStateTransitions)
{
    LOGT("%p %u %p\n", cmdBuffer, transitionCount, pStateTransitions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    grCmdBufferEndRenderPass(grCmdBuffer);

    VkBufferMemoryBarrier* barriers = malloc(transitionCount * sizeof(VkBufferMemoryBarrier));
    for (unsigned i = 0; i < transitionCount; i++) {
        const GR_MEMORY_STATE_TRANSITION* stateTransition = &pStateTransitions[i];
        GrGpuMemory* grGpuMemory = (GrGpuMemory*)stateTransition->mem;

        grGpuMemoryBindBuffer(grGpuMemory);

        barriers[i] = (VkBufferMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = getVkAccessFlagsMemory(stateTransition->oldState),
            .dstAccessMask = getVkAccessFlagsMemory(stateTransition->newState),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = grGpuMemory->buffer,
            .offset = stateTransition->offset,
            .size = stateTransition->regionSize > 0 ? stateTransition->regionSize : VK_WHOLE_SIZE,
        };
    }

    VKD.vkCmdPipelineBarrier(grCmdBuffer->commandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                             0, 0, NULL, transitionCount, barriers, 0, NULL);
    free(barriers);
}

// FIXME what are target states for?
GR_VOID grCmdBindTargets(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT colorTargetCount,
    const GR_COLOR_TARGET_BIND_INFO* pColorTargets,
    const GR_DEPTH_STENCIL_BIND_INFO* pDepthTarget)
{
    LOGT("%p %u %p %p\n", cmdBuffer, colorTargetCount, pColorTargets, pDepthTarget);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;

    assert(colorTargetCount <= GR_MAX_COLOR_TARGETS);

    // Find minimum extent
    VkExtent3D minExtent = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    for (unsigned i = 0; i < colorTargetCount; i++) {
        const GrColorTargetView* grColorTargetView = (GrColorTargetView*)pColorTargets[i].view;

        if (grColorTargetView != NULL) {
            minExtent.width = MIN(minExtent.width, grColorTargetView->extent.width);
            minExtent.height = MIN(minExtent.height, grColorTargetView->extent.height);
            minExtent.depth = MIN(minExtent.depth, grColorTargetView->extent.depth);
        }
    }
    if (pDepthTarget != NULL) {
        const GrDepthStencilView* grDepthStencilView = (GrDepthStencilView*)pDepthTarget->view;

        if (grDepthStencilView != NULL) {
            minExtent.width = MIN(minExtent.width, grDepthStencilView->extent.width);
            minExtent.height = MIN(minExtent.height, grDepthStencilView->extent.height);
            minExtent.depth = MIN(minExtent.depth, grDepthStencilView->extent.depth);
        }
    }

    // Copy attachments
    unsigned attachmentCount = 0;
    VkImageView attachments[COUNT_OF(grCmdBuffer->attachments)];
    for (unsigned i = 0; i < colorTargetCount; i++) {
        const GrColorTargetView* grColorTargetView = (GrColorTargetView*)pColorTargets[i].view;

        if (grColorTargetView != NULL) {
            attachments[attachmentCount] = grColorTargetView->imageView;
            attachmentCount++;
        }
    }
    if (pDepthTarget != NULL) {
        const GrDepthStencilView* grDepthStencilView = (GrDepthStencilView*)pDepthTarget->view;

        if (grDepthStencilView != NULL) {
            attachments[attachmentCount] = grDepthStencilView->imageView;
            attachmentCount++;
        }
    }

    if (memcmp(&minExtent, &grCmdBuffer->minExtent, sizeof(minExtent)) != 0 ||
        attachmentCount != grCmdBuffer->attachmentCount ||
        memcmp(attachments, grCmdBuffer->attachments, attachmentCount * sizeof(VkImageView)) != 0) {
        // Targets have changed
        grCmdBuffer->minExtent = minExtent;
        grCmdBuffer->attachmentCount = attachmentCount;
        memcpy(grCmdBuffer->attachments, attachments, attachmentCount * sizeof(VkImageView));
        grCmdBuffer->dirtyFlags |= FLAG_DIRTY_FRAMEBUFFER;
    }
}

GR_VOID grCmdPrepareImages(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT transitionCount,
    const GR_IMAGE_STATE_TRANSITION* pStateTransitions)
{
    LOGT("%p %u %p\n", cmdBuffer, transitionCount, pStateTransitions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    grCmdBufferEndRenderPass(grCmdBuffer);

    VkImageMemoryBarrier* barriers = malloc(transitionCount * sizeof(VkImageMemoryBarrier));
    for (unsigned i = 0; i < transitionCount; i++) {
        const GR_IMAGE_STATE_TRANSITION* stateTransition = &pStateTransitions[i];
        GrImage* grImage = (GrImage*)stateTransition->image;

        barriers[i] = (VkImageMemoryBarrier) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = NULL,
            .srcAccessMask = getVkAccessFlagsImage(stateTransition->oldState),
            .dstAccessMask = getVkAccessFlagsImage(stateTransition->newState),
            .oldLayout = getVkImageLayout(stateTransition->oldState),
            .newLayout = getVkImageLayout(stateTransition->newState),
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = grImage->image,
            .subresourceRange = getVkImageSubresourceRange(stateTransition->subresourceRange,
                                                           grImage->isCube),
        };
    }

    VKD.vkCmdPipelineBarrier(grCmdBuffer->commandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // TODO optimize
                             0, 0, NULL, 0, NULL, transitionCount, barriers);
    free(barriers);
}

GR_VOID grCmdDraw(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT firstVertex,
    GR_UINT vertexCount,
    GR_UINT firstInstance,
    GR_UINT instanceCount)
{
    LOGT("%p %u %u %u %u\n", cmdBuffer, firstVertex, vertexCount, firstInstance, instanceCount);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    if (grCmdBuffer->dirtyFlags != 0) {
        grCmdBufferUpdateResources(grCmdBuffer);
    }

    grCmdBufferBeginRenderPass(grCmdBuffer);

    VKD.vkCmdDraw(grCmdBuffer->commandBuffer,
                  vertexCount, instanceCount, firstVertex, firstInstance);
}

GR_VOID grCmdDrawIndexed(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT firstIndex,
    GR_UINT indexCount,
    GR_INT vertexOffset,
    GR_UINT firstInstance,
    GR_UINT instanceCount)
{
    LOGT("%p %u %u %d %u %u\n",
         cmdBuffer, firstIndex, indexCount, vertexOffset, firstInstance, instanceCount);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    if (grCmdBuffer->dirtyFlags != 0) {
        grCmdBufferUpdateResources(grCmdBuffer);
    }

    grCmdBufferBeginRenderPass(grCmdBuffer);

    VKD.vkCmdDrawIndexed(grCmdBuffer->commandBuffer,
                         indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

GR_VOID grCmdDrawIndirect(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY mem,
    GR_GPU_SIZE offset)
{
    LOGT("%p %p %u\n", cmdBuffer, mem, offset);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrGpuMemory* grGpuMemory = (GrGpuMemory*)mem;

    if (grCmdBuffer->dirtyFlags != 0) {
        grCmdBufferUpdateResources(grCmdBuffer);
    }

    grCmdBufferBeginRenderPass(grCmdBuffer);

    VKD.vkCmdDrawIndirect(grCmdBuffer->commandBuffer, grGpuMemory->buffer, offset, 1, 0);
}

GR_VOID grCmdDrawIndexedIndirect(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY mem,
    GR_GPU_SIZE offset)
{
    LOGT("%p %p %u\n", cmdBuffer, mem, offset);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrGpuMemory* grGpuMemory = (GrGpuMemory*)mem;

    if (grCmdBuffer->dirtyFlags != 0) {
        grCmdBufferUpdateResources(grCmdBuffer);
    }

    grCmdBufferBeginRenderPass(grCmdBuffer);

    VKD.vkCmdDrawIndexedIndirect(grCmdBuffer->commandBuffer, grGpuMemory->buffer, offset, 1, 0);
}

GR_VOID grCmdDispatch(
    GR_CMD_BUFFER cmdBuffer,
    GR_UINT x,
    GR_UINT y,
    GR_UINT z)
{
    LOGT("%p %u %u %u\n", cmdBuffer, x, y, z);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);

    if (grCmdBuffer->dirtyFlags != 0) {
        grCmdBufferUpdateResources(grCmdBuffer);
    }

    grCmdBufferEndRenderPass(grCmdBuffer);

    VKD.vkCmdDispatch(grCmdBuffer->commandBuffer, x, y, z);
}

GR_VOID grCmdCopyMemory(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY srcMem,
    GR_GPU_MEMORY destMem,
    GR_UINT regionCount,
    const GR_MEMORY_COPY* pRegions)
{
    LOGT("%p %p %p %u %p\n", cmdBuffer, srcMem, destMem, regionCount, pRegions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrGpuMemory* grSrcGpuMemory = (GrGpuMemory*)srcMem;
    GrGpuMemory* grDstGpuMemory = (GrGpuMemory*)destMem;

    grCmdBufferEndRenderPass(grCmdBuffer);
    grGpuMemoryBindBuffer(grSrcGpuMemory);
    grGpuMemoryBindBuffer(grDstGpuMemory);

    VkBufferCopy* vkRegions = malloc(regionCount * sizeof(VkBufferCopy));
    for (unsigned i = 0; i < regionCount; i++) {
        const GR_MEMORY_COPY* region = &pRegions[i];

        vkRegions[i] = (VkBufferCopy) {
            .srcOffset = region->srcOffset,
            .dstOffset = region->destOffset,
            .size = region->copySize,
        };
    }

    VKD.vkCmdCopyBuffer(grCmdBuffer->commandBuffer, grSrcGpuMemory->buffer, grDstGpuMemory->buffer,
                        regionCount, vkRegions);

    free(vkRegions);
}

GR_VOID grCmdCopyImage(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE srcImage,
    GR_IMAGE destImage,
    GR_UINT regionCount,
    const GR_IMAGE_COPY* pRegions)
{
    LOGT("%p %p %p %u %p\n", cmdBuffer, srcImage, destImage, regionCount, pRegions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrImage* grSrcImage = (GrImage*)srcImage;
    GrImage* grDstImage = (GrImage*)destImage;
    unsigned srcTileSize = getVkFormatTileSize(grSrcImage->format);
    unsigned dstTileSize = getVkFormatTileSize(grDstImage->format);

    if (quirkHas(QUIRK_COMPRESSED_IMAGE_COPY_IN_TEXELS)) {
        srcTileSize = 1;
        dstTileSize = 1;
    }

    grCmdBufferEndRenderPass(grCmdBuffer);

    if (grSrcImage->image != VK_NULL_HANDLE) {
        VkImageCopy* vkRegions = malloc(regionCount * sizeof(VkImageCopy));
        for (unsigned i = 0; i < regionCount; i++) {
            const GR_IMAGE_COPY* region = &pRegions[i];

            vkRegions[i] = (VkImageCopy) {
                .srcSubresource = getVkImageSubresourceLayers(region->srcSubresource),
                .srcOffset = {
                    region->srcOffset.x * srcTileSize,
                    region->srcOffset.y * srcTileSize,
                    region->srcOffset.z,
                },
                .dstSubresource = getVkImageSubresourceLayers(region->destSubresource),
                .dstOffset = {
                    region->destOffset.x * dstTileSize,
                    region->destOffset.y * dstTileSize,
                    region->destOffset.z,
                },
                .extent = {
                    region->extent.width * dstTileSize,
                    region->extent.height * dstTileSize,
                    region->extent.depth,
                },
            };
        }

        VKD.vkCmdCopyImage(grCmdBuffer->commandBuffer,
                           grSrcImage->image, getVkImageLayout(GR_IMAGE_STATE_DATA_TRANSFER),
                           grDstImage->image, getVkImageLayout(GR_IMAGE_STATE_DATA_TRANSFER),
                           regionCount, vkRegions);

        free(vkRegions);
    } else {
        VkBufferImageCopy* vkRegions = malloc(regionCount * sizeof(VkBufferImageCopy));
        for (unsigned i = 0; i < regionCount; i++) {
            const GR_IMAGE_COPY* region = &pRegions[i];

            if (region->srcSubresource.aspect != GR_IMAGE_ASPECT_COLOR) {
                LOGW("unhandled non-color aspect 0x%X\n", region->srcSubresource.aspect);
            }
            if (region->srcOffset.x != 0 || region->srcOffset.y != 0 || region->srcOffset.z != 0) {
                LOGW("unhandled region offset %u %u %u for buffer\n",
                     region->srcOffset.x, region->srcOffset.y, region->srcOffset.z);
            }

            const VkExtent3D srcTexelExtent = {
                grSrcImage->extent.width * srcTileSize,
                grSrcImage->extent.height * srcTileSize,
                grSrcImage->extent.depth,
            };

            vkRegions[i] = (VkBufferImageCopy) {
                .bufferOffset = grImageGetBufferOffset(srcTexelExtent, grSrcImage->format,
                                                       region->srcSubresource.arraySlice,
                                                       grSrcImage->arrayLayers,
                                                       region->srcSubresource.mipLevel),
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = getVkImageSubresourceLayers(region->destSubresource),
                .imageOffset = {
                    region->destOffset.x * dstTileSize,
                    region->destOffset.y * dstTileSize,
                    region->destOffset.z,
                },
                .imageExtent = {
                    region->extent.width * dstTileSize,
                    region->extent.height * dstTileSize,
                    region->extent.depth,
                },
            };
        }

        VKD.vkCmdCopyBufferToImage(grCmdBuffer->commandBuffer,
                                   grSrcImage->buffer, grDstImage->image,
                                   getVkImageLayout(GR_IMAGE_STATE_DATA_TRANSFER),
                                   regionCount, vkRegions);

        free(vkRegions);
    }
}

GR_VOID grCmdCopyMemoryToImage(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY srcMem,
    GR_IMAGE destImage,
    GR_UINT regionCount,
    const GR_MEMORY_IMAGE_COPY* pRegions)
{
    LOGT("%p %p %p %u %p\n", cmdBuffer, srcMem, destImage, regionCount, pRegions);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrGpuMemory* grSrcGpuMemory = (GrGpuMemory*)srcMem;
    GrImage* grDstImage = (GrImage*)destImage;
    unsigned dstTileSize = getVkFormatTileSize(grDstImage->format);

    if (quirkHas(QUIRK_COMPRESSED_IMAGE_COPY_IN_TEXELS)) {
        dstTileSize = 1;
    }

    grCmdBufferEndRenderPass(grCmdBuffer);
    grGpuMemoryBindBuffer(grSrcGpuMemory);

    VkBufferImageCopy* vkRegions = malloc(regionCount * sizeof(VkBufferImageCopy));
    for (unsigned i = 0; i < regionCount; i++) {
        const GR_MEMORY_IMAGE_COPY* region = &pRegions[i];

        vkRegions[i] = (VkBufferImageCopy) {
            .bufferOffset = region->memOffset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = getVkImageSubresourceLayers(region->imageSubresource),
            .imageOffset = {
                region->imageOffset.x * dstTileSize,
                region->imageOffset.y * dstTileSize,
                region->imageOffset.z
            },
            .imageExtent = {
                region->imageExtent.width * dstTileSize,
                region->imageExtent.height * dstTileSize,
                region->imageExtent.depth,
            },
        };
    }

    VKD.vkCmdCopyBufferToImage(grCmdBuffer->commandBuffer, grSrcGpuMemory->buffer,
                               grDstImage->image, getVkImageLayout(GR_IMAGE_STATE_DATA_TRANSFER),
                               regionCount, vkRegions);

    free(vkRegions);
}

GR_VOID grCmdFillMemory(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset,
    GR_GPU_SIZE fillSize,
    GR_UINT32 data)
{
    LOGT("%p %p %u %u %u\n", cmdBuffer, destMem, destOffset, fillSize, data);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrGpuMemory* grDstGpuMemory = (GrGpuMemory*)destMem;

    grCmdBufferEndRenderPass(grCmdBuffer);
    grGpuMemoryBindBuffer(grDstGpuMemory);

    VKD.vkCmdFillBuffer(grCmdBuffer->commandBuffer, grDstGpuMemory->buffer, destOffset,
                        fillSize, data);
}

GR_VOID grCmdClearColorImage(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    const GR_FLOAT color[4],
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %g %g %g %g %u %p\n",
         cmdBuffer, image, color[0], color[1], color[2], color[3], rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrImage* grImage = (GrImage*)image;

    grCmdBufferEndRenderPass(grCmdBuffer);

    const VkClearColorValue vkColor = {
        .float32 = { color[0], color[1], color[2], color[3] },
    };

    VkImageSubresourceRange* vkRanges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
    for (int i = 0; i < rangeCount; i++) {
        vkRanges[i] = getVkImageSubresourceRange(pRanges[i], grImage->isCube);
    }

    VKD.vkCmdClearColorImage(grCmdBuffer->commandBuffer, grImage->image,
                             getVkImageLayout(GR_IMAGE_STATE_CLEAR),
                             &vkColor, rangeCount, vkRanges);

    free(vkRanges);
}

GR_VOID grCmdClearColorImageRaw(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    const GR_UINT32 color[4],
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %u %u %u %u %u %p\n",
         cmdBuffer, image, color[0], color[1], color[2], color[3], rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrImage* grImage = (GrImage*)image;

    grCmdBufferEndRenderPass(grCmdBuffer);

    const VkClearColorValue vkColor = {
        .uint32 = { color[0], color[1], color[2], color[3] },
    };

    VkImageSubresourceRange* vkRanges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
    for (int i = 0; i < rangeCount; i++) {
        vkRanges[i] = getVkImageSubresourceRange(pRanges[i], grImage->isCube);
    }

    VKD.vkCmdClearColorImage(grCmdBuffer->commandBuffer, grImage->image,
                             getVkImageLayout(GR_IMAGE_STATE_CLEAR),
                             &vkColor, rangeCount, vkRanges);

    free(vkRanges);
}

GR_VOID grCmdClearDepthStencil(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE image,
    GR_FLOAT depth,
    GR_UINT8 stencil,
    GR_UINT rangeCount,
    const GR_IMAGE_SUBRESOURCE_RANGE* pRanges)
{
    LOGT("%p %p %g %u %u %p\n", cmdBuffer, image, depth, stencil, rangeCount, pRanges);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrImage* grImage = (GrImage*)image;

    grCmdBufferEndRenderPass(grCmdBuffer);

    const VkClearDepthStencilValue depthStencilValue = {
        .depth = depth,
        .stencil = stencil,
    };

    VkImageSubresourceRange* vkRanges = malloc(rangeCount * sizeof(VkImageSubresourceRange));
    for (int i = 0; i < rangeCount; i++) {
        vkRanges[i] = getVkImageSubresourceRange(pRanges[i], grImage->isCube);
    }

    VKD.vkCmdClearDepthStencilImage(grCmdBuffer->commandBuffer, grImage->image,
                                    getVkImageLayout(GR_IMAGE_STATE_CLEAR), &depthStencilValue,
                                    rangeCount, vkRanges);

    free(vkRanges);
}

GR_VOID grCmdSetEvent(
    GR_CMD_BUFFER cmdBuffer,
    GR_EVENT event)
{
    LOGT("%p %p\n", cmdBuffer, event);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrEvent* grEvent = (GrEvent*)event;

    grCmdBufferEndRenderPass(grCmdBuffer);

    VKD.vkCmdSetEvent(grCmdBuffer->commandBuffer, grEvent->event,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

GR_VOID grCmdResetEvent(
    GR_CMD_BUFFER cmdBuffer,
    GR_EVENT event)
{
    LOGT("%p %p\n", cmdBuffer, event);
    GrCmdBuffer* grCmdBuffer = (GrCmdBuffer*)cmdBuffer;
    const GrDevice* grDevice = GET_OBJ_DEVICE(grCmdBuffer);
    GrEvent* grEvent = (GrEvent*)event;

    grCmdBufferEndRenderPass(grCmdBuffer);

    VKD.vkCmdResetEvent(grCmdBuffer->commandBuffer, grEvent->event,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}
