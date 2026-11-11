/* $Id: VBoxClipboard.cpp 115131 2026-08-25 17:30:42Z andreas.loeffler@oracle.com $ */
/** @file
 * VBoxClipboard - Shared clipboard, Windows Guest Implementation.
 */

/*
 * Copyright (C) 2006-2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_SHARED_CLIPBOARD
#include <VBox/log.h>

#include "VBoxTray.h"
#include "VBoxTrayInternal.h"
#include "VBoxHelpers.h"

#include <iprt/asm.h>
#include <iprt/errcore.h>
#include <iprt/ldr.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/utf16.h>

#include <VBox/GuestHost/SharedClipboard.h>
#include <VBox/GuestHost/SharedClipboard-win.h>
#include <VBox/GuestHost/clipboard-helper.h>
#include <VBox/HostServices/VBoxClipboardSvc.h> /* Temp, remove. */
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
# include <VBox/GuestHost/SharedClipboard-transfers.h>
#endif

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
# include <iprt/win/shlobj.h>
# include <iprt/win/shlwapi.h>
#endif


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
struct SHCLCONTEXT
{
    /** Pointer to the VBoxClient service environment. */
    const VBOXTRAYSVCENV *pEnv;
    /** Command context. */
    VBGLR3SHCLCMDCTX      CmdCtx;
    /** Windows-specific context data. */
    SHCLWINCTX            Win;
    /** Thread handle for window thread. */
    RTTHREAD              hThread;
    /** Waitable worker for guest-to-host clipboard reads. */
    RTTHREAD              hReadThread;
    /** Queue for guest-to-host clipboard reads. */
    RTREQQUEUE            hReadQueue;
    /** Number of active and queued guest-to-host reads. */
    uint32_t volatile     cReadRequests;
    /** Set after the host message worker has stopped queuing requests. */
    bool volatile         fReadShutdown;
    /** Start indicator flag. */
    bool                  fStarted;
    /** Shutdown indicator flag. */
    bool                  fShutdown;
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    /** Associated transfer data. */
    SHCLTRANSFERCTX       TransferCtx;
#endif
};


/** An isolated Windows clipboard read, shared by the reader and its caller. */
typedef struct VBTRSHCLREADJOB
{
    /** Reference count for the caller and the Windows clipboard thread. */
    uint32_t volatile cRefs;
    /** Signaled when the result is ready. */
    RTSEMEVENT        hDone;
    /** Windows clipboard format to read. */
    UINT              uFormat;
    /** Owned copy of the clipboard data, if available. */
    void             *pvData;
    /** Size of the copied data. */
    uint32_t          cbData;
    /** Windows error from opening or locking the clipboard, if any. */
    DWORD             dwError;
} VBTRSHCLREADJOB;
typedef VBTRSHCLREADJOB *PVBTRSHCLREADJOB;


/*********************************************************************************************************************************
*   Static variables                                                                                                             *
*********************************************************************************************************************************/
/** Static clipboard context (since it is the single instance). Directly used in the windows proc. */
static SHCLCONTEXT g_Ctx = { NULL };
/** Static window class name. */
static char s_szClipWndClassName[] = SHCL_WIN_WNDCLASS_NAME;


/*********************************************************************************************************************************
*   Prototypes                                                                                                                   *
*********************************************************************************************************************************/
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
static DECLCALLBACK(void) vbtrShClTransferCreatedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx);
static DECLCALLBACK(void) vbtrShClTransferUnregisteredCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx,
                                                               PSHCLTRANSFERCTX pTransferCtx);
static DECLCALLBACK(void) vbtrShClTransferDestroyCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx);
static DECLCALLBACK(int)  vbtrShClTransferInitializedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx);
static DECLCALLBACK(void) vbtrShClTransferStartedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx);
static DECLCALLBACK(void) vbtrShClTransferErrorCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx, int rc);
#endif


#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
/**
 * @copydoc ShClWinDataObject::CALLBACKS::pfnTransferBegin
 *
 * Called by ShClWinDataObject::GetData() when the user wants to paste data.
 * This then requests a new transfer on the host.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(int) vbtrShClDataObjectTransferBeginCallback(ShClWinDataObject::PCALLBACKCTX pCbCtx)
{
    LogFlowFuncEnter();

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    int rc = VbglR3ClipboardTransferRequest(&pCtx->CmdCtx);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnCreated
 *
 * Called by ShClTransferCreate via VbglR3.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(void) vbtrShClTransferCreatedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx)
{
    LogRelFlowFuncEnter();

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    int rc = ShClWinTransferCreate(&pCtx->Win, pCbCtx->pTransfer);

    LogRelFlowFuncLeaveRC(rc);
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnUnregistered
 *
 * Disables the IDataObject and drops its long-lived transfer reference before
 * consuming teardown waits for temporary transfer users.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(void) vbtrShClTransferUnregisteredCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx,
                                                               PSHCLTRANSFERCTX pTransferCtx)
{
    RT_NOREF(pTransferCtx);

    PSHCLTRANSFER pTransfer = pCbCtx->pTransfer;
    AssertPtr(pTransfer);

    ShClWinTransferUnregister(pTransfer);
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnDestroy
 *
 * Called by ShClTransferDestroy via VbglR3.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(void) vbtrShClTransferDestroyCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx)
{
    LogFlowFuncEnter();

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    ShClWinTransferDestroy(&pCtx->Win, pCbCtx->pTransfer);

    LogFlowFuncLeave();
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnInitialize
 *
 * Called by ShClTransferInit via VbglR3.
 * For H->G: Called on transfer intialization to initialize the "in-flight" IDataObject for a data transfer.
 * For G->H: Called on transfer intialization to populate the transfer's root list.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(int) vbtrShClTransferInitializeCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx)
{
    LogFlowFuncEnter();

    int rc = VINF_SUCCESS;

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    PSHCLTRANSFER pTransfer = pCbCtx->pTransfer;
    AssertPtr(pTransfer);

    switch(ShClTransferGetDir(pTransfer))
    {
        case SHCLTRANSFERDIR_HOST_TO_GUEST:
        {
            rc = ShClWinTransferInitialize(&pCtx->Win, pTransfer);
            break;
        }

        case SHCLTRANSFERDIR_GUEST_TO_HOST:
        {
            rc = ShClWinTransferGetRootsFromClipboard(&pCtx->Win, pTransfer);
            break;
        }

        default:
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnInitialized
 *
 * Called by ShClTransferInit via VbglR3.
 * For H->G: Called on transfer intialization to start the data transfer for the "in-flight" IDataObject.
 * For G->H: Nothing to do here.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(int) vbtrShClTransferInitializedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx)
{
    LogFlowFuncEnter();

    int rc = VINF_SUCCESS;

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    PSHCLTRANSFER pTransfer = pCbCtx->pTransfer;
    AssertPtr(pTransfer);

    switch(ShClTransferGetDir(pTransfer))
    {
        case SHCLTRANSFERDIR_HOST_TO_GUEST:
        {
            rc = ShClWinTransferStart(&pCtx->Win, pTransfer);
            break;
        }

        case SHCLTRANSFERDIR_GUEST_TO_HOST:
            break;

        default:
            break;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Worker for a reading clipboard from the host.
 *
 * @returns VBox status code.
 * @retval  VERR_SHCLPB_NO_DATA if no clipboard data is available.
 * @param   pCtx                Shared Clipbaord context to use.
 * @param   uFmt                The format to read clipboard data in.
 * @param   ppvData             Where to return the allocated data read.
 *                              Must be free'd by the caller.
 * @param   pcbData             Where to return number of bytes read.
 * @param   pvUser              User-supplied context.
 *
 * @thread  Clipboard main thread.
 *
 */
