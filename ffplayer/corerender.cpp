// ����ͷ�ļ�
#include <windows.h>
#include <mmsystem.h>
#include "coreplayer.h"
#include "corerender.h"
#include "wavqueue.h"
#include "bmpqueue.h"
#include "log.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}

// �ڲ����Ͷ���
typedef struct
{
    int         nVideoWidth;
    int         nVideoHeight;
    PixelFormat PixelFormat;
    SwrContext *pSWRContext;
    SwsContext *pSWSContext;

    HWAVEOUT    hWaveOut;
    WAVQUEUE    WavQueue;
    int         nWavBufAvail;
    BYTE       *pWavBufCur;
    WAVEHDR    *pWavHdrCur;

    #define RS_PAUSE  (1 << 0)
    #define RS_SEEK   (1 << 1)
    #define RS_CLOSE  (1 << 2)
    int         nRenderStatus;
    int         nRenderPosX;
    int         nRenderPosY;
    int         nRenderWidth;
    int         nRenderHeight;
    HWND        hRenderWnd;
    HDC         hRenderDC;
    HDC         hBufferDC;
    HANDLE      hVideoThread;
    BMPQUEUE    BmpQueue;

    DWORD       dwCurTick;
    DWORD       dwLastTick;
    int         iFrameTick;
    int         iSleepTick;

    int64_t     i64CurTimeA;
    int64_t     i64CurTimeV;
} RENDER;

// �ڲ�����ʵ��
static void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
    RENDER  *render = (RENDER*)dwInstance;
    WAVEHDR *pwhdr  = NULL;
    int64_t *ppts   = NULL;
    switch (uMsg)
    {
    case WOM_DONE:
        wavqueue_read_request(&(render->WavQueue), &ppts, &pwhdr);
        //++ play completed ++//
        if (!(render->nRenderStatus & RS_PAUSE) && *ppts == -1) {
            PostMessage(render->hRenderWnd, MSG_COREPLAYER, PLAY_COMPLETED, 0);
        }
        //-- play completed --//
        else
        {
            render->i64CurTimeA = *ppts;
            log_printf(TEXT("i64CurTimeA = %lld\n"), render->i64CurTimeA);
        }
        wavqueue_read_done(&(render->WavQueue));
        break;
    }
}

static DWORD WINAPI VideoRenderThreadProc(RENDER *render)
{
    while (!(render->nRenderStatus & RS_CLOSE))
    {
        if (render->nRenderStatus & RS_PAUSE) {
            Sleep(20);
            continue;
        }

        HBITMAP hbitmap = NULL;
        int64_t *ppts   = NULL;
        bmpqueue_read_request(&(render->BmpQueue), &ppts, &hbitmap);
        if (!(render->nRenderStatus & RS_SEEK)) {
            render->i64CurTimeV = *ppts; // the current play time
            SelectObject(render->hBufferDC, hbitmap);
            BitBlt(render->hRenderDC, render->nRenderPosX, render->nRenderPosY,
                render->nRenderWidth, render->nRenderHeight,
                render->hBufferDC, 0, 0, SRCCOPY);
        }
        bmpqueue_read_done(&(render->BmpQueue));

        // ++ frame rate control ++ //
        render->dwLastTick = render->dwCurTick;
        render->dwCurTick  = GetTickCount();
        int64_t diff = render->dwCurTick - render->dwLastTick;
        if (diff > render->iFrameTick) {
            render->iSleepTick--;
        }
        else if (diff < render->iFrameTick) {
            render->iSleepTick++;
        }
        // -- frame rate control -- //

        // ++ av sync control ++ //
        diff = render->i64CurTimeV - render->i64CurTimeA;
        if (diff >  88) render->iSleepTick++;
        if (diff < -88) render->iSleepTick--;
        // -- av sync control -- //

        //++ for seek ++//
        if (render->nRenderStatus & RS_SEEK) {
            render->iSleepTick = render->iFrameTick;
        }
        //-- for seek --//

        // do sleep
        if (render->iSleepTick > 0) Sleep(render->iSleepTick);

        log_printf(TEXT("i64CurTimeV = %lld\n"), render->i64CurTimeV);
        log_printf(TEXT("%lld, %d\n"), diff, render->iSleepTick);
    }

    return 0;
}

