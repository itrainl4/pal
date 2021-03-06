/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2014-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "core/cmdBuffer.h"
#include "core/fence.h"
#include "core/cmdStream.h"
#include "core/device.h"
#include "core/engine.h"
#include "core/fence.h"
#include "core/gpuMemory.h"
#include "core/platform.h"
#include "core/queue.h"
#include "core/queueContext.h"
#include "core/queueSemaphore.h"
#include "core/swapChain.h"
#include "core/hw/ossip/ossDevice.h"
#include "core/hw/gfxip/gfxDevice.h"
#include "core/hw/gfxip/gfxCmdBuffer.h"
#include "core/hw/gfxip/rpm/rsrcProcMgr.h"
#include "palDequeImpl.h"
#include "palSysUtil.h"
#include "palAutoBuffer.h"

using namespace Util;

namespace Pal
{

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555
// Struct for passing the log file and pal setting pointers to the command buffer dump callback.
struct CmdDumpToFilePayload
{
    File*               pLogFile;
    const PalSettings*  pSettings;
};

// =====================================================================================================================
// Helper fuction for writing out the header of a text dump of a command buffer.
static Result WriteCmdBufferDumpHeaderToFile(
    const CmdBufferDumpDesc&      cmdBufferDesc,
    File*                         pLogFile,
    uint64                        sizeOfBufferInDwords)
{
    const char* QueueTypeStrings[] =
    {
        "# Universal Queue - QueueContext",
        "# Compute Queue - QueueContext",
        "# DMA Queue - QueueContext",
        "",
    };

    static_assert(ArrayLen(QueueTypeStrings) == static_cast<size_t>(QueueTypeCount),
        "Mismatch between QueueTypeStrings array size and QueueTypeCount");

    const char* EngineQueueStrings[] =
    {
        "# Universal Queue -",
        "# Compute Queue -",
        "# DMA Queue -",
        " ",
    };

    static_assert(ArrayLen(EngineQueueStrings) == static_cast<size_t>(EngineTypeCount),
        "Mismatch between UniversalQueueStrings array size and EngineTypeCount");

    const char* commandString = "";
    const char* suffix = "";

    if ((cmdBufferDesc.flags.isPostamble == true) ||
        (cmdBufferDesc.flags.isPreamble == true))
    {
        commandString = QueueTypeStrings[cmdBufferDesc.queueType];
    }
    else
    {
        commandString = EngineQueueStrings[cmdBufferDesc.engineType];

        if (cmdBufferDesc.engineType == EngineTypeUniversal)
        {
            if (cmdBufferDesc.subEngineType == SubEngineType::Primary)
            {
                suffix = " DE";
            }
            else
            {
                suffix = " CE";
            }
        }
    }

    // First, output the header information.
    constexpr size_t MaxLineSize = 128;
    char line[MaxLineSize];

    Snprintf(line, MaxLineSize, "%s%s%s%llu\n", commandString, suffix, " Command length = ", sizeOfBufferInDwords);
    return pLogFile->Write(line, strlen(line));
}

// =====================================================================================================================
// callback function for writing commmand buffers to a file.
static void PAL_STDCALL WriteCmdDumpToFile(
    const CmdBufferDumpDesc&        cmdBufferDesc,
    const CmdBufferChunkDumpDesc*   pChunks,
    uint32                          numChunks,
    void*                           pUserData)
{
    CmdDumpToFilePayload* pPayload = reinterpret_cast<CmdDumpToFilePayload*>(pUserData);
    File* pLogFile = pPayload->pLogFile;

    const CmdBufDumpFormat dumpFormat = pPayload->pSettings->cmdBufDumpFormat;

    Result result = Result::Success;

    if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatText)
    {
        // Compute the size of all data associated with this stream.
        uint64 sizeOfBufferInDwords = 0;
        for (uint32 index = 0; index < numChunks; ++index)
        {
            sizeOfBufferInDwords += NumBytesToNumDwords(static_cast<uint32>(pChunks[index].size));
        }

        result = WriteCmdBufferDumpHeaderToFile(cmdBufferDesc, pLogFile, sizeOfBufferInDwords);
    }

    uint32 subEngineId = 0; // DE subengine ID

    if (cmdBufferDesc.subEngineType == SubEngineType::ConstantEngine)
    {
        if (cmdBufferDesc.flags.isPreamble == true)
        {
            subEngineId = 2; // CE preamble subengine ID
        }
        else
        {
            subEngineId = 1; // CE subengine ID
        }
    }
    else if (cmdBufferDesc.engineType == EngineType::EngineTypeDma)
    {
        subEngineId = 4; // SDMA engine ID
    }

    // Next, walk through all the chunks that make up this command stream and write their command to the file.
    for (uint32 index = 0; index < numChunks; ++index)
    {
        const CmdBufferChunkDumpDesc& chunkDesc = pChunks[index];

        if ((dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinary) ||
            (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders))
        {
            if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders)
            {
                const CmdBufferDumpHeader chunkheader =
                {
                    static_cast<uint32>(sizeof(CmdBufferDumpHeader)),
                    static_cast<uint32>(chunkDesc.size),
                    subEngineId
                };
                pLogFile->Write(&chunkheader, sizeof(chunkheader));
            }
            pLogFile->Write(chunkDesc.pCommands, chunkDesc.size);
        }
        else
        {
            constexpr uint32 MaxLineSize = 16;
            char line[MaxLineSize];

            PAL_ASSERT(dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatText);

            const uint32 chunkSizeInDwords = NumBytesToNumDwords(static_cast<uint32>(chunkDesc.size));

            for (uint32 idx = 0; idx < chunkSizeInDwords && (result == Result::Success); ++idx)
            {
                Snprintf(line, MaxLineSize, "0x%08x\n", reinterpret_cast<const uint32*>(chunkDesc.pCommands)[idx]);
                result = pLogFile->Write(line, strlen(line));
            }
        }
    }

    // Don't bother returning an error if the command buffer wasn't dumped correctly as we don't want this to affect
    // operation of the "important" stuff...  but still make it apparent that the dump file isn't accurate.
    PAL_ALERT(result != Result::Success);
}
#endif // PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555

// =====================================================================================================================
void SubmissionContext::TakeReference()
{
    AtomicIncrement(&m_refCount);
}

// =====================================================================================================================
void SubmissionContext::ReleaseReference()
{
    PAL_ASSERT(m_refCount > 0);

    if (AtomicDecrement(&m_refCount) == 0)
    {
        Platform*const pPlatform = m_pPlatform;
        PAL_DELETE_THIS(SubmissionContext, pPlatform);
    }
}

// =====================================================================================================================
Queue::Queue(
    uint32                 queueCount,
    Device*                pDevice,
    const QueueCreateInfo* pCreateInfo)
    :
    m_pDevice(pDevice),
    m_pSubmissionContext(nullptr),
    m_pDummyCmdBuffer(nullptr),
    m_ifhMode(IfhModeDisabled),
    m_pQueueInfos(nullptr),
    m_queueCount(queueCount),
    m_stalled(false),
    m_pWaitingSemaphore(nullptr),
    m_batchedSubmissionCount(0),
    m_batchedCmds(pDevice->GetPlatform()),
    m_deviceMembershipNode(this),
    m_lastFrameCnt(0),
    m_submitIdPerFrame(0)
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
    ,m_pTrackedCmdBufferDeque(nullptr)
#endif
{
    if (m_pDevice->Settings().ifhGpuMask & (0x1 << m_pDevice->ChipProperties().gpuIndex))
    {
        m_ifhMode = m_pDevice->GetIfhMode();
    }
}

// =====================================================================================================================
Queue::~Queue()
{
    if (m_pQueueInfos != nullptr)
    {
        PAL_SAFE_DELETE_ARRAY(m_pQueueInfos, m_pDevice->GetPlatform());
    }
}

// =====================================================================================================================
// Queues must be careful to clean up their member classes before destructing because some of them may call one of the
// queues' virtual functions.
void Queue::Destroy()
{
    // NOTE: If there are still outstanding batched commands for this Queue, something has gone very wrong!
    PAL_ASSERT(m_batchedCmds.NumElements() == 0);

    // There are some CmdStreams which are created with UntrackedCmdAllocator, then the CmdStreamChunks in those
    // CmdStreams will have race condition when CmdStreams are destructed. Only CPU side reference count is used to
    // track chunks. Multi-queues share the same UntrackedCmdAllocator will have chance to overwrite command chunk
    // which is still not executed (but is marked as free by RemoveCommandStreamReference in the ~CmdStream() )
    // and can cause ASIC hang. This issue could be easily re-produced under SRIOV platform because virtual GPU is
    // slow and have chance to be preempted. Solution is call WaitIdle before doing anything else.
    WaitIdle();

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
    if (m_pTrackedCmdBufferDeque != nullptr)
    {
        while (m_pTrackedCmdBufferDeque->NumElements() > 0)
        {
            TrackedCmdBuffer* pTrackedCmdBuffer = nullptr;
            m_pTrackedCmdBufferDeque->PopFront(&pTrackedCmdBuffer);
            DestroyTrackedCmdBuffer(pTrackedCmdBuffer);
        }
        PAL_SAFE_DELETE(m_pTrackedCmdBufferDeque, m_pDevice->GetPlatform());
    }
#endif

    if (m_pDummyCmdBuffer != nullptr)
    {
        m_pDummyCmdBuffer->DestroyInternal();
        m_pDummyCmdBuffer = nullptr;
    }

    if (m_pQueueInfos != nullptr)
    {
        for (uint32 qIndex = 0; qIndex < m_queueCount; qIndex++)
        {
            if (m_pQueueInfos[qIndex].pQueueContext != nullptr)
            {
                m_pQueueInfos[qIndex].pQueueContext->Destroy();
                m_pQueueInfos[qIndex].pQueueContext = nullptr;
            }

            // When m_pInternalCopyQueue is created, m_pEngines has not been initilized.
            // Therefore, any of m_pQueueInfos[qIndex].pEngine of m_pInternalCopyQueue is nullptr.
            if (m_pQueueInfos[qIndex].pEngine != nullptr)
            {
                m_pQueueInfos[qIndex].pEngine->RemoveQueue(this);
            }
        }
    }

    if (m_deviceMembershipNode.InList())
    {
        m_pDevice->RemoveQueue(this);
    }

    if (m_pSubmissionContext != nullptr)
    {
        m_pSubmissionContext->ReleaseReference();
        m_pSubmissionContext = nullptr;
    }

    this->~Queue();
}