static DECLCALLBACK(int) vbtrShClRequestDataFromSourceCallbackWorker(PSHCLCONTEXT pCtx,
                                                                     SHCLFORMAT uFmt, void **ppvData, uint32_t *pcbData, void *pvUser)
{
    RT_NOREF(pvUser);

    return VbglR3ClipboardReadDataEx(&pCtx->CmdCtx, uFmt, ppvData, pcbData);
}

/**
 * @copydoc SHCLCALLBACKS::pfnOnRequestDataFromSource
 *
 * Called from the IDataObject implementation to request data from the host.
 *
 * @thread  shclwnd thread.
 */
DECLCALLBACK(int) vbtrShClRequestDataFromSourceCallback(PSHCLCONTEXT pCtx,
                                                        SHCLFORMAT uFmt, void **ppv, uint32_t *pcb, void *pvUser)
{
    PRTREQ pReq = NULL;
    int rc = RTReqQueueCallEx(pCtx->Win.hReqQ, &pReq, SHCL_TIMEOUT_DEFAULT_MS, RTREQFLAGS_IPRT_STATUS,
                              (PFNRT)vbtrShClRequestDataFromSourceCallbackWorker, 5, pCtx, uFmt, ppv, pcb, pvUser);
    RTReqRelease(pReq);
    return rc;
}

/**
 * @copydoc SHCLTRANSFERCALLBACKS::pfnOnStart
 *
 * Called from VbglR3 (main thread) to notify the IDataObject.
 *
 * @thread  Clipboard main thread.
 */
static DECLCALLBACK(void) vbtrShClTransferStartedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx)
{
    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    PSHCLTRANSFER pTransfer = pCbCtx->pTransfer;
    AssertPtr(pTransfer);

    SHCLTRANSFERDIR const enmDir = ShClTransferGetDir(pTransfer);

    int rc = VINF_SUCCESS;

    /* The guest wants to transfer data to the host. */
    if (enmDir == SHCLTRANSFERDIR_GUEST_TO_HOST)
    {
        rc = ShClWinTransferGetRootsFromClipboard(&pCtx->Win, pTransfer);
    }
    else if (enmDir == SHCLTRANSFERDIR_HOST_TO_GUEST)
    {
        /* Nothing to do here. */
    }
    else
        AssertFailedStmt(rc = VERR_NOT_SUPPORTED);

    if (RT_FAILURE(rc))
        LogRelMax(16, ("*** Error: Shared Clipboard: Starting transfer %RU16/%RU64 failed with %Rrc\n", ShClTransferGetID(pTransfer), ShClTransferGetGeneration(pTransfer), rc));
}

/** @copydoc SHCLTRANSFERCALLBACKS::pfnOnCompleted */
static DECLCALLBACK(void) vbtrShClTransferCompletedCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx, int rcCompletion)
{
    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    LogRelFlowFunc(("rcCompletion=%Rrc\n", rcCompletion));

    VBoxTrayVerbose(1, "Shared Clipboard: Transfer %RU16 %s\n",
                    ShClTransferGetID(pCbCtx->pTransfer), rcCompletion == VERR_CANCELLED ? "canceled" : "complete");

    SHCLTRANSFERSTATUS enmSts;

    switch (rcCompletion)
    {
        case VERR_CANCELLED:
            enmSts = SHCLTRANSFERSTATUS_CANCELED;
            break;

        case VINF_SUCCESS:
            enmSts = SHCLTRANSFERSTATUS_COMPLETED;
            break;

        default:
            AssertFailedStmt(enmSts = SHCLTRANSFERSTATUS_ERROR);
            break;
    }

    int rc = VbglR3ClipboardTransferSendStatus(&pCtx->CmdCtx, pCbCtx->pTransfer, enmSts, rcCompletion);
    LogRelFlowFuncLeaveRC(rc);
}

/** @copydoc SHCLTRANSFERCALLBACKS::pfnOnError */
static DECLCALLBACK(void) vbtrShClTransferErrorCallback(PSHCLTRANSFERCALLBACKCTX pCbCtx, int rcError)
{
    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pCbCtx->pvUser;
    AssertPtr(pCtx);

    LogRelMax(16, ("*** Error: Shared Clipboard: Transfer %RU16/%RU64 failed with %Rrc\n",
                   ShClTransferGetID(pCbCtx->pTransfer), ShClTransferGetGeneration(pCbCtx->pTransfer), rcError));

    if (g_cVerbosity) /* Only show this in verbose mode. */
    {
        char szMsg  [256]; /* Sizes according to MSDN. */
        char szTitle[64];

        /** @todo Add some translation macros here. */
        RTStrPrintf(szTitle, sizeof(szTitle), "VirtualBox Shared Clipboard");
        RTStrPrintf(szMsg, sizeof(szMsg),
                    "Transfer %RU16 failed with %Rrc", ShClTransferGetID(pCbCtx->pTransfer), rcError);

        VBoxTrayHlpShowBalloonTipEx(g_hInstance, g_hwndToolWindow, ID_TRAYICON,
                                    szMsg, szTitle,
                                    5000 /* Time to display in msec */, NIIF_INFO);
    }

    int rc = VbglR3ClipboardTransferSendStatus(&pCtx->CmdCtx, pCbCtx->pTransfer, SHCLTRANSFERSTATUS_ERROR, rcError);
    LogRelFlowFuncLeaveRC(rc);
}
#endif /* VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS */

static LRESULT vbtrShClWndProcWorker(PSHCLCONTEXT pCtx, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AssertPtr(pCtx);

    const PSHCLWINCTX pWinCtx = &pCtx->Win;

    LRESULT lresultRc = 0;

    switch (msg)
    {
        case WM_CLIPBOARDUPDATE:
        {
            LogFunc(("WM_CLIPBOARDUPDATE: pWinCtx=%p\n", pWinCtx));

            if (pCtx->fShutdown) /* If we're about to shut down, skip handling stuff here. */
                break;

            int rc = RTCritSectEnter(&pWinCtx->CritSect);
            if (RT_SUCCESS(rc))
            {
                const HWND hWndClipboardOwner = GetClipboardOwner();

                LogFunc(("WM_CLIPBOARDUPDATE: hWndOldClipboardOwner=%p, hWndNewClipboardOwner=%p\n",
                         pWinCtx->hWndClipboardOwnerUs, hWndClipboardOwner));

                if (pWinCtx->hWndClipboardOwnerUs != hWndClipboardOwner)
                {
                    int rc2 = RTCritSectLeave(&pWinCtx->CritSect);
                    AssertRC(rc2);

                    /* Clipboard was updated by another application.
                     * Report available formats to the host. */
                    SHCLFORMATS fFormats;
                    rc = ShClWinGetFormats(pWinCtx, &fFormats);
                    if (RT_SUCCESS(rc))
                    {
                        LogFunc(("WM_CLIPBOARDUPDATE: Reporting formats %#x\n", fFormats));
                        rc = VbglR3ClipboardReportFormats(pCtx->CmdCtx.idClient, fFormats);
                    }
                    else if (rc != VERR_ACCESS_DENIED)
                        LogRelMax(16, ("*** Error: Shared Clipboard: Enumerating Windows clipboard formats failed with %Rrc\n", rc));
                }
                else
                {
                    int rc2 = RTCritSectLeave(&pWinCtx->CritSect);
                    AssertRC(rc2);
                }
            }
            else
                LogRelMax(16, ("*** Error: Shared Clipboard: Entering the Windows clipboard state lock failed with %Rrc\n", rc));

            break;
        }

        case WM_CHANGECBCHAIN:
        {
            LogFunc(("WM_CHANGECBCHAIN\n"));
            lresultRc = ShClWinHandleWMChangeCBChain(pWinCtx, hwnd, msg, wParam, lParam);
            break;
        }

        case WM_DRAWCLIPBOARD:
        {
            LogFlowFunc(("WM_DRAWCLIPBOARD: pWinCtx=%p\n", pWinCtx));

            int rc = RTCritSectEnter(&pWinCtx->CritSect);
            if (RT_SUCCESS(rc))
            {
                const HWND hWndClipboardOwner = GetClipboardOwner();

                LogFunc(("WM_DRAWCLIPBOARD: hWndClipboardOwnerUs=%p, hWndNewClipboardOwner=%p\n",
                         pWinCtx->hWndClipboardOwnerUs, hWndClipboardOwner));

                if (pWinCtx->hWndClipboardOwnerUs != hWndClipboardOwner)
                {
                    int rc2 = RTCritSectLeave(&pWinCtx->CritSect);
                    AssertRC(rc2);

                    /* Clipboard was updated by another application. */
                    /* WM_DRAWCLIPBOARD always expects a return code of 0, so don't change "rc" here. */
                    SHCLFORMATS fFormats;
                    rc = ShClWinGetFormats(pWinCtx, &fFormats);
                    if (   RT_SUCCESS(rc)
                        && fFormats != VBOX_SHCL_FMT_NONE)
                        rc = VbglR3ClipboardReportFormats(pCtx->CmdCtx.idClient, fFormats);
                    else if (RT_FAILURE(rc) && rc != VERR_ACCESS_DENIED)
                        LogRelMax(16, ("*** Error: Shared Clipboard: Enumerating Windows clipboard formats failed with %Rrc\n", rc));
                }
                else
                {
                    int rc2 = RTCritSectLeave(&pWinCtx->CritSect);
                    AssertRC(rc2);
                }
            }
            else
                LogRelMax(16, ("*** Error: Shared Clipboard: Entering the Windows clipboard state lock failed with %Rrc\n", rc));

            lresultRc = ShClWinChainPassToNext(pWinCtx, msg, wParam, lParam);
            break;
        }

        case WM_TIMER:
        {
            int rc = ShClWinHandleWMTimer(pWinCtx);
            AssertRC(rc);

            break;
        }

        case WM_CLOSE:
        {
            /* Do nothing. Ignore the message. */
            break;
        }

        case WM_RENDERFORMAT: /* Guest wants to render the clipboard data. */
        {
            /* Insert the requested clipboard format data into the clipboard. */
            const UINT       uFmtWin  = (UINT)wParam;
            const SHCLFORMAT uFmtVBox = ShClWinClipboardFormatToVBox(uFmtWin);

            LogFunc(("WM_RENDERFORMAT: uFmtWin=%u -> uFmtVBox=0x%x\n", uFmtWin, uFmtVBox));

            char *pszFmts = ShClFormatsToStrA(uFmtVBox);
            VBoxTrayVerbose(1, "Shared Clipboard: Rendering Windows format %#x as VBox format %#x/'%s'\n",
                            uFmtWin, uFmtVBox, pszFmts ? pszFmts : "<alloc failed>");
            RTStrFree(pszFmts);

            if (uFmtVBox == VBOX_SHCL_FMT_NONE)
            {
                LogRelMax(16, ("Shared Clipboard: Unsupported Windows clipboard format %#x requested\n", uFmtWin));
                ShClWinClear();
            }
            else
            {
                void    *pvData = NULL;
                uint32_t cbData;

                HANDLE hMem;
                int rc = VbglR3ClipboardReadDataEx(&pCtx->CmdCtx, uFmtVBox, &pvData, &cbData);
                if (RT_SUCCESS(rc))
                {
                    /* Verify the size of returned text, the memory block for clipboard must have the exact string size. */
                    if (uFmtVBox == VBOX_SHCL_FMT_UNICODETEXT)
                    {
                        size_t cwcActual = 0;
                        rc = RTUtf16NLenEx((PCRTUTF16)pvData, cbData / sizeof(RTUTF16), &cwcActual);
                        if (RT_SUCCESS(rc))
                            cbData = (uint32_t)((cwcActual + 1 /* '\0' */) * sizeof(RTUTF16));
                        else
                            LogRelMax(16, ("*** Error: Shared Clipboard: Invalid UTF-16 string from host: cb=%RU32, cwcActual=%zu, rc=%Rrc\n", cbData, cwcActual, rc));
                    }
                    else if (uFmtVBox == VBOX_SHCL_FMT_HTML)
                    {
                        /* Wrap content into CF_HTML clipboard format if needed. */
                        if (!ShClWinIsCFHTML((const char *)pvData))
                        {
                            char    *pszWrapped = NULL;
                            uint32_t cbWrapped  = 0;
                            rc = ShClWinConvertMIMEToCFHTML((const char *)pvData, cbData, &pszWrapped, &cbWrapped);
                            if (RT_SUCCESS(rc))
                            {
                                AssertBreakStmt(cbWrapped, rc = VERR_INVALID_PARAMETER);
                                pvData = RTMemRealloc(pvData, cbWrapped);
                                if (pvData)
                                {
                                    memcpy(pvData, pszWrapped, cbWrapped);
                                    cbData = cbWrapped;
                                }
                                else
                                    rc = VERR_NO_MEMORY;
                                RTMemFree(pszWrapped);
                            }

                            if (RT_FAILURE(rc))
                                LogRelMax(16, ("*** Error: Shared Clipboard: Converting host HTML data to the Windows CF_HTML clipboard format failed with %Rrc\n", rc));
                        }
                    }

                    hMem = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, cbData);
                    if (hMem)
                    {
                        void *pvMem = GlobalLock(hMem);
                        if (pvMem)
                        {
                            memcpy(pvMem, pvData, cbData);
                            GlobalUnlock(hMem);

                            HANDLE hClip = SetClipboardData(uFmtWin, hMem);
                            if (hClip)
                                hMem = NULL; /* The system now owns the memory. */
                            else
                                LogRelMax(16, ("*** Error: Shared Clipboard: Setting host data buffer on the Windows clipboard failed with %Rrc\n",
                                               RTErrConvertFromWin32(GetLastError())));
                        }
                        else
                            LogRelMax(16, ("*** Error: Shared Clipboard: Locking the host clipboard data buffer failed with %Rrc\n", RTErrConvertFromWin32(GetLastError())));
                        if (hMem)
                            GlobalFree(hMem);
                    }
                    else
                        LogRelMax(16, ("*** Error: Shared Clipboard: Allocating %RU32 bytes for the host clipboard data buffer failed\n", cbData));
                }
            }

            break;
        }

        case WM_RENDERALLFORMATS:
        {
            LogFunc(("WM_RENDERALLFORMATS\n"));

            int rc = ShClWinHandleWMRenderAllFormats(pWinCtx, hwnd);
            AssertRC(rc);

            break;
        }

        case SHCL_WIN_WM_REPORT_FORMATS: /* Host reported clipboard formats. */
        {
            LogFunc(("SHCL_WIN_WM_REPORT_FORMATS\n"));

            /* Announce available formats. Do not insert data -- will be inserted in WM_RENDERFORMAT. */
            PVBGLR3CLIPBOARDEVENT pEvent = (PVBGLR3CLIPBOARDEVENT)lParam;
            AssertPtr(pEvent);
            Assert(pEvent->enmType == VBGLR3CLIPBOARDEVENTTYPE_REPORT_FORMATS);

            const SHCLFORMATS fFormats = pEvent->u.fReportedFormats;

            char *pszFmts = ShClFormatsToStrA(fFormats);
            VBoxTrayVerbose(1, "Shared Clipboard: Host reported formats %#x/'%s'\n",
                            fFormats, pszFmts ? pszFmts : "<alloc failed>");
            RTStrFree(pszFmts);

            if (fFormats != VBOX_SHCL_FMT_NONE) /* Could arrive with some older GA versions. */
            {
                int rc = ShClWinClearAndAnnounceFormats(pWinCtx, fFormats, hwnd);
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
                if (   RT_SUCCESS(rc)
                    && fFormats & VBOX_SHCL_FMT_URI_LIST)
                {
                    /*
                     * Create our IDataObject implementation and push it to the Windows clibpoard.
                     * That way Windows will recognize that there is a data transfer available.
                     */
                    ShClWinDataObject::CALLBACKS Callbacks;
                    RT_ZERO(Callbacks);
                    Callbacks.pfnTransferBegin = vbtrShClDataObjectTransferBeginCallback;

                    rc = ShClWinTransferCreateAndSetDataObject(pWinCtx, pCtx, &Callbacks);
                }
#endif
                if (   RT_FAILURE(rc)
                    && rc != VERR_ACCESS_DENIED)
                    LogRelMax(16, ("*** Error: Shared Clipboard: Announcing host clipboard formats %#x to Windows failed with %Rrc\n", fFormats, rc));
            }

            LogFunc(("SHCL_WIN_WM_REPORT_FORMATS: fFormats=0x%x, lastErr=%ld\n", fFormats, GetLastError()));
            VbglR3ClipboardEventFree(pEvent);
            break;
        }

        case WM_DESTROY:
        {
            LogFunc(("WM_DESTROY\n"));

            int rc = ShClWinHandleWMDestroy(pWinCtx);
            AssertRC(rc);

            /*
             * Don't need to call PostQuitMessage cause
             * the VBoxTray already finished a message loop.
             */

            break;
        }

        default:
        {
            LogFunc(("WM_ %p\n", msg));
            lresultRc = DefWindowProc(hwnd, msg, wParam, lParam);
            break;
        }
    }

    LogFunc(("WM_ rc %d\n", lresultRc));
    return lresultRc;
}

