#include <string.h>
#include "mantle/mantle.h"
#include "mantle/mantleDbg.h"
#include "mantle/mantleWsiWinExt.h"
#include "logger.h"

// Queue Functions

GR_RESULT grQueueSetGlobalMemReferences(
    GR_QUEUE queue,
    GR_UINT memRefCount,
    const GR_MEMORY_REF* pMemRefs)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

// Memory Management Functions

GR_RESULT grSetMemoryPriority(
    GR_GPU_MEMORY mem,
    GR_ENUM priority)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grRemapVirtualMemoryPages(
    GR_DEVICE device,
    GR_UINT rangeCount,
    const GR_VIRTUAL_MEMORY_REMAP_RANGE* pRanges,
    GR_UINT preWaitSemaphoreCount,
    const GR_QUEUE_SEMAPHORE* pPreWaitSemaphores,
    GR_UINT postSignalSemaphoreCount,
    const GR_QUEUE_SEMAPHORE* pPostSignalSemaphores)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grPinSystemMemory(
    GR_DEVICE device,
    const GR_VOID* pSysMem,
    GR_SIZE memSize,
    GR_GPU_MEMORY* pMem)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

// Shader and Pipeline Functions

GR_RESULT grStorePipeline(
    GR_PIPELINE pipeline,
    GR_SIZE* pDataSize,
    GR_VOID* pData)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grLoadPipeline(
    GR_DEVICE device,
    GR_SIZE dataSize,
    const GR_VOID* pData,
    GR_PIPELINE* pPipeline)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

// Query and Synchronization Functions

GR_RESULT grCreateQueryPool(
    GR_DEVICE device,
    const GR_QUERY_POOL_CREATE_INFO* pCreateInfo,
    GR_QUERY_POOL* pQueryPool)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grGetQueryPoolResults(
    GR_QUERY_POOL queryPool,
    GR_UINT startQuery,
    GR_UINT queryCount,
    GR_SIZE* pDataSize,
    GR_VOID* pData)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

// Multi-Device Management Functions

GR_RESULT grOpenSharedMemory(
    GR_DEVICE device,
    const GR_MEMORY_OPEN_INFO* pOpenInfo,
    GR_GPU_MEMORY* pMem)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grOpenSharedQueueSemaphore(
    GR_DEVICE device,
    const GR_QUEUE_SEMAPHORE_OPEN_INFO* pOpenInfo,
    GR_QUEUE_SEMAPHORE* pSemaphore)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grOpenPeerMemory(
    GR_DEVICE device,
    const GR_PEER_MEMORY_OPEN_INFO* pOpenInfo,
    GR_GPU_MEMORY* pMem)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grOpenPeerImage(
    GR_DEVICE device,
    const GR_PEER_IMAGE_OPEN_INFO* pOpenInfo,
    GR_IMAGE* pImage,
    GR_GPU_MEMORY* pMem)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

// Command Buffer Building Functions

GR_VOID grCmdDispatchIndirect(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY mem,
    GR_GPU_SIZE offset)
{
    LOGW("STUB\n");
}

GR_VOID grCmdCopyImageToMemory(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE srcImage,
    GR_GPU_MEMORY destMem,
    GR_UINT regionCount,
    const GR_MEMORY_IMAGE_COPY* pRegions)
{
    LOGW("STUB\n");
}

GR_VOID grCmdResolveImage(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE srcImage,
    GR_IMAGE destImage,
    GR_UINT regionCount,
    const GR_IMAGE_RESOLVE* pRegions)
{
    LOGW("STUB\n");
}

GR_VOID grCmdCloneImageData(
    GR_CMD_BUFFER cmdBuffer,
    GR_IMAGE srcImage,
    GR_ENUM srcImageState,
    GR_IMAGE destImage,
    GR_ENUM destImageState)
{
    LOGW("STUB\n");
}