// =====================================================================================================================
// Initializes this Queue object's QueueContext and batched-command Mutex objects.
Result Queue::Init(
    const QueueCreateInfo* pCreateInfo,
    void*                  pContextPlacementAddr)
{
    Result result = Result::Success;
    m_pQueueInfos = PAL_NEW_ARRAY(SubQueueInfo, m_queueCount, m_pDevice->GetPlatform(), AllocInternal);
    if (m_pQueueInfos == nullptr)
    {
        result = Result::ErrorOutOfMemory;
    }

    if (result == Result::Success)
    {
        for (uint32 qIndex = 0; qIndex < m_queueCount; qIndex++)
        {
            memset(&m_pQueueInfos[qIndex], 0, sizeof(SubQueueInfo));
            m_pQueueInfos[qIndex].createInfo = pCreateInfo[qIndex];

            const EngineType curEngineType = m_pQueueInfos[qIndex].createInfo.engineType;
            const uint32 curEngineId       = m_pQueueInfos[qIndex].createInfo.engineIndex;

            m_pQueueInfos[qIndex].createInfo.submitOptMode =
                                 ((m_pDevice->Settings().submitOptModeOverride == 0) ?
                                   pCreateInfo[qIndex].submitOptMode                 :
                                   static_cast<SubmitOptMode>(m_pDevice->Settings().submitOptModeOverride - 1));

            m_pQueueInfos[qIndex].pEngine = m_pDevice->GetEngine(curEngineType, curEngineId);

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 530
            const Pal::EngineSubType engineSubType =
                m_pDevice->EngineProperties().perEngine[curEngineType].engineSubType[curEngineId];

            // Override the priority here.
            // RtCuHighCompute and Exclusive Compute are only supported on Windows for now.
            m_pQueueInfos[qIndex].createInfo.priority =
                (engineSubType == EngineSubType::RtCuHighCompute) ? QueuePriority::Realtime :
                (engineSubType == EngineSubType::RtCuMedCompute) ? QueuePriority::Medium :
                (curEngineType == EngineTypeExclusiveCompute) ? QueuePriority::High :
                pCreateInfo[qIndex].priority;
#endif

            if (m_pQueueInfos[qIndex].createInfo.priority != QueuePriority::Realtime)
            {
                // CU reservation is only supported on queues with realtime priority.
                m_pQueueInfos[qIndex].createInfo.numReservedCu = 0;
            }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 530
            if (engineSubType == EngineSubType::VrHighPriority)
            {
                m_pQueueInfos[qIndex].createInfo.dispatchTunneling = 1;
            }
#endif

            if (m_pDevice->EngineProperties().perEngine[curEngineType].flags.supportPersistentCeRam == 0)
            {
                PAL_ASSERT((pCreateInfo[qIndex].persistentCeRamOffset == 0) &&
                           (pCreateInfo[qIndex].persistentCeRamSize == 0));

                m_pQueueInfos[qIndex].createInfo.persistentCeRamOffset = 0;
                m_pQueueInfos[qIndex].createInfo.persistentCeRamSize = 0;
            }
            else
            {
                constexpr uint32 CeRamAlignBytes = 32;

                // Align the offset and size of persistent CE RAM to 32 bytes (8 DWORDs).
                m_pQueueInfos[qIndex].createInfo.persistentCeRamOffset =
                    Pow2AlignDown(pCreateInfo[qIndex].persistentCeRamOffset, CeRamAlignBytes);
                const uint32 difference =
                    (pCreateInfo[qIndex].persistentCeRamOffset -
                     m_pQueueInfos[qIndex].createInfo.persistentCeRamOffset);
                m_pQueueInfos[qIndex].createInfo.persistentCeRamSize =
                    static_cast<uint32>(
                        Pow2Align(((sizeof(uint32) * pCreateInfo[qIndex].persistentCeRamSize) + difference),
                            CeRamAlignBytes) / sizeof(uint32));

                PAL_ASSERT((m_pQueueInfos[qIndex].createInfo.persistentCeRamOffset ==
                            pCreateInfo[qIndex].persistentCeRamOffset) &&
                           (m_pQueueInfos[qIndex].createInfo.persistentCeRamSize == pCreateInfo[qIndex].persistentCeRamSize));

                // The client can request some part of the CE ram to be persistent through consecutive submissions,
                // and the whole CE ram used must be at least as big as that.
                PAL_ASSERT(m_pDevice->CeRamDwordsUsed(EngineTypeUniversal) >=
                           m_pQueueInfos[qIndex].createInfo.persistentCeRamOffset +
                           m_pQueueInfos[qIndex].createInfo.persistentCeRamSize);
            }

            m_pQueueInfos[qIndex].pQueueContext = nullptr;
        } // end of for loop
    }

    if (result == Result::Success)
    {
        result = m_batchedCmdsLock.Init();
    }

    if (result == Result::Success)
    {
        GfxDevice*  pGfxDevice = m_pDevice->GetGfxDevice();
        void* pNextQueueContextPlacementAddr = pContextPlacementAddr;
        // NOTE: OSSIP hardware is used for DMA Queues, GFXIP hardware is used for Compute & Universal Queues, and
        // no hardware block is used for Timer Queues since those are software-only.
        for (uint32 qIndex = 0; ((qIndex < m_queueCount) && (result == Result::Success)); qIndex++)
        {
            switch (m_pQueueInfos[qIndex].createInfo.queueType)
            {
            case QueueTypeCompute:
            case QueueTypeUniversal:
                if (pGfxDevice != nullptr)
                {
                    result = pGfxDevice->CreateQueueContext(
                                m_pQueueInfos[qIndex].createInfo,
                                m_pQueueInfos[qIndex].pEngine,
                                pNextQueueContextPlacementAddr,
                                &m_pQueueInfos[qIndex].pQueueContext);
                }
                else
                {
                    result = Result::ErrorIncompatibleDevice;
                }
                break;
            case QueueTypeDma:
            {
                if (m_pDevice->EngineProperties().perEngine[EngineTypeDma].numAvailable > 0)
                {
                    OssDevice* pOssDevice = m_pDevice->GetOssDevice();

                    if (pOssDevice != nullptr)
                    {
                        result = pOssDevice->CreateQueueContext(
                                    m_pQueueInfos[qIndex].createInfo.queueType,
                                    pNextQueueContextPlacementAddr,
                                    &m_pQueueInfos[qIndex].pQueueContext);
                    }
                    else if ((pGfxDevice != nullptr) && IsGfx10(*m_pDevice))
                    {
                        result = pGfxDevice->CreateQueueContext(
                                    m_pQueueInfos[qIndex].createInfo,
                                    m_pQueueInfos[qIndex].pEngine,
                                    pNextQueueContextPlacementAddr,
                                    &m_pQueueInfos[qIndex].pQueueContext);
                    }
                    else
                    {
                        result = Result::ErrorIncompatibleDevice;
                    }
                }
                else
                {
                    result = Result::ErrorIncompatibleDevice;
                }
            }
            break;
            case QueueTypeTimer:
                // For gang submit, we expect the queue type of any subQueue can be universalQueue,
                // ComputeQueue or SDMAQueue. In case the queue type is not any of the three, it indicates
                // that gang submit is disabled.
                m_pQueueInfos[qIndex].pQueueContext =
                    PAL_PLACEMENT_NEW(pNextQueueContextPlacementAddr) QueueContext(m_pDevice);
                break;
            default:
                // We shouldn't get here. It means someone tried to create a queue type we don't support.
                PAL_ASSERT_ALWAYS();
                result = Result::ErrorUnknown;
                break;
            } // end of switch

            pNextQueueContextPlacementAddr = VoidPtrInc(
                                             pNextQueueContextPlacementAddr,
                                             m_pDevice->QueueContextSize(m_pQueueInfos[qIndex].createInfo));
        } // end of for loop
    }

    // Skip the dummy command buffer on timer engines because there is no timer engine command buffer.
    if ((result == Result::Success) && (GetEngineType() != EngineTypeTimer))
    {
        CmdBufferCreateInfo createInfo = {};
        createInfo.pCmdAllocator = m_pDevice->InternalCmdAllocator(GetEngineType());
        createInfo.queueType     = Type();
        createInfo.engineType    = GetEngineType();

        CmdBufferInternalCreateInfo internalInfo = {};
        internalInfo.flags.isInternal = 1;

        result = m_pDevice->CreateInternalCmdBuffer(createInfo, internalInfo, &m_pDummyCmdBuffer);

        if (result == Result::Success)
        {
            CmdBufferBuildInfo buildInfo = {};
            buildInfo.flags.optimizeExclusiveSubmit = 1;
            result = m_pDummyCmdBuffer->Begin(buildInfo);

            if (result == Result::Success)
            {
                result = m_pDummyCmdBuffer->End();
            }
        }
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
    const bool shouldAllocTrackedCmdBuffers =
        m_pDevice->GetPlatform()->IsDeveloperModeEnabled();

    // Initialize internal tracked command buffers if they are required.
    if ((result == Result::Success) && shouldAllocTrackedCmdBuffers)
    {
        m_pTrackedCmdBufferDeque =
            PAL_NEW(TrackedCmdBufferDeque, m_pDevice->GetPlatform(), AllocInternal)
            (m_pDevice->GetPlatform());

        result = (m_pTrackedCmdBufferDeque != nullptr) ? Result::Success : Result::ErrorOutOfMemory;
    }
#endif

    return result;
}

// =====================================================================================================================
// Submits a set of command buffers for execution on this Queue.
Result Queue::SubmitInternal(
    const MultiSubmitInfo& submitInfo,
    bool                   postBatching)
{
    Result result = Result::Success;

    if (submitInfo.pPerSubQueueInfo == nullptr)
    {
        PAL_ASSERT(submitInfo.perSubQueueInfoCount == 0);
        result = Result::ErrorInvalidPointer;
    }

    if (result == Result::Success)
    {
        PAL_ASSERT(submitInfo.perSubQueueInfoCount <= m_queueCount);
        AutoBuffer<InternalSubmitInfo, 8, Platform> internalSubmitInfos(
            submitInfo.perSubQueueInfoCount, m_pDevice->GetPlatform());
        for (uint32 qIndex = 0; (qIndex < submitInfo.perSubQueueInfoCount) && (result == Result::Success); qIndex++)
        {
            for (uint32 idx = 0; idx < submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount; ++idx)
            {
                // Pre-process the command buffers before submission.
                // Command buffers that require building the commands at submission time should build them here.
                auto*const pCmdBuffer = static_cast<CmdBuffer*>(submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers[idx]);
                result = pCmdBuffer->PreSubmit();
            }
        }

        if (result == Result::Success)
        {
            result = ValidateSubmit(submitInfo);
        }

        if (result == Result::Success)
        {
            if (submitInfo.perSubQueueInfoCount > 0)
            {
                for (uint32 qIndex = 0; (qIndex < submitInfo.perSubQueueInfoCount) && (result == Result::Success); qIndex++)
                {
                    uint32 cmdBufferCount = submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount;
                    QueueContext* pQueueContext = m_pQueueInfos[qIndex].pQueueContext;
                    InternalSubmitInfo curInternalSubmitInfo = {};

                    result = pQueueContext->PreProcessSubmit(&curInternalSubmitInfo, cmdBufferCount);
                    internalSubmitInfos[qIndex] = curInternalSubmitInfo;
                }
            }
            else
            {
                result = m_pQueueInfos[0].pQueueContext->PreProcessSubmit(&internalSubmitInfos[0], 0);
            }
        }

#if PAL_ENABLE_PRINTS_ASSERTS
        if (result == Result::Success)
        {
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555
            if (IsCmdDumpEnabled())
            {
                Util::File logFile;
                // Open file for write depending on the settings
                const Result openResult = OpenCommandDumpFile(submitInfo, internalSubmitInfos[0], &logFile);

                if (openResult == Result::Success) // file opened correctly
                {
                    MultiSubmitInfo submitInfoCopy = submitInfo;

                    CmdDumpToFilePayload payload = {};
                    payload.pLogFile = &logFile;
                    payload.pSettings = &m_pDevice->Settings();

                    submitInfoCopy.pfnCmdDumpCb = WriteCmdDumpToFile;
                    submitInfoCopy.pUserData = &payload;

                    DumpCmdBuffers(submitInfoCopy, internalSubmitInfos[0]);
                }
            }
#else // PAL_CLIENT_INTERFACE_MAJOR_VERSION < 555
            // Dump command buffer
            DumpCmdToFile(submitInfo, internalSubmitInfos[0]);
#endif
        }
#endif // PAL_ENABLE_PRINTS_ASSERTS

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555
        if ((submitInfo.pfnCmdDumpCb != nullptr) && (result == Result::Success))
        {
            DumpCmdBuffers(submitInfo, internalSubmitInfos[0]);
        }
#endif // PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555

        if (result == Result::Success)
        {
            if (m_ifhMode == IfhModeDisabled)
            {
                for (uint32 qIndex = 0; (qIndex < submitInfo.perSubQueueInfoCount); qIndex++)
                {
                    for (uint32 idx = 0; idx < submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount; ++idx)
                    {
                        // Each command buffer being submitted needs to be notified about it, so
                        // the command stream(s) can manage their GPU-completion tracking.
                        auto*const pCmdBuffer = static_cast<CmdBuffer*>(
                            submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers[idx]);
                        pCmdBuffer->IncrementSubmitCount();
                    }
                }
            }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
            for (uint32 idx = 0; idx < submitInfo.fenceCount; idx++)
            {
                PAL_ASSERT(submitInfo.ppFences[idx] != nullptr);
                static_cast<Fence*>(submitInfo.ppFences[idx])->AssociateWithContext(m_pSubmissionContext);
            }
#else
            if (submitInfo.pFence != nullptr)
            {
                static_cast<Fence*>(submitInfo.pFence)->AssociateWithContext(m_pSubmissionContext);
            }
#endif

            // Either execute the submission immediately, or enqueue it for later, depending on whether or not we are
            // stalled and/or the caller is a function after the batching logic and thus must execute immediately.
            if (postBatching || (m_stalled == false))
            {
                result = OsSubmit(submitInfo, &internalSubmitInfos[0]);
            }
            else
            {
                result = EnqueueSubmit(submitInfo, &internalSubmitInfos[0]);
            }
        }

        if (result == Result::Success)
        {
            for (uint32 qIndex = 0; qIndex < submitInfo.perSubQueueInfoCount; qIndex++)
            {
                m_pQueueInfos[qIndex].pQueueContext->PostProcessSubmit();
            }
        }
    }

    return result;
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555
// =====================================================================================================================
// Calls DumpCmdStream on the preamble, postamble, and all the command streams in the submitInfo.
void Queue::DumpCmdBuffers(
    const MultiSubmitInfo&      submitInfo,
    const InternalSubmitInfo&   internalSubmitInfo
    ) const
{
    if (submitInfo.perSubQueueInfoCount > 0)
    {
        for (uint32 idx = 0; idx < internalSubmitInfo.numPreambleCmdStreams; ++idx)
        {
            const CmdStream* const pCmdStream = internalSubmitInfo.pPreambleCmdStream[idx];
            PAL_ASSERT(pCmdStream != nullptr);

            CmdBufferDumpDesc cmdBufferDesc = {};

            cmdBufferDesc.engineType = GetEngineType();
            cmdBufferDesc.queueType = Type();
            cmdBufferDesc.subEngineType = pCmdStream->GetSubEngineType();
            cmdBufferDesc.flags.isPreamble = 1;
            cmdBufferDesc.cmdBufferIdx = UINT32_MAX;

            DumpCmdStream(cmdBufferDesc, pCmdStream, submitInfo.pfnCmdDumpCb, submitInfo.pUserData);
        }

        for (uint32 idxCmdBuf = 0; idxCmdBuf < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; ++idxCmdBuf)
        {
            const CmdBuffer* const pCmdBuffer = static_cast<CmdBuffer*>(
                submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[idxCmdBuf]);
            PAL_ASSERT(pCmdBuffer != nullptr);

            for (uint32 idx = 0; idx < pCmdBuffer->NumCmdStreams(); ++idx)
            {
                const CmdStream* const pCmdStream = pCmdBuffer->GetCmdStream(idx);
                PAL_ASSERT(pCmdStream != nullptr);

                CmdBufferDumpDesc cmdBufferDesc = {};

                cmdBufferDesc.engineType = GetEngineType();
                cmdBufferDesc.queueType = Type();
                cmdBufferDesc.subEngineType = pCmdStream->GetSubEngineType();
                cmdBufferDesc.cmdBufferIdx = idxCmdBuf;

                DumpCmdStream(cmdBufferDesc, pCmdStream, submitInfo.pfnCmdDumpCb, submitInfo.pUserData);
            }
        }

        for (uint32 idx = 0; idx < internalSubmitInfo.numPostambleCmdStreams; ++idx)
        {
            const CmdStream* const pCmdStream = internalSubmitInfo.pPostambleCmdStream[idx];
            PAL_ASSERT(pCmdStream != nullptr);

            CmdBufferDumpDesc cmdBufferDesc = {};

            cmdBufferDesc.engineType = GetEngineType();
            cmdBufferDesc.queueType = Type();
            cmdBufferDesc.subEngineType = pCmdStream->GetSubEngineType();
            cmdBufferDesc.flags.isPostamble = 1;
            cmdBufferDesc.cmdBufferIdx = UINT32_MAX;

            DumpCmdStream(cmdBufferDesc, pCmdStream, submitInfo.pfnCmdDumpCb, submitInfo.pUserData);
        }
    }
}

// =====================================================================================================================
// Iterates though the chunks in the command stream and sends them to the callback function for dumping.
void Queue::DumpCmdStream(
    const CmdBufferDumpDesc& cmdBufferDesc,
    const CmdStream*         pCmdStream,
    CmdDumpCallback          pfnCmdDumpCb,
    void*                    pUserData
    ) const
{
    PAL_ASSERT(pfnCmdDumpCb != nullptr);

    // need to get the number of chunks in this stream for the auto buffer
    const uint32 numOfChunks = pCmdStream->GetNumChunks();

    AutoBuffer<CmdBufferChunkDumpDesc, 8, Platform> cmdBufferChunks(numOfChunks, m_pDevice->GetPlatform());

    // Walk through all the chunks that make up this command stream and add them to the chunk list
    for (auto iter = pCmdStream->GetFwdIterator(); iter.IsValid(); iter.Next())
    {
        const uint32 id                   = iter.Position();
        const CmdStreamChunk* const chunk = iter.Get();
        cmdBufferChunks[id].id            = id;
        cmdBufferChunks[id].pCommands     = chunk->WriteAddr();
        cmdBufferChunks[id].size          = chunk->DwordsAllocated() * static_cast<uint32>(sizeof(uint32));
    }

    pfnCmdDumpCb(cmdBufferDesc, cmdBufferChunks.Data(), numOfChunks, pUserData);
}

#if PAL_ENABLE_PRINTS_ASSERTS
// =====================================================================================================================
// Helper function to find out if command dumping to file at submit time is enabled.
bool Queue::IsCmdDumpEnabled() const
{
    // To dump the command buffer upon submission for the specified frames
    const auto& settings              = m_pDevice->Settings();
    const CmdBufDumpFormat dumpFormat = settings.cmdBufDumpFormat;
    const uint32 frameCnt             = m_pDevice->GetFrameCount();

    const bool cmdBufDumpEnabled = (m_pDevice->IsCmdBufDumpEnabled() ||
        ((frameCnt >= settings.submitTimeCmdBufDumpStartFrame) &&
         (frameCnt <= settings.submitTimeCmdBufDumpEndFrame)));

    return ((settings.cmdBufDumpMode == CmdBufDumpModeSubmitTime) && cmdBufDumpEnabled);
}

// =====================================================================================================================
// Opens the command buffer dump file and writes out the header according to settings.
Result Queue::OpenCommandDumpFile(
    const MultiSubmitInfo&      submitInfo,
    const InternalSubmitInfo&   internalSubmitInfo,
    Util::File*                 pLogFile)
{
    if (submitInfo.perSubQueueInfoCount > 0)
    {
        // To dump the command buffer upon submission for the specified frames
        const auto& settings = m_pDevice->Settings();
        const CmdBufDumpFormat dumpFormat = settings.cmdBufDumpFormat;

        static const char* const pSuffix[] =
        {
            ".txt",     // CmdBufDumpFormat::CmdBufDumpFormatText
            ".bin",     // CmdBufDumpFormat::CmdBufDumpFormatBinary
            ".pm4"      // CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders
        };

        const uint32 frameCnt = m_pDevice->GetFrameCount();
        const char* pLogDir = &settings.cmdBufDumpDirectory[0];

        // Create the directory. We don't care if it fails (existing is fine, failure is caught when opening the file).
        MkDir(pLogDir);

        // Maximum length of a filename allowed for command buffer dumps, seems more reasonable than 32
        constexpr uint32 MaxFilenameLength = 512;
        char filename[MaxFilenameLength] = {};

        // Multiple submissions of one frame
        if (m_lastFrameCnt == frameCnt)
        {
            m_submitIdPerFrame++;
        }
        else
        {
            // First submission of one frame
            m_submitIdPerFrame = 0;
        }

        // Add queue type and this pointer to file name to make name unique since there could be multiple queues/engines
        // and/or multiple vitual queues (on the same engine on) which command buffers are submitted
        Snprintf(filename, MaxFilenameLength, "%s/Frame_%u_%p_%u_%04u%s",
            pLogDir,
            Type(),
            this,
            frameCnt,
            m_submitIdPerFrame,
            pSuffix[dumpFormat]);

        m_lastFrameCnt = frameCnt;

        if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatText)
        {
            PAL_ALERT_MSG(pLogFile->Open(&filename[0], FileAccessMode::FileAccessWrite) != Result::Success,
                "Failed to open CmdBuf dump file '%s'", filename);
        }
        else if ((dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinary) ||
            (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders))
        {
            const uint32 fileMode = FileAccessMode::FileAccessWrite | FileAccessMode::FileAccessBinary;
            PAL_ALERT_MSG(pLogFile->Open(&filename[0], fileMode) != Result::Success,
                "Failed to open CmdBuf dump file '%s'", filename);

            if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders)
            {
                const CmdBufferDumpFileHeader fileHeader =
                {
                    static_cast<uint32>(sizeof(CmdBufferDumpFileHeader)), // Structure size
                    1,                                                    // Header version
                    m_pDevice->ChipProperties().familyId,                 // ASIC family
                    m_pDevice->ChipProperties().eRevId,                   // ASIC revision
                    0                                                     // Reserved
                };
                pLogFile->Write(&fileHeader, sizeof(fileHeader));
            }

            CmdBufferListHeader listHeader =
            {
                static_cast<uint32>(sizeof(CmdBufferListHeader)),   // Structure size
                EngineId(),                                         // Engine index
                0                                                   // Number of command buffer chunks
            };

            for (uint32 idxCmdBuf = 0; idxCmdBuf < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; ++idxCmdBuf)
            {
                PAL_ASSERT(submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[idxCmdBuf] != nullptr);
                const CmdBuffer* const pCmdBuffer = static_cast<CmdBuffer*>(
                    submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[idxCmdBuf]);

                for (uint32 idxStream = 0; idxStream < pCmdBuffer->NumCmdStreams(); ++idxStream)
                {
                    listHeader.count += pCmdBuffer->GetCmdStream(idxStream)->GetNumChunks();
                }
            }

            for (uint32 idx = 0; idx < internalSubmitInfo.numPreambleCmdStreams; ++idx)
            {
                PAL_ASSERT(internalSubmitInfo.pPreambleCmdStream[idx] != nullptr);
                listHeader.count += internalSubmitInfo.pPreambleCmdStream[idx]->GetNumChunks();
            }

            for (uint32 idx = 0; idx < internalSubmitInfo.numPostambleCmdStreams; ++idx)
            {
                PAL_ASSERT(internalSubmitInfo.pPostambleCmdStream[idx] != nullptr);
                listHeader.count += internalSubmitInfo.pPostambleCmdStream[idx]->GetNumChunks();
            }

            pLogFile->Write(&listHeader, sizeof(listHeader));
        }
        else
        {
            // If we get here, dumping is enabled, but it's not one of the modes listed above.
            // Perhaps someone added a new mode?
            PAL_ASSERT_ALWAYS();
        }

        return (pLogFile->IsOpen()) ? Result::Success : Result::ErrorInitializationFailed;
    }
    else
    {
        return Result::ErrorInitializationFailed;
    }
}