// ����ʵ��
HANDLE renderopen(HWND hwnd, AVRational frate, int pixfmt, int w, int h,
                  int64_t ch_layout, AVSampleFormat sndfmt, int srate)
{
    WAVEFORMATEX wfx = {0};

    RENDER *render = (RENDER*)malloc(sizeof(RENDER));
    memset(render, 0, sizeof(RENDER));
    render->hRenderWnd = hwnd; // save hwnd

    // init for audio
    wfx.cbSize          = sizeof(wfx);
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.wBitsPerSample  = 16;    // 16bit
    wfx.nSamplesPerSec  = 44100; // 44.1k
    wfx.nChannels       = 2;     // stereo
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nBlockAlign * wfx.nSamplesPerSec;
    waveOutOpen(&(render->hWaveOut), WAVE_MAPPER, &wfx, (DWORD_PTR)waveOutProc, (DWORD)render, CALLBACK_FUNCTION);
    waveOutPause(render->hWaveOut);
    wavqueue_create(&(render->WavQueue), render->hWaveOut, ((int64_t)44100 * 4 * frate.den / frate.num) & ~0x3);

    /* allocate & init swr context */
    render->pSWRContext = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100,
                                             ch_layout, sndfmt, srate, 0, NULL);
    swr_init(render->pSWRContext);

    // init for video
    render->nVideoWidth  = w;
    render->nVideoHeight = h;
    render->PixelFormat  = (PixelFormat)pixfmt;
    rendersetrect(render, 0, 0, w, h);

    render->iFrameTick = 1000 * frate.den / frate.num;
    render->iSleepTick = render->iFrameTick;

    // create dc & bitmaps
    render->hRenderDC = GetDC(render->hRenderWnd);
    render->hBufferDC = CreateCompatibleDC(NULL);

    RECT rect = {0};
    GetClientRect(hwnd, &rect);
    bmpqueue_create(&(render->BmpQueue), render->hBufferDC, rect.right, rect.bottom, 16);

    render->nRenderStatus = 0;
    render->hVideoThread  = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)VideoRenderThreadProc, render, 0, NULL);
    return (HANDLE)render;
}

void renderclose(HANDLE hrender)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;

    //++ audio ++//
    // destroy wave queue
    wavqueue_destroy(&(render->WavQueue));

    // wave out close
    if (render->hWaveOut) waveOutClose(render->hWaveOut);

    // free swr context
    swr_free(&(render->pSWRContext));
    //-- audio --//

    //++ video ++//
    // set nRenderStatus to RS_CLOSE
    render->nRenderStatus |= RS_CLOSE;
    if (bmpqueue_isempty(&(render->BmpQueue))) {
        bmpqueue_write_request(&(render->BmpQueue), NULL, NULL, NULL);
        bmpqueue_write_done   (&(render->BmpQueue));
    }
    WaitForSingleObject(render->hVideoThread, -1);
    CloseHandle(render->hVideoThread);

    // destroy bmp queue
    bmpqueue_destroy(&(render->BmpQueue));

    // set zero to free sws context
    rendersetrect(render, 0, 0, 0, 0);

    if (render->hBufferDC) DeleteDC (render->hBufferDC);
    if (render->hRenderDC) ReleaseDC(render->hRenderWnd, render->hRenderDC);
    //-- video --//

    // free context
    free(render);
}