static LRESULT CALLBACK vbtrShClWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static int vbtrShClCreateWindow(PSHCLCONTEXT pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    int rc = VINF_SUCCESS;

    AssertPtr(pCtx->pEnv);
    HINSTANCE hInstance = pCtx->pEnv->hInstance;
    Assert(hInstance != 0);

    /* Register the Window Class. */
    WNDCLASSEX wc;
    RT_ZERO(wc);

    wc.cbSize = sizeof(WNDCLASSEX);

    if (!GetClassInfoEx(hInstance, s_szClipWndClassName, &wc))
    {
        wc.style         = CS_NOCLOSE;
        wc.lpfnWndProc   = vbtrShClWndProc;
        wc.hInstance     = pCtx->pEnv->hInstance;
        wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1);
        wc.lpszClassName = s_szClipWndClassName;

        ATOM wndClass = RegisterClassEx(&wc);
        if (wndClass == 0)
            rc = RTErrConvertFromWin32(GetLastError());
    }

    if (RT_SUCCESS(rc))
    {
        const PSHCLWINCTX pWinCtx = &pCtx->Win;

        /* Create the window. */
        pWinCtx->hWnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                       s_szClipWndClassName, s_szClipWndClassName,
                                       WS_POPUPWINDOW,
                                       -200, -200, 100, 100, NULL, NULL, hInstance, NULL);
        if (pWinCtx->hWnd == NULL)
        {
            rc = VERR_NOT_SUPPORTED;
        }
        else
        {
            SetWindowPos(pWinCtx->hWnd, HWND_TOPMOST, -200, -200, 0, 0,
                         SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSIZE);

            rc = ShClWinChainAdd(pWinCtx);
            if (RT_SUCCESS(rc))
            {
                if (!ShClWinIsNewAPI(&pWinCtx->newAPI))
                    pWinCtx->oldAPI.timerRefresh = SetTimer(pWinCtx->hWnd, 0, 10 * 1000 /* 10s */, NULL);
            }
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) vbtrShClWindowThread(RTTHREAD hThread, void *pvUser)
{
    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pvUser;
    AssertPtr(pCtx);

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    HRESULT hr = OleInitialize(NULL);
    if (FAILED(hr))
    {
        VBoxTrayError("Shared Clipboard: Initializing OLE in window thread failed (%Rhrc) -- file transfers unavailable\n", hr);
        /* Not critical, the rest of the clipboard might work. */
    }
    else
        VBoxTrayInfo("Shared Clipboard: Initialized OLE in window thread\n");
#endif /* VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS */

    int rc = vbtrShClCreateWindow(pCtx);
    if (RT_FAILURE(rc))
    {
        VBoxTrayError("Shared Clipboard: Unable to create window, rc=%Rrc\n", rc);
        return rc;
    }

    pCtx->fStarted = true; /* Set started indicator. */

    int rc2 = RTThreadUserSignal(hThread);
    bool fSignalled = RT_SUCCESS(rc2);

    VBoxTrayInfo("Shared Clipboard: Window thread running\n");

    if (RT_SUCCESS(rc))
    {
        for (;;)
        {
            MSG uMsg;
            BOOL fRet;
            while ((fRet = GetMessage(&uMsg, 0, 0, 0)) > 0)
            {
                TranslateMessage(&uMsg);
                DispatchMessage(&uMsg);
            }
            Assert(fRet >= 0);

            if (ASMAtomicReadBool(&pCtx->fShutdown))
                break;

            /** @todo Immediately drop on failure? */
        }
    }

    if (!fSignalled)
    {
        rc2 = RTThreadUserSignal(hThread);
        AssertRC(rc2);
    }

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    OleSetClipboard(NULL); /* Make sure to flush the clipboard on destruction. */
    OleUninitialize();
#endif

    VBoxTrayInfo("Shared Clipboard: Window thread ended\n");

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static LRESULT CALLBACK vbtrShClWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    PSHCLCONTEXT pCtx = &g_Ctx; /** @todo r=andy Make pCtx available through SetWindowLongPtr() / GWL_USERDATA. */
    AssertPtr(pCtx);

    /* Forward with proper context. */
    return vbtrShClWndProcWorker(pCtx, hWnd, uMsg, wParam, lParam);
}