#endif // PAL_ENABLE_PRINTS_ASSERTS
#endif // PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 555

#if PAL_ENABLE_PRINTS_ASSERTS
// =====================================================================================================================
// Dumps a set of command buffers submitted on this Queue.
void Queue::DumpCmdToFile(
    const MultiSubmitInfo&    submitInfo,
    const InternalSubmitInfo& internalSubmitInfo)
{
    // To dump the command buffer upon submission for the specified frames
    const auto&            settings   = m_pDevice->Settings();
    const CmdBufDumpFormat dumpFormat = settings.cmdBufDumpFormat;

    static const char* const pSuffix[] =
    {
        ".txt",     // CmdBufDumpFormat::CmdBufDumpFormatText
        ".bin",     // CmdBufDumpFormat::CmdBufDumpFormatBinary
        ".pm4"      // CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders
    };

    const uint32 frameCnt = m_pDevice->GetFrameCount();
    const bool cmdBufDumpEnabled = (m_pDevice->IsCmdBufDumpEnabled() ||
                                    ((frameCnt >= settings.submitTimeCmdBufDumpStartFrame) &&
                                     (frameCnt <= settings.submitTimeCmdBufDumpEndFrame)));

    if ((settings.cmdBufDumpMode == CmdBufDumpModeSubmitTime) &&
        (submitInfo.perSubQueueInfoCount > 0)                 &&
        (cmdBufDumpEnabled))
    {
        const char* pLogDir = &settings.cmdBufDumpDirectory[0];
        // Create the directory. We don't care if it fails (existing is fine, failure is caught when opening the file).
        MkDir(pLogDir);

        // Maximum length of a filename allowed for command buffer dumps, seems more reasonable than 32
        constexpr uint32 MaxFilenameLength = 512;

        char filename[MaxFilenameLength] = {};
        File logFile;

        // Multiple submissions of one frame
        if (m_lastFrameCnt == frameCnt)
        {
            m_submitIdPerFrame++;
        }
        else
        {
            // First submission of one frame
            m_submitIdPerFrame = 0;
        }

        // Add queue type and this pointer to file name to make name unique since there could be multiple queues/engines
        // and/or multiple vitual queues (on the same engine on) which command buffers are submitted
        Snprintf(filename, MaxFilenameLength, "%s/Frame_%u_%p_%u_%04u%s",
                 pLogDir,
                 Type(),
                 this,
                 frameCnt,
                 m_submitIdPerFrame,
                 pSuffix[dumpFormat]);

        m_lastFrameCnt = frameCnt;

        if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatText)
        {
            PAL_ALERT_MSG(logFile.Open(&filename[0], FileAccessMode::FileAccessWrite) != Result::Success,
                          "Failed to open CmdBuf dump file '%s'", filename);
        }
        else if ((dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinary) ||
                 (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders))
        {
            const uint32 fileMode = FileAccessMode::FileAccessWrite | FileAccessMode::FileAccessBinary;
            PAL_ALERT_MSG(logFile.Open(&filename[0], fileMode) != Result::Success,
                          "Failed to open CmdBuf dump file '%s'", filename);

            if (dumpFormat == CmdBufDumpFormat::CmdBufDumpFormatBinaryHeaders)
            {
                const CmdBufferDumpFileHeader fileHeader =
                {
                    static_cast<uint32>(sizeof(CmdBufferDumpFileHeader)), // Structure size
                    1,                                                    // Header version
                    m_pDevice->ChipProperties().familyId,                 // ASIC family
                    m_pDevice->ChipProperties().eRevId,                   // ASIC revision
                    0                                                     // Reserved
                };
                logFile.Write(&fileHeader, sizeof(fileHeader));
            }

            CmdBufferListHeader listHeader =
            {
                static_cast<uint32>(sizeof(CmdBufferListHeader)),   // Structure size
                EngineId(),                                         // Engine index
                0                                                   // Number of command buffer chunks
            };

            // As a prototype, we don't dump cmdbuffers of other subQueus besides the master queue.
            for (uint32 idxCmdBuf = 0; idxCmdBuf < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; ++idxCmdBuf)
            {
                const auto*const pCmdBuffer =
                    static_cast<CmdBuffer*>(submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[idxCmdBuf]);

                for (uint32 idxStream = 0; idxStream < pCmdBuffer->NumCmdStreams(); ++idxStream)
                {
                    listHeader.count += pCmdBuffer->GetCmdStream(idxStream)->GetNumChunks();
                }
            }

            for (uint32 idx = 0; idx < internalSubmitInfo.numPreambleCmdStreams; ++idx)
            {
                PAL_ASSERT(internalSubmitInfo.pPreambleCmdStream[idx] != nullptr);
                listHeader.count += internalSubmitInfo.pPreambleCmdStream[idx]->GetNumChunks();
            }

            for (uint32 idx = 0; idx < internalSubmitInfo.numPostambleCmdStreams; ++idx)
            {
                PAL_ASSERT(internalSubmitInfo.pPostambleCmdStream[idx] != nullptr);
                listHeader.count += internalSubmitInfo.pPostambleCmdStream[idx]->GetNumChunks();
            }

            logFile.Write(&listHeader, sizeof(listHeader));
        }
        else
        {
            // If we get here, dumping is enabled, but it's not one of the modes listed above.
            // Perhaps someone added a new mode?
            PAL_ASSERT_ALWAYS();
        }

        const char* QueueTypeStrings[] =
        {
            "# Universal Queue - QueueContext Command length = ",
            "# Compute Queue - QueueContext Command length = ",
            "# DMA Queue - QueueContext Command length = ",
            "",
        };

        static_assert(ArrayLen(QueueTypeStrings) == static_cast<size_t>(QueueTypeCount),
                      "Mismatch between QueueTypeStrings array size and QueueTypeCount");

        for (uint32 idx = 0; idx < internalSubmitInfo.numPreambleCmdStreams; ++idx)
        {
            PAL_ASSERT(internalSubmitInfo.pPreambleCmdStream[idx] != nullptr);
            internalSubmitInfo.pPreambleCmdStream[idx]->DumpCommands(
                               &logFile,
                               QueueTypeStrings[Type()],
                               dumpFormat);
        }

        // As a prototype, we don't dump cmdbuffers of other subQueus besides the master queue.
        for (uint32 idxCmdBuf = 0; idxCmdBuf < submitInfo.pPerSubQueueInfo[0].cmdBufferCount; ++idxCmdBuf)
        {
            const auto*const pCmdBuffer =
                static_cast<CmdBuffer*>(submitInfo.pPerSubQueueInfo[0].ppCmdBuffers[idxCmdBuf]);
            pCmdBuffer->DumpCmdStreamsToFile(&logFile, dumpFormat);
        }

        for (uint32 idx = 0; idx < internalSubmitInfo.numPostambleCmdStreams; ++idx)
        {
            PAL_ASSERT(internalSubmitInfo.pPostambleCmdStream[idx] != nullptr);
            internalSubmitInfo.pPostambleCmdStream[idx]->DumpCommands(
                                                         &logFile,
                                                         QueueTypeStrings[Type()],
                                                         dumpFormat);
        }
    }
}
#endif

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
// =====================================================================================================================
Result Queue::CreateTrackedCmdBuffer(
    TrackedCmdBuffer** ppTrackedCmdBuffer)
{
    PAL_ASSERT(m_pTrackedCmdBufferDeque != nullptr);

    Result result = Result::Success;

    TrackedCmdBuffer* pTrackedCmdBuffer = nullptr;

    if ((m_pTrackedCmdBufferDeque->NumElements() == 0) ||
        (m_pTrackedCmdBufferDeque->Front()->pFence->GetStatus() == Result::NotReady))
    {
        pTrackedCmdBuffer = PAL_NEW(TrackedCmdBuffer, m_pDevice->GetPlatform(), AllocInternal);

        if (pTrackedCmdBuffer == nullptr)
        {
            result = Result::ErrorOutOfMemory;
        }

        if (result == Result::Success)
        {
            Pal::FenceCreateInfo createInfo = {};
            result = m_pDevice->CreateInternalFence(createInfo, &pTrackedCmdBuffer->pFence);
        }

        if (result == Result::Success)
        {
            CmdBufferCreateInfo createInfo = {};
            createInfo.pCmdAllocator = m_pDevice->InternalCmdAllocator(GetEngineType());
            createInfo.queueType     = Type();
            createInfo.engineType    = GetEngineType();
            createInfo.engineSubType =
                m_pDevice->EngineProperties().perEngine[GetEngineType()].engineSubType[EngineId()];

            CmdBufferInternalCreateInfo internalInfo = {};
            internalInfo.flags.isInternal            = 1;

            result = m_pDevice->CreateInternalCmdBuffer(createInfo, internalInfo, &pTrackedCmdBuffer->pCmdBuffer);
        }
    }
    else
    {
        result = m_pTrackedCmdBufferDeque->PopFront(&pTrackedCmdBuffer);
    }

    // Immediately push this command buffer onto the back of the deque to avoid leaking memory.
    if (result == Result::Success)
    {
        result = m_pTrackedCmdBufferDeque->PushBack(pTrackedCmdBuffer);
    }

    if ((result != Result::Success) && (pTrackedCmdBuffer != nullptr))
    {
        DestroyTrackedCmdBuffer(pTrackedCmdBuffer);
        pTrackedCmdBuffer = nullptr;
    }

    // Rebuild the command buffer with the assumption it will be used for a single submission.
    if (result == Result::Success)
    {
        CmdBufferBuildInfo buildInfo = {};
        buildInfo.flags.optimizeOneTimeSubmit = 1;

        result = pTrackedCmdBuffer->pCmdBuffer->Begin(buildInfo);
    }

    if (result == Result::Success)
    {
        *ppTrackedCmdBuffer = pTrackedCmdBuffer;
    }

    return result;
}