void renderaudiowrite(HANDLE hrender, AVFrame *audio)
{
    if (!hrender) return;
    RENDER  *render  = (RENDER*)hrender;
    int      sampnum = 0;
    int64_t *ppts    = NULL;

    if (render->nRenderStatus & RS_SEEK) return;

    //++ play completed ++//
    if (audio == (AVFrame*)-1) {
        if (render->nWavBufAvail > 0) {
            memset(render->pWavBufCur, 0, render->nWavBufAvail);
            render->nWavBufAvail -= render->nWavBufAvail;
            render->pWavBufCur   += render->nWavBufAvail;
            waveOutWrite(render->hWaveOut, render->pWavHdrCur, sizeof(WAVEHDR));
            wavqueue_write_done(&(render->WavQueue));
        }

        wavqueue_write_request(&(render->WavQueue), &ppts, &(render->pWavHdrCur));
        *ppts = -1; // *ppts == -1, means completed
        memset(render->pWavHdrCur->lpData, 0, render->pWavHdrCur->dwBufferLength);
        waveOutWrite(render->hWaveOut, render->pWavHdrCur, sizeof(WAVEHDR));
        wavqueue_write_done(&(render->WavQueue));
        return;
    }
    //-- play completed --//

    do {
        if (render->nWavBufAvail == 0) {
            wavqueue_write_request(&(render->WavQueue), &ppts, &(render->pWavHdrCur));
            *ppts = audio->pts + render->pWavHdrCur->dwBufferLength / 4 * 1000 / 44100;
            render->nWavBufAvail = (int  )render->pWavHdrCur->dwBufferLength;
            render->pWavBufCur   = (BYTE*)render->pWavHdrCur->lpData;
        }

        //++ do resample audio data ++//
        sampnum = swr_convert(render->pSWRContext, (uint8_t **)&(render->pWavBufCur),
            render->nWavBufAvail / 4, (const uint8_t**)audio->extended_data,
            audio->nb_samples);
        audio->extended_data  = NULL;
        audio->nb_samples     = 0;
        render->nWavBufAvail -= sampnum * 4;
        render->pWavBufCur   += sampnum * 4;
        //-- do resample audio data --//

        if (render->nWavBufAvail == 0) {
            waveOutWrite(render->hWaveOut, render->pWavHdrCur, sizeof(WAVEHDR));
            wavqueue_write_done(&(render->WavQueue));
        }
    } while (sampnum > 0);
}

void rendervideowrite(HANDLE hrender, AVFrame *video)
{
    if (!hrender) return;
    RENDER   *render  = (RENDER*)hrender;
    AVPicture picture = {0};
    BYTE     *bmpbuf  = NULL;
    int       stride  = 0;
    int64_t  *ppts    = NULL;

    if (render->nRenderStatus & RS_SEEK) return;
    bmpqueue_write_request(&(render->BmpQueue), &ppts, &bmpbuf, &stride);
    *ppts               = video->pts;
    picture.data[0]     = bmpbuf;
    picture.linesize[0] = stride;
    sws_scale(render->pSWSContext, video->data, video->linesize, 0, render->nVideoHeight, picture.data, picture.linesize);
    bmpqueue_write_done(&(render->BmpQueue));
}

void rendersetrect(HANDLE hrender, int x, int y, int w, int h)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;

    // release first
    if (render->pSWSContext)
    {
        sws_freeContext(render->pSWSContext);
        render->pSWSContext = NULL;
    }
    if (x == 0 && y == 0 && w == 0 && h == 0) return;

    render->nRenderPosX   = x;
    render->nRenderPosY   = y;
    render->nRenderWidth  = w;
    render->nRenderHeight = h;

    // recreate then
    render->pSWSContext = sws_getContext(
        render->nVideoWidth,
        render->nVideoHeight,
        render->PixelFormat,
        render->nRenderWidth,
        render->nRenderHeight,
        PIX_FMT_RGB565,
        SWS_BILINEAR,
        0, 0, 0);
}

void renderstart(HANDLE hrender)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;
    waveOutRestart(render->hWaveOut);
    render->nRenderStatus &= ~RS_PAUSE;
}

void renderpause(HANDLE hrender)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;
    waveOutPause(render->hWaveOut);
    render->nRenderStatus |= RS_PAUSE;
}

void renderseek(HANDLE hrender, DWORD sec)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;

    if (sec != (DWORD)-1) {
        render->nRenderStatus |= RS_SEEK;
        waveOutReset(render->hWaveOut);
        render->i64CurTimeA = sec * 1000;
        render->i64CurTimeV = sec * 1000;
    }
    else {
        render->nRenderStatus &= ~RS_SEEK;
    }
}

void rendertime(HANDLE hrender, DWORD *time)
{
    if (!hrender) return;
    RENDER *render = (RENDER*)hrender;
    if (time) {
        DWORD atime = render->i64CurTimeA > 0 ? (DWORD)(render->i64CurTimeA / 1000) : 0;
        DWORD vtime = render->i64CurTimeV > 0 ? (DWORD)(render->i64CurTimeV / 1000) : 0;
        *time = atime > vtime ? atime : vtime;
    }
}