/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnPreInit}
 */
static DECLCALLBACK(int) vbtrShClPreInit(void)
{
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnOption}
 */
static DECLCALLBACK(int) vbtrShClOption(const char **ppszShort, int argc, char **argv, int *pi)
{
    RT_NOREF(ppszShort, argc, argv, pi);

    return -1;
}

/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnInit}
 */
DECLCALLBACK(int) vbtrShClInit(const PVBOXTRAYSVCENV pEnv, void **ppvInstance)
{
    LogFlowFuncEnter();

    PSHCLCONTEXT pCtx = &g_Ctx; /* Only one instance for now. */
    AssertPtr(pCtx);

    if (pCtx->pEnv)
    {
        /* Clipboard was already initialized. 2 or more instances are not supported. */
        return VERR_NOT_SUPPORTED;
    }

    if (VbglR3AutoLogonIsRemoteSession())
    {
        /* Do not use clipboard for remote sessions. */
        VBoxTrayInfo("Shared Clipboard: Clipboard has been disabled for a remote session\n");
        return VERR_NOT_SUPPORTED;
    }

    pCtx->pEnv          = pEnv;
    pCtx->hThread       = NIL_RTTHREAD;
    pCtx->hReadThread   = NIL_RTTHREAD;
    pCtx->hReadQueue    = NIL_RTREQQUEUE;
    pCtx->cReadRequests = 0;
    pCtx->fReadShutdown = false;
    pCtx->fStarted      = false;
    pCtx->fShutdown     = false;

    int rc = RTReqQueueCreate(&pCtx->Win.hReqQ);
    AssertRCReturn(rc, rc);

    rc = ShClWinCtxInit(&pCtx->Win);
    if (RT_SUCCESS(rc))
        rc = VbglR3ClipboardConnectEx(&pCtx->CmdCtx, VBOX_SHCL_GF_0_CONTEXT_ID);
    if (RT_SUCCESS(rc))
    {
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
        rc = ShClTransferCtxInit(&pCtx->TransferCtx);
#endif
        if (RT_SUCCESS(rc))
        {
            /* Message pump thread for our proxy window. */
            rc = RTThreadCreate(&pCtx->hThread, vbtrShClWindowThread, pCtx /* pvUser */,
                                0, RTTHREADTYPE_MSG_PUMP, RTTHREADFLAGS_WAITABLE,
                                "shclwnd");
            if (RT_SUCCESS(rc))
            {
                int rc2 = RTThreadUserWait(pCtx->hThread, RT_MS_30SEC /* Timeout in ms */);
                AssertRC(rc2);

                if (!pCtx->fStarted) /* Did the thread fail to start? */
                    rc = VERR_NOT_SUPPORTED; /* Report back Shared Clipboard as not being supported. */
            }
        }

        if (RT_SUCCESS(rc))
        {
            *ppvInstance = pCtx;
        }
        else
            VbglR3ClipboardDisconnectEx(&pCtx->CmdCtx);
    }

    if (RT_FAILURE(rc))
        VBoxTrayError("Shared Clipboard: Unable to initialize, rc=%Rrc\n", rc);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Releases a reference to a Windows clipboard read job.
 *
 * @param   pJob                Clipboard read job.
 */
static void vbtrShClReadJobRelease(PVBTRSHCLREADJOB pJob)
{
    if (ASMAtomicDecU32(&pJob->cRefs) == 0)
    {
        RTSemEventDestroy(pJob->hDone);
        RTMemFree(pJob->pvData);
        RTMemFree(pJob);
    }
}


/**
 * Fetches Windows clipboard data without depending on the clipboard service
 * or its logger. Windows can still be rendering data when VBoxTray shuts down.
 *
 * @returns VBox status code.
 * @param   hThread             Current thread.
 * @param   pvUser              Clipboard read job, with one reference for this thread.
 */
static DECLCALLBACK(int) vbtrShClFetchClipboard(RTTHREAD hThread, void *pvUser)
{
    RT_NOREF(hThread);
    PVBTRSHCLREADJOB const pJob = (PVBTRSHCLREADJOB)pvUser;

    /* Match ShClWinOpen's bounded retry without accessing the service logger. */
    for (uint32_t i = 0; ; i++)
    {
        if (ASMAtomicReadU32(&pJob->cRefs) == 1)
            break;
        if (OpenClipboard(NULL))
        {
            HANDLE hClip = GetClipboardData(pJob->uFormat);
            if (hClip && ASMAtomicReadU32(&pJob->cRefs) > 1)
            {
                const void *pvClip = GlobalLock(hClip);
                if (pvClip)
                {
                    size_t cbClip = GlobalSize(hClip);
                    if (pJob->uFormat == CF_UNICODETEXT)
                    {
                        size_t cwcActual;
                        int rc = RTUtf16NLenEx((PCRTUTF16)pvClip, cbClip / sizeof(RTUTF16), &cwcActual);
                        cbClip = RT_SUCCESS(rc) ? (cwcActual + 1) * sizeof(RTUTF16) : 0;
                    }
                    if (cbClip > 0 && cbClip <= UINT32_MAX)
                    {
                        pJob->pvData = RTMemDup(pvClip, cbClip);
                        if (pJob->pvData)
                            pJob->cbData = (uint32_t)cbClip;
                    }
                    GlobalUnlock(hClip);
                }
                else
                    pJob->dwError = GetLastError();
            }
            CloseClipboard();
            break;
        }
        if (i == 10)
        {
            pJob->dwError = GetLastError();
            break;
        }
        RTThreadSleep(1U << i);
    }

    RTSemEventSignal(pJob->hDone);
    vbtrShClReadJobRelease(pJob);
    return VINF_SUCCESS;
}


/**
 * Completes a host read request in order. The wait can end at shutdown while
 * the Windows-only thread still owns its job.
 *
 * @param   pCtx                Shared clipboard context.
 * @param   pEvent              Clipboard read event, owned by this request.
 */
static DECLCALLBACK(void) vbtrShClReadData(PSHCLCONTEXT pCtx, PVBGLR3CLIPBOARDEVENT pEvent)
{
    if (ASMAtomicReadBool(&pCtx->fShutdown))
    {
        VbglR3ClipboardEventFree(pEvent);
        ASMAtomicDecU32(&pCtx->cReadRequests);
        return;
    }

    SHCLFORMAT const fFormat = pEvent->u.fReadData;
    UINT uFormat = 0;
    bool fHtml = false;
    if (fFormat & VBOX_SHCL_FMT_BITMAP)
        uFormat = CF_DIB;
    else if (fFormat & VBOX_SHCL_FMT_UNICODETEXT)
        uFormat = CF_UNICODETEXT;
    else if (fFormat & VBOX_SHCL_FMT_HTML)
    {
        uFormat = RegisterClipboardFormat(SHCL_WIN_REGFMT_HTML);
        fHtml = true;
    }

    PVBTRSHCLREADJOB pJob = NULL;
    int rc = VERR_NOT_SUPPORTED;
    if (uFormat)
    {
        pJob = (PVBTRSHCLREADJOB)RTMemAllocZ(sizeof(*pJob));
        rc = VERR_NO_MEMORY;
        if (pJob)
        {
            pJob->cRefs = 1;
            pJob->uFormat = uFormat;
            rc = RTSemEventCreate(&pJob->hDone);
            if (RT_SUCCESS(rc))
            {
                pJob->cRefs = 2;
                rc = RTThreadCreate(NULL, vbtrShClFetchClipboard, pJob, 0, RTTHREADTYPE_IO, 0, "shclfetch");
                if (RT_SUCCESS(rc))
                {
                    do
                        rc = RTSemEventWait(pJob->hDone, 100 /* ms */);
                    while (rc == VERR_TIMEOUT && !ASMAtomicReadBool(&pCtx->fShutdown));
                }
                else
                    vbtrShClReadJobRelease(pJob);
            }
            else
            {
                RTMemFree(pJob);
                pJob = NULL;
            }
        }
    }

    if (!ASMAtomicReadBool(&pCtx->fShutdown))
    {
        void *pvData = NULL;
        uint32_t cbData = 0;
        char *pszMime = NULL;
        if (RT_SUCCESS(rc))
        {
            pvData = pJob->pvData;
            cbData = pJob->cbData;
            if (pJob->dwError && pJob->dwError != ERROR_ACCESS_DENIED)
                LogRelMax(16, ("*** Error: Shared Clipboard: Reading Windows clipboard format %#x failed with %Rrc\n",
                               fFormat, RTErrConvertFromWin32(pJob->dwError)));

            const char *pszClip = (const char *)pvData;
            if (   fHtml
                && pszClip
                && RTStrNLen(pszClip, cbData) < cbData
                && ShClWinIsCFHTML(pszClip))
            {
                uint32_t cbMime = 0;
                if (RT_SUCCESS(ShClWinConvertCFHTMLToMIME(pszClip, cbData, &pszMime, &cbMime)))
                {
                    pvData = pszMime;
                    cbData = cbMime;
                }
            }
        }
        else if (rc != VERR_NOT_SUPPORTED)
            LogRelMax(16, ("*** Error: Shared Clipboard: Starting or waiting for Windows clipboard format %#x failed with %Rrc\n",
                           fFormat, rc));

        /* An empty reply completes a failed read. */
        VbglR3ClipboardWriteDataEx(&pEvent->cmdCtx, fFormat, pvData, cbData);
        RTMemFree(pszMime);
    }
    if (pJob)
        vbtrShClReadJobRelease(pJob);
    VbglR3ClipboardEventFree(pEvent);
    ASMAtomicDecU32(&pCtx->cReadRequests);
}


/**
 * Processes guest-to-host clipboard reads in order.
 *
 * @returns VBox status code.
 * @param   hThread             Current thread.
 * @param   pvUser              Shared clipboard context.
 */
static DECLCALLBACK(int) vbtrShClReadThread(RTTHREAD hThread, void *pvUser)
{
    RT_NOREF(hThread);
    PSHCLCONTEXT const pCtx = (PSHCLCONTEXT)pvUser;

    while (!ASMAtomicReadBool(&pCtx->fReadShutdown))
    {
        int rc = RTReqQueueProcess(pCtx->hReadQueue, 250 /* ms */);
        Assert(rc == VERR_TIMEOUT || rc == VERR_INTERRUPTED);
        RT_NOREF(rc);
    }

    /* Discard the remaining events after the producer has stopped. */
    RTReqQueueProcess(pCtx->hReadQueue, 0);
    return VINF_SUCCESS;
}


/**
 * Completes a read that could not be queued.
 *
 * @param   pCtx                Shared clipboard context.
 * @param   pEvent              Clipboard read event.
 * @param   pfShutdown          Host message worker's shutdown indicator.
 */
static void vbtrShClRejectRead(PSHCLCONTEXT pCtx, PVBGLR3CLIPBOARDEVENT pEvent, bool volatile *pfShutdown)
{
    /* Without a context ID, the host matches replies to requests in format
       order. Wait for the reader to send all earlier replies first. */
    if (pEvent->cmdCtx.fUseLegacyProtocol)
        while (   ASMAtomicReadU32(&pCtx->cReadRequests) != 0
               && !ASMAtomicReadBool(pfShutdown))
            RTThreadSleep(100 /* ms */);

    if (!ASMAtomicReadBool(pfShutdown))
        VbglR3ClipboardWriteDataEx(&pEvent->cmdCtx, pEvent->u.fReadData, NULL, 0);
}


/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnWorker}
 */