// =====================================================================================================================
Result Queue::SubmitTrackedCmdBuffer(
    TrackedCmdBuffer* pTrackedCmdBuffer,
    const GpuMemory*  pWrittenPrimary)
{
    Result result = pTrackedCmdBuffer->pCmdBuffer->End();

    if (result == Result::Success)
    {
        result = m_pDevice->ResetFences(1, reinterpret_cast<IFence**>(&pTrackedCmdBuffer->pFence));
    }

    if (result == Result::Success)
    {
        PerSubQueueSubmitInfo perSubQueueInfo = {};
        perSubQueueInfo.cmdBufferCount        = 1;
        perSubQueueInfo.ppCmdBuffers          = reinterpret_cast<ICmdBuffer**>(&pTrackedCmdBuffer->pCmdBuffer);

        MultiSubmitInfo submitInfo      = {};
        submitInfo.perSubQueueInfoCount = 1;
        submitInfo.pPerSubQueueInfo     = &perSubQueueInfo;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
        submitInfo.ppFences             = &pTrackedCmdBuffer->pFence;
        submitInfo.fenceCount           = 1;
#else
        submitInfo.pFence               = pTrackedCmdBuffer->pFence;
#endif
        if ((m_pDevice->GetPlatform()->GetProperties().supportBlockIfFlipping == 1) &&
            (pWrittenPrimary != nullptr) && (pWrittenPrimary->IsFlippable()))
        {
            submitInfo.ppBlockIfFlipping    = reinterpret_cast<const IGpuMemory**>(&pWrittenPrimary);
            submitInfo.blockIfFlippingCount = 1;
        }

        result = Submit(submitInfo);
    }

    return result;
}