GR_VOID grCmdUpdateMemory(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset,
    GR_GPU_SIZE dataSize,
    const GR_UINT32* pData)
{
    LOGW("STUB\n");
}

GR_VOID grCmdMemoryAtomic(
    GR_CMD_BUFFER cmdBuffer,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset,
    GR_UINT64 srcData,
    GR_ENUM atomicOp)
{
    LOGW("STUB\n");
}

GR_VOID grCmdBeginQuery(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT slot,
    GR_FLAGS flags)
{
    LOGW("STUB\n");
}

GR_VOID grCmdEndQuery(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT slot)
{
    LOGW("STUB\n");
}

GR_VOID grCmdResetQueryPool(
    GR_CMD_BUFFER cmdBuffer,
    GR_QUERY_POOL queryPool,
    GR_UINT startQuery,
    GR_UINT queryCount)
{
    LOGW("STUB\n");
}

GR_VOID grCmdWriteTimestamp(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM timestampType,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset)
{
    LOGW("STUB\n");
}

GR_VOID grCmdInitAtomicCounters(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_UINT startCounter,
    GR_UINT counterCount,
    const GR_UINT32* pData)
{
    LOGW("STUB\n");
}

GR_VOID grCmdLoadAtomicCounters(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_UINT startCounter,
    GR_UINT counterCount,
    GR_GPU_MEMORY srcMem,
    GR_GPU_SIZE srcOffset)
{
    LOGW("STUB\n");
}

GR_VOID grCmdSaveAtomicCounters(
    GR_CMD_BUFFER cmdBuffer,
    GR_ENUM pipelineBindPoint,
    GR_UINT startCounter,
    GR_UINT counterCount,
    GR_GPU_MEMORY destMem,
    GR_GPU_SIZE destOffset)
{
    LOGW("STUB\n");
}

// Debug Functions

GR_RESULT GR_STDCALL grDbgSetValidationLevel(
    GR_DEVICE device,
    GR_ENUM validationLevel)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgRegisterMsgCallback(
    GR_DBG_MSG_CALLBACK_FUNCTION pfnMsgCallback,
    GR_VOID* pUserData)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgUnregisterMsgCallback(
    GR_DBG_MSG_CALLBACK_FUNCTION pfnMsgCallback)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgSetMessageFilter(
    GR_DEVICE device,
    GR_ENUM msgCode,
    GR_ENUM filter)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgSetObjectTag(
    GR_BASE_OBJECT object,
    GR_SIZE tagSize,
    const GR_VOID* pTag)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgSetGlobalOption(
    GR_DBG_GLOBAL_OPTION dbgOption,
    GR_SIZE dataSize,
    const GR_VOID* pData)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grDbgSetDeviceOption(
    GR_DEVICE device,
    GR_DBG_DEVICE_OPTION dbgOption,
    GR_SIZE dataSize,
    const GR_VOID* pData)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_VOID GR_STDCALL grCmdDbgMarkerBegin(
    GR_CMD_BUFFER cmdBuffer,
    const GR_CHAR* pMarker)
{
    LOGW("STUB\n");
}

GR_VOID GR_STDCALL grCmdDbgMarkerEnd(
    GR_CMD_BUFFER cmdBuffer)
{
    LOGW("STUB\n");
}

// WSI Functions

GR_RESULT grWsiWinReleaseFullscreenOwnership(
    GR_WSI_WIN_DISPLAY display)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grWsiWinSetGammaRamp(
    GR_WSI_WIN_DISPLAY display,
    const GR_WSI_WIN_GAMMA_RAMP* pGammaRamp)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grWsiWinWaitForVerticalBlank(
    GR_WSI_WIN_DISPLAY display)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}

GR_RESULT grWsiWinGetScanLine(
    GR_WSI_WIN_DISPLAY display,
    GR_INT* pScanLine)
{
    LOGW("STUB\n");
    return GR_UNSUPPORTED;
}