DECLCALLBACK(int) vbtrShClWorker(void *pvInstance, bool volatile *pfShutdown)
{
    AssertPtr(pvInstance);
    LogFlowFunc(("pvInstance=%p\n", pvInstance));

    /*
     * Tell the control thread that it can continue
     * spawning services.
     */
    RTThreadUserSignal(RTThreadSelf());

    const PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pvInstance;
    AssertPtr(pCtx);

    const PSHCLWINCTX pWinCtx = &pCtx->Win;

    VBoxTrayVerbose(1, "Shared Clipboard: Worker loop running\n");

    int rcReadInit = RTReqQueueCreate(&pCtx->hReadQueue);
    if (RT_SUCCESS(rcReadInit))
        rcReadInit = RTThreadCreate(&pCtx->hReadThread, vbtrShClReadThread, pCtx,
                                    0, RTTHREADTYPE_IO, RTTHREADFLAGS_WAITABLE, "shclread");
    if (RT_FAILURE(rcReadInit))
        VBoxTrayError("Shared Clipboard: Starting the clipboard read worker failed with %Rrc\n", rcReadInit);

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    HRESULT hr = OleInitialize(NULL);
    if (FAILED(hr))
    {
        VBoxTrayError("Shared Clipboard: Initializing OLE in worker thread failed (%Rhrc) -- file transfers unavailable\n", hr);
        /* Not critical, the rest of the clipboard might work. */
    }
    else
        VBoxTrayInfo("Shared Clipboard: Initialized OLE in worker thread\n");

    /*
     * Init callbacks.
     * Those will be registered within VbglR3 when a new transfer gets initialized.
     */
    RT_ZERO(pCtx->CmdCtx.Transfers.Callbacks);

    pCtx->CmdCtx.Transfers.Callbacks.pvUser = pCtx; /* Assign context as user-provided callback data. */
    pCtx->CmdCtx.Transfers.Callbacks.cbUser = sizeof(SHCLCONTEXT);

    pCtx->CmdCtx.Transfers.Callbacks.pfnOnCreated      = vbtrShClTransferCreatedCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnUnregistered = vbtrShClTransferUnregisteredCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnDestroy      = vbtrShClTransferDestroyCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnInitialize   = vbtrShClTransferInitializeCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnInitialized  = vbtrShClTransferInitializedCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnStarted      = vbtrShClTransferStartedCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnCompleted    = vbtrShClTransferCompletedCallback;
    pCtx->CmdCtx.Transfers.Callbacks.pfnOnError        = vbtrShClTransferErrorCallback;
#endif /* VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS */

    int rc;

    /* The thread waits for incoming messages from the host. */
    PVBGLR3CLIPBOARDEVENT pEvent = NULL;
    for (;;)
    {
        LogFlowFunc(("Waiting for host message (fUseLegacyProtocol=%RTbool, fHostFeatures=%#RX64) ...\n",
                     pCtx->CmdCtx.fUseLegacyProtocol, pCtx->CmdCtx.fHostFeatures));

        if (!pEvent)
            pEvent = (PVBGLR3CLIPBOARDEVENT)RTMemAllocZ(sizeof(VBGLR3CLIPBOARDEVENT));
        AssertPtrBreakStmt(pEvent, rc = VERR_NO_MEMORY);

        uint32_t idMsg  = 0;
        uint32_t cParms = 0;
        rc = VbglR3ClipboardMsgPeekWait(&pCtx->CmdCtx, &idMsg, &cParms, NULL /* pidRestoreCheck */);
        if (RT_SUCCESS(rc))
        {
#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
            rc = VbglR3ClipboardEventGetNextEx(idMsg, cParms, &pCtx->CmdCtx, &pCtx->TransferCtx, pEvent);
#else
            rc = VbglR3ClipboardEventGetNext(idMsg, cParms, &pCtx->CmdCtx, pEvent);
#endif
        }
        else if (rc == VERR_TRY_AGAIN) /* No new message (yet). */
        {
            RTReqQueueProcess(pCtx->Win.hReqQ, RT_MS_1SEC);
            continue;
        }
        else if (   rc != VERR_INTERRUPTED
                 && !*pfShutdown)
            LogRelMax(16, ("*** Error: Shared Clipboard: Waiting for a host message failed with %Rrc\n", rc));

        if (RT_FAILURE(rc))
        {
            LogFlowFunc(("Getting next event failed with %Rrc\n", rc));

            VbglR3ClipboardEventFree(pEvent);
            pEvent = NULL;

            if (*pfShutdown)
                break;

            /* Wait a bit before retrying. */
            RTThreadSleep(1000);
            continue;
        }
        else
        {
            AssertPtr(pEvent);
            LogFlowFunc(("Event uType=%RU32\n", pEvent->enmType));

            switch (pEvent->enmType)
            {
                case VBGLR3CLIPBOARDEVENTTYPE_REPORT_FORMATS:
                {
                    /* The host has announced available clipboard formats.
                     * Forward the information to the window, so it can later
                     * respond to WM_RENDERFORMAT message. */
                    if (::PostMessage(pWinCtx->hWnd, SHCL_WIN_WM_REPORT_FORMATS,
                                      0 /* wParam */, (LPARAM)pEvent /* lParam */))
                        pEvent = NULL; /* Ownership transferred to the window thread. */
                    else if (!*pfShutdown)
                        LogRelMax(16, ("*** Error: Shared Clipboard: Posting host clipboard formats %#x to the Windows clipboard thread failed with %Rrc\n",
                                       pEvent->u.fReportedFormats, RTErrConvertFromWin32(GetLastError())));
                    break;
                }

                case VBGLR3CLIPBOARDEVENTTYPE_READ_DATA:
                {
                    if (*pfShutdown)
                        break;

                    int rcRead = rcReadInit;
                    if (RT_SUCCESS(rcRead))
                    {
                        /* Bound the backlog if Windows delays rendering a format. */
                        if (ASMAtomicIncU32(&pCtx->cReadRequests) <= 16)
                            rcRead = RTReqQueueCallEx(pCtx->hReadQueue, NULL, 0, RTREQFLAGS_VOID | RTREQFLAGS_NO_WAIT,
                                                       (PFNRT)vbtrShClReadData, 2, pCtx, pEvent);
                        else
                            rcRead = VERR_RESOURCE_BUSY;
                        if (RT_FAILURE(rcRead))
                            ASMAtomicDecU32(&pCtx->cReadRequests);
                    }
                    if (RT_SUCCESS(rcRead))
                        pEvent = NULL; /* Ownership transferred to the reader. */
                    else
                    {
                        LogRelMax(16, ("*** Error: Shared Clipboard: Dispatching host data request for format %#x failed with %Rrc\n",
                                       pEvent->u.fReadData, rcRead));
                        vbtrShClRejectRead(pCtx, pEvent, pfShutdown);
                    }
                    break;
                }

                case VBGLR3CLIPBOARDEVENTTYPE_QUIT:
                {
                    VBoxTrayVerbose(1, "Shared Clipboard: Host requested termination\n");
                    ASMAtomicXchgBool(pfShutdown, true);
                    break;
                }

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
                case VBGLR3CLIPBOARDEVENTTYPE_TRANSFER_STATUS:
                {
                    /* Nothing to do here. */
                    rc = VINF_SUCCESS;
                    break;
                }
#endif
                case VBGLR3CLIPBOARDEVENTTYPE_NONE:
                {
                    /* Nothing to do here. */
                    rc = VINF_SUCCESS;
                    break;
                }

                default:
                {
                    AssertMsgFailedBreakStmt(("Event type %RU32 not implemented\n", pEvent->enmType), rc = VERR_NOT_SUPPORTED);
                }
            }

            if (pEvent)
            {
                VbglR3ClipboardEventFree(pEvent);
                pEvent = NULL;
            }
        }

        if (*pfShutdown)
            break;
    }

    /* Stop the window thread after all clipboard events have been queued. */
    ASMAtomicWriteBool(&pCtx->fShutdown, true);
    PostMessage(pWinCtx->hWnd, WM_QUIT, 0, 0);

    VBoxTrayVerbose(1, "Shared Clipboard: Worker loop ended\n");

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    OleSetClipboard(NULL); /* Make sure to flush the clipboard on destruction. */
    OleUninitialize();
#endif

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnStop}
 */