// =====================================================================================================================
// Destroys a tracked command buffer object. It is the caller's responsibility to make sure that the command buffer has
// finished executing before calling this function and that it has been removed from the deque.
void Queue::DestroyTrackedCmdBuffer(
    TrackedCmdBuffer* pTrackedCmdBuffer)
{
    if (pTrackedCmdBuffer->pCmdBuffer != nullptr)
    {
        pTrackedCmdBuffer->pCmdBuffer->DestroyInternal();
    }

    if (pTrackedCmdBuffer->pFence != nullptr)
    {
        pTrackedCmdBuffer->pFence->DestroyInternal(m_pDevice->GetPlatform());
    }

    PAL_SAFE_DELETE(pTrackedCmdBuffer, m_pDevice->GetPlatform());
}
#endif

// =====================================================================================================================
// Waits for all requested submissions on this Queue to finish, including any batched-up submissions. This call never
// fails, but may wait awhile if the command buffers are long-running, or forever if the GPU is hung.) We do not wait
// for pending semaphore waits or delay operations.
// NOTE: Part of the public IQueue interface.
Result Queue::WaitIdle()
{
    Result result = Result::Success;

    // If this queue is blocked by a semaphore, this will spin loop until all batched submissions have been processed.
    while (m_batchedSubmissionCount > 0)
    {
        // Yield this CPU to give other threads a chance to run and so we don't burn too much power.
        YieldThread();
    }

    // When we get here, all batched operations (if there were any) have been processed, so wait for the OS-specific
    // Queue to become idle.
    return OsWaitIdle();
}

// =====================================================================================================================
// Signals the specified Semaphore using this Queue. The 'signal' operation will be executed by the GPU or OS scheduler
// when all previously-submitted work on this Queue has completed.
// NOTE: Part of the public IQueue interface.
Result Queue::SignalQueueSemaphoreInternal(
    IQueueSemaphore* pQueueSemaphore,
    uint64           value,
    bool             postBatching)
{
    QueueSemaphore*const pSemaphore = static_cast<QueueSemaphore*>(pQueueSemaphore);

    Result result = Result::Success;

    // Either signal the semaphore immediately, or enqueue it for later, depending on whether or not we are stalled
    // and/or the caller is a function after the batching logic and thus must execute immediately.
    if (postBatching || (m_stalled == false))
    {
        // The Semaphore object is responsible for notifying any stalled Queues which may get released by this signal
        // operation.
        result = pSemaphore->Signal(this, value);
    }
    else
    {
        // After taking the lock, check again to see if we're stalled. The original check which brought us down
        // this path didn't take the lock beforehand, so its possible that another thread released this Queue
        // from the stalled state before we were able to get into this method.
        MutexAuto lock(&m_batchedCmdsLock);
        if (m_stalled)
        {
            BatchedQueueCmdData cmdData  = { };
            cmdData.command              = BatchedQueueCmd::SignalSemaphore;
            cmdData.semaphore.pSemaphore = pQueueSemaphore;
            cmdData.semaphore.value      = value;

            result = m_batchedCmds.PushBack(cmdData);
        }
        else
        {
            result = pSemaphore->Signal(this, value);
        }
    }

    return result;
}

// =====================================================================================================================
// Waits on the specified Semaphore using this Queue. The 'wait' operation may be batched-up if the corresponding
// Signal has not been sent by the client yet. After this wait, all future GPU work submitted to this Queue will not
// execute until the Semaphore has been signaled by the GPU on another Queue.
// NOTE: Part of the public IQueue interface.
Result Queue::WaitQueueSemaphoreInternal(
    IQueueSemaphore* pQueueSemaphore,
    uint64           value,
    bool             postBatching)
{
    QueueSemaphore*const pSemaphore = static_cast<QueueSemaphore*>(pQueueSemaphore);

    Result result = Result::Success;

    // Either wait on the semaphore immediately, or enqueue it for later, depending on whether or not we are stalled
    // and/or the caller is a function after the batching logic and thus must execute immediately.
    if (postBatching || (m_stalled == false))
    {
        // If this Queue isn't stalled yet, we can execute the wait immediately (which, of course, could stall
        // this Queue).
        result = pSemaphore->Wait(this, value, &m_stalled);
    }
    else
    {
        // After taking the lock, check again to see if we're stalled. The original check which brought us down
        // this path didn't take the lock beforehand, so its possible that another thread released this Queue
        // from the stalled state before we were able to get into this method.
        MutexAuto lock(&m_batchedCmdsLock);
        if (m_stalled)
        {
            BatchedQueueCmdData cmdData  = { };
            cmdData.command              = BatchedQueueCmd::WaitSemaphore;
            cmdData.semaphore.pSemaphore = pQueueSemaphore;
            cmdData.semaphore.value      = value;

            result = m_batchedCmds.PushBack(cmdData);
        }
        else
        {
            result = pSemaphore->Wait(this, value, &m_stalled);
        }
    }

    return result;
}

// =====================================================================================================================
// Queues the specified image for presentation on the screen. All previous work done on this queue will complete before
// the image is displayed. If isClientPresent is true, this call came directly from the client and can be used to
// denote a frame boundary. In some cases PAL may make internal calls which do not denote frame boundaries.
Result Queue::PresentDirectInternal(
    const PresentDirectInfo& presentInfo,
    bool                     isClientPresent)
{

    Result result = Result::Success;

    // Check if our queue supports the given present mode.
    if (IsPresentModeSupported(presentInfo.presentMode) == false)
    {
        if ((presentInfo.presentMode == PresentMode::Windowed) && (m_pDevice->IsMasterGpu() == false))
        {
            result = Result::ErrorWindowedPresentUnavailable;
        }
        else
        {
            result = Result::ErrorUnavailable;
        }
    }
    else
    {
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
        // We only want to add postprocessing when this is a non-internal present. Internal presents are expected to
        // have done so themselves.
        if (((result == Result::Success) && isClientPresent) && (presentInfo.pSrcImage != nullptr))
        {
            result = SubmitPostprocessCmdBuffer(static_cast<Image&>(*presentInfo.pSrcImage));
        }
#endif

        if (result == Result::Success)
        {
            // Either execute the present immediately, or enqueue it for later, depending on whether or not we are
            // stalled.
            if (m_stalled == false)
            {
                result = OsPresentDirect(presentInfo);
            }
            else
            {
                // After taking the lock, check again to see if we're stalled. The original check which brought us down
                // this path didn't take the lock beforehand, so its possible that another thread released this Queue
                // from the stalled state before we were able to get into this method.
                MutexAuto lock(&m_batchedCmdsLock);
                if (m_stalled)
                {
                    BatchedQueueCmdData cmdData = {};
                    cmdData.command             = BatchedQueueCmd::PresentDirect;
                    cmdData.presentDirect.info  = presentInfo;

                    result = m_batchedCmds.PushBack(cmdData);
                }
                else
                {
                    result = OsPresentDirect(presentInfo);
                }
            }
        }
    }

    // Increment our frame counter if this present denotes a frame boundary.
    if (isClientPresent)
    {
        m_pDevice->IncFrameCount();
    }

    return result;
}