DECLCALLBACK(int) vbtrShClStop(void *pvInstance)
{
    AssertPtrReturn(pvInstance, VERR_INVALID_POINTER);

    LogFunc(("Stopping pvInstance=%p\n", pvInstance));

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pvInstance;
    AssertPtr(pCtx);

    /* Set shutdown indicator. */
    ASMAtomicWriteBool(&pCtx->fShutdown, true);

    /* Disconnect from the host service.
     * This will also send a VBOX_SHCL_HOST_MSG_QUIT from the host so that we can break out from our message worker. */
    VbglR3ClipboardDisconnect(pCtx->CmdCtx.idClient);
    pCtx->CmdCtx.idClient = 0;

    LogFlowFuncLeaveRC(VINF_SUCCESS);
    return VINF_SUCCESS;
}

/**
 * @interface_method_impl{VBOXTRAYSVCDESC,pfnDestroy}
 */
DECLCALLBACK(void) vbtrShClDestroy(void *pvInstance)
{
    AssertPtrReturnVoid(pvInstance);

    PSHCLCONTEXT pCtx = (PSHCLCONTEXT)pvInstance;
    AssertPtrReturnVoid(pCtx);

    /* Make sure that we are disconnected. */
    Assert(pCtx->CmdCtx.idClient == 0);

    LogFlowFunc(("pCtx=%p\n", pCtx));

    VBoxTrayVerbose(1, "Shared Clipboard: Destroying ...\n");

    const PSHCLWINCTX pWinCtx = &pCtx->Win;

    if (pCtx->hThread != NIL_RTTHREAD)
    {
        int rcThread = VERR_WRONG_ORDER;
        int rc = RTThreadWait(pCtx->hThread, 60 * 1000 /* Timeout in ms */, &rcThread);
        LogFlowFunc(("Waiting for thread resulted in %Rrc (thread exited with %Rrc)\n",
                     rc, rcThread));
        RT_NOREF(rc);
    }

    if (pWinCtx->hWnd)
    {
        DestroyWindow(pWinCtx->hWnd);
        pWinCtx->hWnd = NULL;
    }

    if (pCtx->hReadThread != NIL_RTTHREAD)
    {
        /* The read worker can leave its wait even if Windows is still rendering clipboard data. */
        ASMAtomicWriteBool(&pCtx->fReadShutdown, true);
        int rc = RTThreadWait(pCtx->hReadThread, RT_INDEFINITE_WAIT, NULL);
        AssertRC(rc);
        pCtx->hReadThread = NIL_RTTHREAD;
    }
    if (pCtx->hReadQueue != NIL_RTREQQUEUE)
    {
        RTReqQueueDestroy(pCtx->hReadQueue);
        pCtx->hReadQueue = NIL_RTREQQUEUE;
    }

    UnregisterClass(s_szClipWndClassName, pCtx->pEnv->hInstance);

    ShClWinCtxDestroy(&pCtx->Win);

#ifdef VBOX_WITH_SHARED_CLIPBOARD_TRANSFERS
    ShClTransferCtxDestroy(&pCtx->TransferCtx);
#endif

    RTReqQueueDestroy(pCtx->Win.hReqQ);

    VBoxTrayVerbose(1, "Shared Clipboard: Destroyed\n");

    return;
}

/**
 * The service description.
 */
VBOXTRAYSVCDESC g_SvcDescClipboard =
{
    /* pszName. */
    "clipboard",
    /* pszDescription. */
    "Shared Clipboard",
    /* pszUsage. */
    NULL,
    /* pszOptions. */
    NULL,
    /* methods */
    vbtrShClPreInit,
    vbtrShClOption,
    vbtrShClInit,
    vbtrShClWorker,
    vbtrShClStop,
    vbtrShClDestroy
};