// =====================================================================================================================
// Queues the specified image for presentation on the screen. All previous work done on this queue will complete before
// the image is displayed.
// NOTE: Part of the public IQueue interface.
Result Queue::PresentSwapChain(
    const PresentSwapChainInfo& presentInfo)
{
    Result result = Result::Success;

    const auto*const  pSrcImage       = static_cast<const Image*>(presentInfo.pSrcImage);
    const Image*const pPresentedImage =
        pSrcImage;

    auto*const pSwapChain = static_cast<SwapChain*>(presentInfo.pSwapChain);

    // Validate the present info. If this succeeds we must always call into the swap chain to release ownership of the
    // image index. Otherwise, the application will deadlock on AcquireNextImage at some point in the future.
    if ((pSrcImage == nullptr) || (pSwapChain == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else if ((pPresentedImage->IsPresentable() == false) ||
             ((presentInfo.presentMode == PresentMode::Fullscreen) && (pPresentedImage->IsFlippable() == false)) ||
             (presentInfo.imageIndex >= pSwapChain->CreateInfo().imageCount))
    {
        result = Result::ErrorInvalidValue;
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
    if (result == Result::Success)
    {
        result = SubmitPostprocessCmdBuffer(static_cast<Image&>(*presentInfo.pSrcImage));
    }
#endif

    if (presentInfo.flags.notifyOnly == 0)
    {
        if (result == Result::Success)
        {
            // Always execute the Present immediately, even if we are stalled. We should (and can) do this because:
            // - The swap chain and present scheduler Present code was designed to only issue batchable queue
            //   operations.
            // - We must release the given swap chain image index before this function returns.
            result = pSwapChain->Present(presentInfo, this);
        }

        IncFrameCount();
    }

    return result;
}

// =====================================================================================================================
// Inserts a delay of a specified amount of time before processing more commands on this queue. Only supported on Timer
// Queues.
// NOTE: Part of the public IQueue interface.
Result Queue::Delay(
    float delay)
{
    Result result = Result::ErrorUnavailable;

    if (Type() == QueueTypeTimer)
    {
        // Either execute the delay immediately, or enqueue it for later, depending on whether or not we are stalled.
        if (m_stalled == false)
        {
            result = OsDelay(delay, nullptr);
        }
        else
        {
            // After taking the lock, check again to see if we're stalled. The original check which brought us down
            // this path didn't take the lock beforehand, so its possible that another thread released this Queue
            // from the stalled state before we were able to get into this method.
            MutexAuto lock(&m_batchedCmdsLock);
            if (m_stalled)
            {
                BatchedQueueCmdData cmdData = { };
                cmdData.command    = BatchedQueueCmd::Delay;
                cmdData.delay.time = delay;

                result = m_batchedCmds.PushBack(cmdData);
            }
            else
            {
                result = OsDelay(delay, nullptr);
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Inserts a delay of a specified amount of time after a vsync on a private screen. Only supported on Timer Queues.
// NOTE: Part of the public IQueue interface.
Result Queue::DelayAfterVsync(
    float                 delayInUs,
    const IPrivateScreen* pScreen)
{
    Result result = Result::ErrorUnavailable;

    if (Type() == QueueTypeTimer)
    {
        // Either execute the delay immediately, or enqueue it for later, depending on whether or not we are stalled.
        if (m_stalled == false)
        {
            result = OsDelay(delayInUs, pScreen);
        }
        else
        {
            // After taking the lock, check again to see if we're stalled. The original check which brought us down
            // this path didn't take the lock beforehand, so its possible that another thread released this Queue
            // from the stalled state before we were able to get into this method.
            MutexAuto lock(&m_batchedCmdsLock);
            if (m_stalled == false)
            {
                result = OsDelay(delayInUs, pScreen);
            }
            else
            {
                // NOTE: Currently there shouldn't be a use case that queue is blocked as external semaphore is used to
                // synchronize submissions in DX and timer queue delays in Mantle, thus application is responsible for
                // correct pairing. Even in case the queue is stalled (in future), we don't want to queue a delay-after-
                // vsync but simply returns an error code to the application.
                PAL_ALERT_ALWAYS();
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Associates the given fence with the last submit before processing more commands on this queue.
// NOTE: Part of the public IQueue interface.
Result Queue::AssociateFenceWithLastSubmit(
    IFence* pFence)
{
    Result result = Result::ErrorInvalidPointer;

    if (pFence != nullptr)
    {
        auto*const pCoreFence = static_cast<Fence*>(pFence);

        // Associate fence with this queue's submission context.
        pCoreFence->AssociateWithContext(m_pSubmissionContext);

        // Either associate the fence timestamp immediately or later, depending on whether or not we are stalled.
        if (m_stalled == false)
        {
            result = DoAssociateFenceWithLastSubmit(pCoreFence);
        }
        else
        {
            // After taking the lock, check again to see if we're stalled. The original check which brought us down
            // this path didn't take the lock beforehand, so its possible that another thread released this Queue
            // from the stalled state before we were able to get into this method.
            MutexAuto lock(&m_batchedCmdsLock);

            if (m_stalled)
            {
                BatchedQueueCmdData cmdData = { };
                cmdData.command               = BatchedQueueCmd::AssociateFenceWithLastSubmit;
                cmdData.associateFence.pFence = pCoreFence;

                result = m_batchedCmds.PushBack(cmdData);
            }
            else
            {
                result = DoAssociateFenceWithLastSubmit(pCoreFence);
            }
        }
    }

    return result;
}

// =====================================================================================================================
// This must be called right after initialization to allow the queue to perform any initialization work which
// requires a fully initialized queue.
Result Queue::LateInit()
{
    Result result = Result::Success;

    result = m_pDevice->AddQueue(this);

    // It's possible that we add this queue to the same engine instance more than once.
    for (uint32 i = 0; ((i < m_queueCount) && (result == Result::Success)); i++)
    {
        if (m_pQueueInfos[i].pEngine != nullptr)
        {
            result = m_pQueueInfos[i].pEngine->AddQueue(this);
        }
    }

    // Dummy submission must be called after m_pDevice->AddQueue to add internal memory reference.
    // We won't have a dummy command buffer available if we're on a timer queue so we need to check first.
    if ((result == Result::Success) && (m_pDummyCmdBuffer != nullptr))
    {
        // If ProcessInitialSubmit returns Success, we need to perform a dummy submit with special preambles
        // to initialize the queue. Otherwise, it's not required for this queue.
        uint32 initialSubmitCount = 0;
        Platform* pPlatform = static_cast<Platform*>(m_pDevice->GetPlatform());
        AutoBuffer<InternalSubmitInfo, 8, Platform> internalSubmitInfos(m_queueCount, pPlatform);
        AutoBuffer<PerSubQueueSubmitInfo, 8, Platform> subQueueInfos(m_queueCount, pPlatform);

        for (uint32 qIndex = 0; qIndex < m_queueCount; qIndex++)
        {
            InternalSubmitInfo internalSubmitInfo = {};
            PerSubQueueSubmitInfo perSubQueueInfo = {};
            if (m_pQueueInfos[qIndex].pQueueContext->ProcessInitialSubmit(&internalSubmitInfo) == Result::Success)
            {
                initialSubmitCount++;
                perSubQueueInfo.cmdBufferCount = 1;
                // Do I need to create a independant DummyCmdBuffer for each universal subQueue?
                perSubQueueInfo.ppCmdBuffers = reinterpret_cast<Pal::ICmdBuffer* const*>(&m_pDummyCmdBuffer);
                PAL_ASSERT(perSubQueueInfo.pCmdBufInfoList == nullptr);

            }
            internalSubmitInfos[qIndex] = internalSubmitInfo;
            subQueueInfos[qIndex]       = perSubQueueInfo;
        }

        if (initialSubmitCount > 0)
        {
            MultiSubmitInfo submitInfo      = {};
            submitInfo.perSubQueueInfoCount = m_queueCount;
            submitInfo.pPerSubQueueInfo     = &subQueueInfos[0];
            if (m_ifhMode == IfhModeDisabled)
            {
                m_pDummyCmdBuffer->IncrementSubmitCount();
            }
            result = OsSubmit(submitInfo, &internalSubmitInfos[0]);
        }
    }

    return result;
}

// =====================================================================================================================
// Used to notify this Queue that it has been released by one of the Semaphores that it has been stalled by. If this
// Queue is no longer stalled by any Semaphores, then this will start executing any commands batched on this Queue.
//
// NOTE: This method is invoked whenever a QueueSemaphore which was blocking this Queue becomes signaled, and needs
// "wake up" the blocked Queue. Since the blocking Semaphore can be signaled on a separate thread from threads which
// are batching-up more Queue commands, a race condition can exist while accessing the list of batched-up commands.
// (which is why we need m_batchedCmdsLock).
Result Queue::ReleaseFromStalledState()
{
    Result result = Result::Success;

    bool stalledAgain = false; // It is possible for one of the batched-up commands to be a Semaphore wait which
                               // may cause this Queue to become stalled once more.

    MutexAuto lock(&m_batchedCmdsLock);

    // Execute all of the batched-up commands as long as we don't become stalled again and don't encounter an error.
    while ((m_batchedCmds.NumElements() > 0) && (stalledAgain == false) && (result == Result::Success))
    {
        BatchedQueueCmdData cmdData = { };

        result = m_batchedCmds.PopFront(&cmdData);
        PAL_ASSERT(result == Result::Success);

        switch (cmdData.command)
        {
        case BatchedQueueCmd::Submit:
            result = OsSubmit(cmdData.submit.submitInfo, cmdData.submit.pInternalSubmitInfo);

            // Once we've executed the submission, we need to free the submission's dynamic arrays. They are all stored
            // in the same memory allocation which was saved in pDynamicMem for convenience.
            PAL_SAFE_FREE(cmdData.submit.pDynamicMem, m_pDevice->GetPlatform());

            // Decrement this count to permit WaitIdle to query the status of the queue's submissions.
            PAL_ASSERT(m_batchedSubmissionCount > 0);
            AtomicDecrement(&m_batchedSubmissionCount);
            break;

        case BatchedQueueCmd::SignalSemaphore:
            result = static_cast<QueueSemaphore*>(cmdData.semaphore.pSemaphore)->Signal(this, cmdData.semaphore.value);
            break;

        case BatchedQueueCmd::WaitSemaphore:
            result = static_cast<QueueSemaphore*>(cmdData.semaphore.pSemaphore)->Wait(this, cmdData.semaphore.value,
                                                                                      &stalledAgain);
            break;

        case BatchedQueueCmd::PresentDirect:
            result = OsPresentDirect(cmdData.presentDirect.info);
            break;

        case BatchedQueueCmd::Delay:
            PAL_ASSERT(Type() == QueueTypeTimer);
            result = OsDelay(cmdData.delay.time, nullptr);
            break;

        case BatchedQueueCmd::AssociateFenceWithLastSubmit:
            result = DoAssociateFenceWithLastSubmit(cmdData.associateFence.pFence);
            break;

        }
    }

    // Update our stalled status: either we've completely drained all batched-up commands and are not stalled, or
    // one of the batched-up commands caused this Queue to become stalled again.
    m_stalled = stalledAgain;

    return result;
}

// =====================================================================================================================
// Validates that the inputs to a Submit() call are legal according to the conditions defined in palQueue.h.
Result Queue::ValidateSubmit(
    const MultiSubmitInfo& submitInfo
    ) const
{
    Result result = Result::Success;

    if (Type() == QueueTypeTimer)
    {
        result = Result::ErrorUnavailable;
    }
    else if (((submitInfo.gpuMemRefCount > 0) && (submitInfo.pGpuMemoryRefs == nullptr)) ||
             ((submitInfo.doppRefCount > 0)   && (submitInfo.pDoppRefs == nullptr))      ||
             ((submitInfo.blockIfFlippingCount > 0) && (submitInfo.ppBlockIfFlipping == nullptr))
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
             ||
             ((submitInfo.fenceCount > 0)           && (submitInfo.ppFences         == nullptr))
#endif
            )
    {
        result = Result::ErrorInvalidPointer;
    }
    else if ((submitInfo.blockIfFlippingCount > MaxBlockIfFlippingCount) ||
        ((submitInfo.blockIfFlippingCount > 0) &&
        (m_pDevice->GetPlatform()->GetProperties().supportBlockIfFlipping == 0)))
    {
        result = Result::ErrorInvalidValue;
    }
    else if ((submitInfo.perSubQueueInfoCount > 0) && (submitInfo.pPerSubQueueInfo == nullptr))
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        for (uint32 qIndex = 0; (qIndex < submitInfo.perSubQueueInfoCount) && (result == Result::Success); qIndex++)
        {
            PAL_ASSERT(submitInfo.perSubQueueInfoCount <= m_queueCount);
            if ((submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount > 0) &&
                (submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers == nullptr))
            {
                result = Result::ErrorInvalidPointer;
            }
            else
            {
                for (uint32 idx = 0; idx < submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount; ++idx)
                {
                    const auto*const pCmdBuffer =
                        static_cast<CmdBuffer*>(submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers[idx]);
                    if (pCmdBuffer == nullptr)
                    {
                        result = Result::ErrorInvalidPointer;
                        break;
                    }
                    else if (pCmdBuffer->RecordState() != CmdBufferRecordState::Executable)
                    {
                        result = Result::ErrorIncompleteCommandBuffer;
                        break;
                    }
                    else if (pCmdBuffer->GetQueueType() != Type())
                    {
                        result = Result::ErrorIncompatibleQueue;
                        break;
                    }

                    PAL_ASSERT(pCmdBuffer->IsNested() == false);
                }
            }
        }
    }

    if (result == Result::Success)
    {
        for (uint32 idx = 0; idx < submitInfo.gpuMemRefCount; ++idx)
        {
            if (submitInfo.pGpuMemoryRefs[idx].pGpuMemory == nullptr)
            {
                result = Result::ErrorInvalidPointer;
                break;
            }
        }
    }

    if (result == Result::Success)
    {
        for (uint32 idx = 0; idx < submitInfo.doppRefCount; ++idx)
        {
            if (submitInfo.pDoppRefs[idx].pGpuMemory == nullptr)
            {
                result = Result::ErrorInvalidPointer;
                break;
            }
        }
    }

    if (result == Result::Success)
    {
        for (uint32 idx = 0; idx < submitInfo.blockIfFlippingCount; ++idx)
        {
            if (submitInfo.ppBlockIfFlipping[idx] == nullptr)
            {
                result = Result::ErrorInvalidPointer;
                break;
            }
        }
    }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
    if (result == Result::Success)
    {
        for (uint32 idx = 0; idx < submitInfo.fenceCount; ++idx)
        {
            if (submitInfo.ppFences[idx] == nullptr)
            {
                result = Result::ErrorInvalidPointer;
                break;
            }
        }
    }
#endif

    return result;
}

// =====================================================================================================================
// Enqueues a command buffer submission for later execution, once this Queue is no longer blocked by any Semaphores.
Result Queue::EnqueueSubmit(
    const MultiSubmitInfo&    submitInfo,
    const InternalSubmitInfo* pInternalSubmitInfo)
{
    Result result = Result::Success;

    // After taking the lock, check again to see if we're stalled. The original check which brought us down this path
    // didn't take the lock beforehand, so its possible that another thread released this Queue from the stalled state
    // before we were able to get into this method.
    MutexAuto lock(&m_batchedCmdsLock);
    if (m_stalled)
    {
        BatchedQueueCmdData cmdData;
        cmdData.command                    = BatchedQueueCmd::Submit;
        cmdData.submit.submitInfo          = submitInfo;
        cmdData.submit.pInternalSubmitInfo = pInternalSubmitInfo;
        cmdData.submit.pDynamicMem         = nullptr;

        // The submitInfo structure we are batching-up needs to have its own copies of the command buffer and memory
        // reference lists, because there's no guarantee those user arrays will remain valid once we become unstalled.
        size_t totalCmdBufBytes     = 0;
        size_t totalCmdBufInfoBytes = 0;
        const size_t totalPerSubQueueInfoBytes = sizeof(PerSubQueueSubmitInfo) * submitInfo.perSubQueueInfoCount;
        // The submitInfo structure we are batching-up needs to have its own copies of the command buffer and memory
        // reference lists, because there's no guarantee those user arrays will remain valid once we become unstalled.

        AutoBuffer<size_t, 8, Platform> cmdBufListBytes(submitInfo.perSubQueueInfoCount, m_pDevice->GetPlatform());
        AutoBuffer<size_t, 8, Platform> cmdBufInfoListBytes(submitInfo.perSubQueueInfoCount, m_pDevice->GetPlatform());

        for (uint32 qIndex = 0; qIndex < submitInfo.perSubQueueInfoCount; qIndex++)
        {
            cmdBufListBytes[qIndex] = (sizeof(ICmdBuffer*) * submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount);
            totalCmdBufBytes += cmdBufListBytes[qIndex];

            cmdBufInfoListBytes[qIndex] = 0;
            if ((submitInfo.pPerSubQueueInfo[qIndex].pCmdBufInfoList != nullptr) &&
                (submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount > 0))
            {
                cmdBufInfoListBytes[qIndex] =
                    (sizeof(CmdBufInfo) * submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount);
            }
            totalCmdBufInfoBytes += cmdBufInfoListBytes[qIndex];
        }
        const size_t memRefListBytes  = (sizeof(GpuMemoryRef) * submitInfo.gpuMemRefCount);
        const size_t blkIfFlipBytes   = (sizeof(IGpuMemory*)  * submitInfo.blockIfFlippingCount);
        const size_t doppRefListBytes = (sizeof(DoppRef) * submitInfo.doppRefCount);
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
        const size_t fenceListBytes   = (sizeof(IFence*) * submitInfo.fenceCount);
#endif
        const size_t internalSubmitInfoListBytes = (sizeof(InternalSubmitInfo) * submitInfo.perSubQueueInfoCount);

        const size_t totalBytes = (
                            totalPerSubQueueInfoBytes +
                            totalCmdBufBytes +
                            memRefListBytes +
                            doppRefListBytes +
                            blkIfFlipBytes +
                            totalCmdBufInfoBytes
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
                            +fenceListBytes
#endif
                            + internalSubmitInfoListBytes
                            );

        if (totalBytes > 0)
        {
            cmdData.submit.pDynamicMem = PAL_MALLOC(totalBytes, m_pDevice->GetPlatform(), AllocInternal);

            if (cmdData.submit.pDynamicMem == nullptr)
            {
                result = Result::ErrorOutOfMemory;
            }
            else
            {
                void* pNextBuffer = cmdData.submit.pDynamicMem;

                PerSubQueueSubmitInfo* pPerSubQueueInfoList = static_cast<PerSubQueueSubmitInfo*>(pNextBuffer);
                cmdData.submit.submitInfo.pPerSubQueueInfo = pPerSubQueueInfoList;
                pNextBuffer = VoidPtrInc(pNextBuffer, totalPerSubQueueInfoBytes);

                for (uint32 qIndex = 0; qIndex < submitInfo.perSubQueueInfoCount; qIndex++)
                {
                    pPerSubQueueInfoList[qIndex].cmdBufferCount = submitInfo.pPerSubQueueInfo[qIndex].cmdBufferCount;

                    if (pPerSubQueueInfoList[qIndex].cmdBufferCount > 0)
                    {
                        auto**const ppBatchedCmdBuffers = reinterpret_cast<ICmdBuffer**>(pNextBuffer);
                        memcpy(ppBatchedCmdBuffers,
                               submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers,
                               cmdBufListBytes[qIndex]);
                        pPerSubQueueInfoList[qIndex].ppCmdBuffers = ppBatchedCmdBuffers;
                        pNextBuffer = VoidPtrInc(pNextBuffer, cmdBufListBytes[qIndex]);
                    }
                    else
                    {
                        pPerSubQueueInfoList[qIndex].ppCmdBuffers = submitInfo.pPerSubQueueInfo[qIndex].ppCmdBuffers;
                    }
                }

                if (submitInfo.gpuMemRefCount > 0)
                {
                    auto*const pBatchedGpuMemoryRefs = static_cast<GpuMemoryRef*>(pNextBuffer);
                    memcpy(pBatchedGpuMemoryRefs, submitInfo.pGpuMemoryRefs, memRefListBytes);

                    cmdData.submit.submitInfo.pGpuMemoryRefs = pBatchedGpuMemoryRefs;
                    pNextBuffer                              = VoidPtrInc(pNextBuffer, memRefListBytes);
                }

                if (submitInfo.doppRefCount > 0)
                {
                    auto*const pBatchedDoppRefs = static_cast<DoppRef*>(pNextBuffer);
                    memcpy(pBatchedDoppRefs, submitInfo.pDoppRefs, doppRefListBytes);

                    cmdData.submit.submitInfo.pDoppRefs = pBatchedDoppRefs;
                    pNextBuffer                         = VoidPtrInc(pNextBuffer, doppRefListBytes);
                }

                if (submitInfo.blockIfFlippingCount > 0)
                {
                    auto**const ppBatchedBlockIfFlipping = static_cast<IGpuMemory**>(pNextBuffer);
                    memcpy(ppBatchedBlockIfFlipping, submitInfo.ppBlockIfFlipping, blkIfFlipBytes);

                    cmdData.submit.submitInfo.ppBlockIfFlipping = ppBatchedBlockIfFlipping;
                    pNextBuffer                                 = VoidPtrInc(pNextBuffer, blkIfFlipBytes);
                }

                for (uint32 qIndex = 0; qIndex < submitInfo.perSubQueueInfoCount; qIndex++)
                {
                    // It's possible that pCmdBufInfoList is nullptr, while cmdBufferCount is larger than 0.
                    if ((submitInfo.pPerSubQueueInfo[qIndex].pCmdBufInfoList != nullptr) &&
                        (pPerSubQueueInfoList[qIndex].cmdBufferCount > 0))
                    {
                        auto*const pBatchedCmdBufInfoList = static_cast<CmdBufInfo*>(pNextBuffer);
                        memcpy(pBatchedCmdBufInfoList,
                               submitInfo.pPerSubQueueInfo[qIndex].pCmdBufInfoList,
                               cmdBufInfoListBytes[qIndex]);
                        pPerSubQueueInfoList[qIndex].pCmdBufInfoList = pBatchedCmdBufInfoList;
                        pNextBuffer = VoidPtrInc(pNextBuffer, cmdBufInfoListBytes[qIndex]);
                    }
                    else
                    {
                        pPerSubQueueInfoList[qIndex].pCmdBufInfoList =
                            submitInfo.pPerSubQueueInfo[qIndex].pCmdBufInfoList;
                    }
                }

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
                if (submitInfo.fenceCount > 0)
                {
                    auto**const ppBatchedFences = static_cast<IFence**>(pNextBuffer);
                    memcpy(ppBatchedFences, submitInfo.ppFences, fenceListBytes);

                    cmdData.submit.submitInfo.ppFences = ppBatchedFences;
                    pNextBuffer = VoidPtrInc(pNextBuffer, fenceListBytes);
                }
#endif

                PAL_ASSERT(submitInfo.perSubQueueInfoCount > 0);
                auto*const pBatchedInternalSubmitInfos = static_cast<InternalSubmitInfo*>(pNextBuffer);
                memcpy(pBatchedInternalSubmitInfos, pInternalSubmitInfo, internalSubmitInfoListBytes);
                cmdData.submit.pInternalSubmitInfo = pBatchedInternalSubmitInfos;
                pNextBuffer = VoidPtrInc(pNextBuffer, internalSubmitInfoListBytes);

            } // Space of cmdData.submit.pDynamicMem is allocated successfully.
        }

        if (result == Result::Success)
        {
            result = m_batchedCmds.PushBack(cmdData);

            if (result == Result::Success)
            {
                // We must track the number of batched submissions to make WaitIdle spin until all submissions have been
                // submitted to the OS layer.
                AtomicIncrement(&m_batchedSubmissionCount);
            }
            else
            {
                PAL_SAFE_FREE(cmdData.submit.pDynamicMem, m_pDevice->GetPlatform());
            }
        }
    }
    else
    {
        // We had a false-positive and aren't really stalled. Submit immediately.
        result = OsSubmit(submitInfo, pInternalSubmitInfo);
    }

    return result;
}

// =====================================================================================================================
Result Queue::QueryAllocationInfo(
    size_t*                    pNumEntries,
    GpuMemSubAllocInfo* const  pAllocInfoList)
{
    Result result = Result::Success;

    if (pNumEntries != nullptr)
    {
        *pNumEntries = 0;
    }
    else
    {
        result = Result::ErrorInvalidPointer;
    }

    return result;
}

// =====================================================================================================================
// Performs a queue submit with zero command buffer count and the fence provided.
Result Queue::SubmitFence(
    IFence* pFence)
{
    PerSubQueueSubmitInfo perSubQueueInfo = {};
    perSubQueueInfo.cmdBufferCount = 0;

    MultiSubmitInfo submitInfo      = {};
    submitInfo.perSubQueueInfoCount = 1;
    submitInfo.pPerSubQueueInfo     = &perSubQueueInfo;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 568
    submitInfo.ppFences             = &pFence;
    submitInfo.fenceCount           = 1;
#else
    submitInfo.pFence               = pFence;
#endif
    return SubmitInternal(submitInfo, false);
}

// =====================================================================================================================
// Increment frame count and move to next frame
void Queue::IncFrameCount()
{
    m_pDevice->IncFrameCount();
}

#if PAL_CLIENT_INTERFACE_MAJOR_VERSION < 518
// =====================================================================================================================
// Applies developer overlay and other postprocessing to be done prior to presenting an image.
Result Queue::SubmitPostprocessCmdBuffer(
    const Image& image)
{
    Result result = Result::Success;

    const bool shouldPostprocess =
        (Queue::SupportsComputeShader(Type()) &&
         (
          m_pDevice->GetPlatform()->ShowDevDriverOverlay()));

    if (shouldPostprocess)
    {
        const auto& presentedImage =
            image;

        TrackedCmdBuffer* pTrackedCmdBuffer = nullptr;
        result = CreateTrackedCmdBuffer(&pTrackedCmdBuffer);

        if (result == Result::Success)
        {

            // If developer mode is enabled, we need to apply the developer overlay.
            if (m_pDevice->GetPlatform()->ShowDevDriverOverlay())
            {
                m_pDevice->ApplyDevOverlay(presentedImage, pTrackedCmdBuffer->pCmdBuffer);
            }

            result = SubmitTrackedCmdBuffer(pTrackedCmdBuffer, presentedImage.GetBoundGpuMemory().Memory());
        }
    }

    return result;
}
#endif

// =====================================================================================================================
// Check whether the present mode is supported by the queue.
bool Queue::IsPresentModeSupported(
    PresentMode presentMode
    ) const
{
    const uint32 supportedPresentModes = m_pDevice->QueueProperties().perQueue[Type()].supportedDirectPresentModes;
    const uint32 presentModeFlag       =
        (presentMode == PresentMode::Fullscreen) ? SupportFullscreenPresent :
        (m_pQueueInfos[0].createInfo.windowedPriorBlit == 1) ? SupportWindowedPriorBlitPresent :
                                                               SupportWindowedPresent;
    return TestAnyFlagSet(supportedPresentModes, presentModeFlag);
}

// =====================================================================================================================
// Perform a dummy submission on this queue.
Result Queue::DummySubmit(
    bool  postBatching)
{
    ICmdBuffer*const pCmdBuffer = DummyCmdBuffer();

    PerSubQueueSubmitInfo perSubQueueInfo = {};
    perSubQueueInfo.cmdBufferCount = 1;
    perSubQueueInfo.ppCmdBuffers = &pCmdBuffer;

    MultiSubmitInfo submitInfo      = {};
    submitInfo.perSubQueueInfoCount = 1;
    submitInfo.pPerSubQueueInfo     = &perSubQueueInfo;

    return SubmitInternal(submitInfo, postBatching);
}

// =====================================================================================================================
bool Queue::UsesPhysicalModeSubmission() const
{
    return (m_pDevice->EngineProperties().perEngine[GetEngineType()].flags.physicalAddressingMode!= 0);
}

// =====================================================================================================================
bool Queue::IsPreemptionSupported() const
{
    return (m_pDevice->IsPreemptionSupported(GetEngineType()) != 0);
}
} // Pal
