/****************************************************************************
*
*    Copyright (c) 2005 - 2015 by Vivante Corp.  All rights reserved.
*
*    The material in this file is confidential and contains trade secrets
*    of Vivante Corporation. This is proprietary information owned by
*    Vivante Corporation. No part of this work may be disclosed,
*    reproduced, copied, transmitted, or used in any way for any purpose,
*    without the express written permission of Vivante Corporation.
*
*****************************************************************************/


/**
**  @file
**  Debug code for hal user layers.
**
*/

#include "gc_hal_user_linux.h"
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#ifdef ANDROID
#include <unistd.h>
#include <android/log.h>

#if (ANDROID_SDK_VERSION >= 18)
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <cutils/trace.h>
#endif

#endif


/*
    gcdDEBUG_IN_KERNEL

    If enabled, the debug output is sent to the kernel.
*/
#define gcdDEBUG_IN_KERNEL 0

/*
    gcdBUFFERED_OUTPUT

    When set to non-zero, all output is collected into a buffer with the
    specified size.  Once the buffer gets full, or the token "$$FLUSH$$" has
    been received, the debug buffer will be printed to the console.
    gcdBUFFERED_SIZE determines the size of the buffer.
*/
#define gcdBUFFERED_OUTPUT  0

/*
    gcdBUFFERED_SIZE

    When set to non-zero, all output is collected into a buffer with the
    specified size.  Once the buffer gets full, or the token "$$FLUSH$$" has
    been received, the debug buffer will be printed to the console.
*/
#define gcdBUFFERED_SIZE    1024 * 1024 * 10

/*
    gcdTHREAD_BUFFERS

    When greater then one, will accumulate messages from the specified number
    of threads in separate output buffers.
*/
#if defined(ANDROID)
#define gcdTHREAD_BUFFERS   4
#else
#define gcdTHREAD_BUFFERS   1
#endif


/*
    gcdENABLE_OVERFLOW

    When set to non-zero, and the output buffer gets full, instead of being
    printed, it will be allowed to overflow removing the oldest messages.
    Meaningful only if gcdBUFFERED_OUTPUT is 1.
*/
#define gcdENABLE_OVERFLOW  1

/*
    gcdSHOW_LINE_NUMBER

    When enabledm each print statement will be preceeded with the current
    line number.
*/
#if defined(ANDROID)
#define gcdSHOW_LINE_NUMBER 0
#else
#define gcdSHOW_LINE_NUMBER 1
#endif

/*
    gcdSHOW_PROCESS_ID

    When enabled each print statement will be preceeded with the current
    process ID.
*/
#define gcdSHOW_PROCESS_ID  0

/*
    gcdSHOW_THREAD_ID

    When enabledm each print statement will be preceeded with the current
    thread ID.
*/
#if defined(ANDROID)
#define gcdSHOW_THREAD_ID   1
#else
#define gcdSHOW_THREAD_ID   0
#endif

/*
    gcdSEPARATE_THREADS

    When enabled, an empty line is printed between messages from different
    threads.
*/
#define gcdSEPARATE_THREADS 0

/*
    gcdSHOW_TIME

    When enabled each print statement will be preceeded with the current
    high-resolution time.
*/
#define gcdSHOW_TIME        0


/*
    gcdFLUSH_FILE

    When enabled each print statement will be succeeded with a fflush
    operation. This should be turned on to capture applications that hang
    the GPU. As otherwise the file contents are not fully synced.
*/
#if gcdDUMP || gcdDUMP_API
#define gcdFLUSH_FILE       1
#else
#define gcdFLUSH_FILE       0
#endif


/******************************************************************************\
******************************** Debug Variables *******************************
\******************************************************************************/

static gceSTATUS _lastError  = gcvSTATUS_OK;
static gctUINT32 _debugLevel = gcvLEVEL_ERROR;
static gctUINT32 _debugZones[16] =
{
    /*Others*/  gcvZONE_NONE,
    /* HAL  */  gcvZONE_NONE,
    /* EGL value scope 0x3FF*/          gcvZONE_NONE,
    /* ES11 value scope 0x3FFFFF */     gcvZONE_NONE,
    /* ES20 value scope 0x1FF*/         gcvZONE_NONE,
    /* VG11 value scope 0xfFF*/         gcvZONE_NONE,
    /* GL   */  gcvZONE_NONE,
    /* DFB  */  gcvZONE_NONE,
    /* GDI  */  gcvZONE_NONE,
    /* D3D  */  gcvZONE_NONE,
    /* ES30 value scope 0x2FFF*/        gcvZONE_NONE,
    /* ---- */  gcvZONE_NONE,
    /* ---- */  gcvZONE_NONE,
    /* ---- */  gcvZONE_NONE,
    /* ---- */  gcvZONE_NONE,
    /* ---- */  gcvZONE_NONE
};

static gctBOOL _dumpAPIZones[16] =
{
    /*Others*/  gcvFALSE,
    /* HAL  */  gcvFALSE,
    /* EGL  */  gcvFALSE,
    /* ES11 */  gcvFALSE,
    /* ES20 */  gcvFALSE,
    /* VG11 */  gcvFALSE,
    /* GL   */  gcvFALSE,
    /* DFB  */  gcvFALSE,
    /* GDI  */  gcvFALSE,
    /* D3D  */  gcvFALSE,
    /* ES30 */  gcvFALSE,
    /* ---- */  gcvFALSE,
    /* ---- */  gcvFALSE,
    /* ---- */  gcvFALSE,
    /* ---- */  gcvFALSE,
    /* ---- */  gcvFALSE
};

/*************************************************************/
#define STACK_THREAD_NUMBER     16

typedef struct _gcsDUMP_FILE_INFO
{
    gctFILE     _debugFile;
    gctUINT32   _threadID;
}
gcsDUMP_FILE_INFO;

static gcsDUMP_FILE_INFO    _FileArray[STACK_THREAD_NUMBER];
static gctUINT32            _usedFileSlot = 0;
static gctUINT32            _currentPos = 0;
/*************************************************************/

static FILE *    _debugFileVS;
static FILE *    _debugFileFS;
static gctUINT32 _shaderFileType;

#if gcdBUFFERED_OUTPUT
    static gctBOOL _debugBuffering = gcvTRUE;
#else
    static gctBOOL _debugBuffering = gcvFALSE;
#endif


/******************************************************************************\
****************************** OS-dependent Macros *****************************
\******************************************************************************/

#if gcdFLUSH_FILE
#define FFLUSH  fflush
#else
#define FFLUSH(...)
#endif

#define gcmLOCKSECTION() \
    static pthread_mutex_t __lockMutex__; \
    static gctBOOL __lockMutexInitialized__ = gcvFALSE; \
    \
    if (!__lockMutexInitialized__) \
    { \
        pthread_mutex_init(&__lockMutex__, gcvNULL); \
        __lockMutexInitialized__ = gcvTRUE; \
    } \
    \
    pthread_mutex_lock(&__lockMutex__)

#define gcmUNLOCKSECTION() \
    pthread_mutex_unlock(&__lockMutex__)

#define gcmGETPROCESSID() \
    getpid()

#ifdef ANDROID
#   define gcmGETTHREADID() \
        (gctUINT32) gettid()

#   define gcmOUTPUT_STRING(File, String) \
        do \
        { \
            if (File == gcvNULL) \
            { \
                __android_log_write(ANDROID_LOG_DEBUG, "v_gal", String); \
            } \
            else \
            { \
                fprintf(File, "%s", String); \
                FFLUSH(File); \
            } \
        } \
        while (0)
#else
    extern long int syscall (long int __sysno, ...);
#   define gcmGETTHREADID() \
        syscall(SYS_gettid)

#   define gcmOUTPUT_STRING(File, String) \
        fprintf((File == gcvNULL) ? stderr : File, "%s", String); \
        FFLUSH((File == gcvNULL) ? stderr : File)
#endif

#define gcmSPRINTF(Destination, Size, Message, Value) \
    snprintf(Destination, Size, Message, Value)

#define gcmVSPRINTF(Destination, Size, Message, Arguments) \
    vsnprintf(Destination, Size, Message, Arguments)

#define gcmSTRCAT(Destination, Size, String) \
    strncat(Destination, String, Size)


/******************************************************************************\
******************************* Private Functions ******************************
\******************************************************************************/

typedef struct _gcsBUFFERED_OUTPUT * gcsBUFFERED_OUTPUT_PTR;
typedef struct _gcsBUFFERED_OUTPUT
{
#if gcdTHREAD_BUFFERS > 1
    gctUINT32               threadID;
#endif

#if gcdSHOW_LINE_NUMBER
    gctUINT64               lineNumber;
#endif

    gctINT                  indent;

#if gcdBUFFERED_OUTPUT
    gctINT                  start;
    gctINT                  index;
    gctINT                  count;
    gctCHAR                 buffer[gcdBUFFERED_SIZE];
#endif

    gcsBUFFERED_OUTPUT_PTR  prev;
    gcsBUFFERED_OUTPUT_PTR  next;
}
gcsBUFFERED_OUTPUT;

#if !gcdDEBUG_IN_KERNEL
static gcsBUFFERED_OUTPUT     _outputBuffer[gcdTHREAD_BUFFERS];
static gcsBUFFERED_OUTPUT_PTR _outputBufferHead = gcvNULL;
static gcsBUFFERED_OUTPUT_PTR _outputBufferTail = gcvNULL;
#endif

static void _Flush(
    IN gctFILE File
    )
{
#if gcdBUFFERED_OUTPUT
    gctINT i, j;

    gcsBUFFERED_OUTPUT_PTR outputBuffer = _outputBufferHead;

    while (outputBuffer != gcvNULL)
    {
        if (outputBuffer->count != 0)
        {
            i = outputBuffer->start;

#if gcdTHREAD_BUFFERS > 1
            gcmOUTPUT_STRING(File, "********************************************************************************\n");
            gcmOUTPUT_STRING(File, "FLUSHING DEBUG OUTPUT BUFFER\n");
            gcmOUTPUT_STRING(File, "********************************************************************************\n");
#endif

            for (j = 0; j < outputBuffer->count; j += 1)
            {
                gcmOUTPUT_STRING(File, outputBuffer->buffer + i);

                i += strlen(outputBuffer->buffer + i) + 1;

                if ((i >= gcdBUFFERED_SIZE) || (outputBuffer->buffer[i] == 0))
                {
                    i = 0;
                }
            }

            outputBuffer->start = 0;
            outputBuffer->index = 0;
            outputBuffer->count = 0;
        }

        outputBuffer = outputBuffer->next;
    }
#endif
}

#if !gcdDEBUG_IN_KERNEL
static void
OutputString(
    IN gctFILE File,
    IN gcsBUFFERED_OUTPUT_PTR OutputBuffer,
    IN gctCONST_STRING String
    )
{
    if (String == gcvNULL)
    {
        _Flush(File);
    }
    else

#if gcdBUFFERED_OUTPUT
    if ((OutputBuffer == gcvNULL) || !_debugBuffering)
#endif

    {
        gcmOUTPUT_STRING(File, String);
    }

#if gcdBUFFERED_OUTPUT
    else
    {
        gctINT n = strlen(String) + 1;

#if gcdENABLE_OVERFLOW
        if (
                (OutputBuffer->index + n >= gcdBUFFERED_SIZE - 1)
                ||
                (
                    (OutputBuffer->index     <  OutputBuffer->start) &&
                    (OutputBuffer->index + n >= OutputBuffer->start)
                )
        )
        {
            if (OutputBuffer->index + n >= gcdBUFFERED_SIZE - 1)
            {
                if (OutputBuffer->index < OutputBuffer->start)
                {
                    while (OutputBuffer->buffer[OutputBuffer->start] != 0)
                    {
                        int length = strlen(OutputBuffer->buffer + OutputBuffer->start) + 1;

                        OutputBuffer->buffer[OutputBuffer->start] = 0;
                        OutputBuffer->start += length;
                        OutputBuffer->count -= 1;
                    }

                    OutputBuffer->start = 0;
                }

                OutputBuffer->index = 0;
            }

            while (OutputBuffer->start - OutputBuffer->index <= n)
            {
                int length = strlen(OutputBuffer->buffer + OutputBuffer->start) + 1;

                OutputBuffer->buffer[OutputBuffer->start] = 0;
                OutputBuffer->start += length;
                OutputBuffer->count -= 1;

                if (OutputBuffer->buffer[OutputBuffer->start] == 0)
                {
                    OutputBuffer->start = 0;
                    break;
                }
            }
        }
#else
        if (OutputBuffer->index + n > gcdBUFFERED_SIZE)
        {
            _Flush(File);
            return;
        }
#endif

        memcpy(OutputBuffer->buffer + OutputBuffer->index, String, n);
        OutputBuffer->index += n;
        OutputBuffer->count += 1;
        OutputBuffer->buffer[OutputBuffer->index] = 0;
    }
#endif
}
#endif

static void
_Print(
    IN gctFILE File,
    IN gctCONST_STRING Message,
    IN va_list Arguments
    )
{
    /* Output to file or debugger. */
#if gcdDEBUG_IN_KERNEL
    gcsHAL_INTERFACE iface;

    iface.command        = gcvHAL_DEBUG;
    iface.u.Debug.set    = gcvTRUE;
    iface.u.Debug.level  = _debugLevel;
    iface.u.Debug.type   = gcvMESSAGE_TEXT;
    iface.u.Debug.zones  = _debugZones[gcmZONE_GET_API(gcvZONE_API_HAL)];
    iface.u.Debug.enable = gcvTRUE;

    gcmVSPRINTF(
        iface.u.Debug.message,
        sizeof(iface.u.Debug.message) - 1,
        Message,
        Arguments
        );

    gcoOS_DeviceControl(
        gcvNULL, IOCTL_GCHAL_INTERFACE,
        &iface, gcmSIZEOF(iface),
        &iface, gcmSIZEOF(iface)
        );
#else
    int i, j, n, indent;
    char buffer[4096];
    gcsBUFFERED_OUTPUT_PTR outputBuffer = gcvNULL;
#if gcdSEPARATE_THREADS
    static gctUINT32 prevThreadID;
#endif

#if gcdTHREAD_BUFFERS > 1
    gctUINT32 threadID;
#endif

    gcmLOCKSECTION();


#if gcdTHREAD_BUFFERS > 1
    /* Get the current thread ID. */
    threadID = gcmGETTHREADID();
#endif

    /* Initialize output buffer list. */
    if (_outputBufferHead == gcvNULL)
    {
        for (i = 0; i < gcdTHREAD_BUFFERS; i += 1)
        {
            if (_outputBufferTail == gcvNULL)
            {
                _outputBufferHead = &_outputBuffer[i];
            }
            else
            {
                _outputBufferTail->next = &_outputBuffer[i];
            }

#if gcdTHREAD_BUFFERS > 1
            _outputBuffer[i].threadID = ~0U;
#endif

            _outputBuffer[i].prev = _outputBufferTail;
            _outputBuffer[i].next =  gcvNULL;

            _outputBufferTail = &_outputBuffer[i];
        }
    }

#if gcdTHREAD_BUFFERS > 1
    /* Locate the output buffer for the thread. */
    outputBuffer = _outputBufferHead;

    while (outputBuffer != gcvNULL)
    {
        if (outputBuffer->threadID == threadID)
        {
            break;
        }

        outputBuffer = outputBuffer->next;
    }

    /* No matching buffer found? */
    if (outputBuffer == gcvNULL)
    {
        /* Get the tail for the buffer. */
        outputBuffer = _outputBufferTail;

        /* Move it to the head. */
        _outputBufferTail       = _outputBufferTail->prev;
        _outputBufferTail->next = gcvNULL;

        outputBuffer->prev = gcvNULL;
        outputBuffer->next = _outputBufferHead;

        _outputBufferHead->prev = outputBuffer;
        _outputBufferHead       = outputBuffer;

        /* Reset the buffer. */
        outputBuffer->threadID   = threadID;
#if gcdBUFFERED_OUTPUT
        outputBuffer->start      = 0;
        outputBuffer->index      = 0;
        outputBuffer->count      = 0;
#endif
#if gcdSHOW_LINE_NUMBER
        outputBuffer->lineNumber = 0;
#endif
    }
#else
    outputBuffer = _outputBufferHead;
#endif

#if gcdSEPARATE_THREADS && (gcdTHREAD_BUFFERS > 1)
    if ((prevThreadID != 0) && (prevThreadID != threadID))
    {
        OutputString(
            File, outputBuffer, "********************************************************************************\n"
            );
    }

    /* Update the previous thread value. */
    prevThreadID = threadID;
#endif

    if (strcmp(Message, "$$FLUSH$$") == 0)
    {
        OutputString(File, gcvNULL, gcvNULL);
        gcmUNLOCKSECTION();
        return;
    }

    i = 0;

#if gcdSHOW_LINE_NUMBER || gcdSHOW_PROCESS_ID || gcdSHOW_THREAD_ID || gcdSHOW_TIME
    buffer[i++] = '[';

#if gcdSHOW_TIME
    {
        gctUINT64 time;
        gcoOS_GetProfileTick(&time);
        i += gcmSPRINTF(
            buffer + i, sizeof(buffer) - i, "%12llu", time
            );
        buffer[sizeof(buffer) - 1] = '\0';
    }
#endif

#if gcdSHOW_LINE_NUMBER

#if gcdSHOW_TIME
    buffer[i++] = ',';
#endif

    {
        i += gcmSPRINTF(
            buffer + i, sizeof(buffer) - i, "%6llu", ++outputBuffer->lineNumber
            );
        buffer[sizeof(buffer) - 1] = '\0';
    }
#endif

#if gcdSHOW_PROCESS_ID

#if gcdSHOW_TIME || gcdSHOW_LINE_NUMBER
    buffer[i++] = ',';
#endif

    {
        gctUINT32 processID = gcmGETPROCESSID();
        i += gcmSPRINTF(
            buffer + i, sizeof(buffer) - i, "pid=%5u", processID
            );
        buffer[sizeof(buffer) - 1] = '\0';
    }
#endif

#if gcdSHOW_THREAD_ID

#if gcdTHREAD_BUFFERS > 1
#if gcdSHOW_TIME || gcdSHOW_LINE_NUMBER || gcdSHOW_PROCESS_ID
    buffer[i++] = ',';
#endif

    {
        i += gcmSPRINTF(
            buffer + i, sizeof(buffer) - i, "tid=%5u", threadID
            );
        buffer[sizeof(buffer) - 1] = '\0';
    }
#endif
#endif
    buffer[i++] = ']';
    buffer[i++] = ' ';
#endif

    if (strncmp(Message, "--", 2) == 0)
    {
        if (outputBuffer->indent == 0)
        {
            OutputString(File, outputBuffer, "ERROR: indent=0\n");
        }

        outputBuffer->indent -= 2;
    }

    indent = outputBuffer->indent % 40;

    for (j = 0; j < indent; ++j)
    {
        buffer[i++] = ' ';
    }

    if (indent != outputBuffer->indent)
    {
        i += gcmSPRINTF(
            buffer + i, sizeof(buffer) - i, " <%d> ", outputBuffer->indent
            );
        buffer[sizeof(buffer) - 1] = '\0';
    }

    /* Print message to buffer. */
    n = gcmVSPRINTF(buffer + i, sizeof(buffer) - i, Message, Arguments);
    buffer[sizeof(buffer) - 1] = '\0';

    if ((n <= 0) || (buffer[i + n - 1] != '\n'))
    {
        /* Append new-line. */
        gcmSTRCAT(buffer, sizeof(buffer), "\n");
        buffer[sizeof(buffer) - 1] = '\0';
    }

    /* Output to debugger. */
    OutputString(File, outputBuffer, buffer);

    if (strncmp(Message, "++", 2) == 0)
    {
        outputBuffer->indent += 2;
    }

    gcmUNLOCKSECTION();
#endif
}

static gctFILE
_SetDumpFile(
    IN gctFILE File,
    IN gctBOOL CloseOldFile
    )
{
    gctFILE oldFile = gcvNULL;
    gctUINT32 selfThreadID = gcmGETTHREADID();
    gctUINT32 pos;
    gctUINT32 tmpCurPos;

    gcmLOCKSECTION();
    tmpCurPos = _currentPos;

    /* Find if this thread has already been recorded */
    for (pos = 0; pos < _usedFileSlot; pos++)
    {
        if (selfThreadID == _FileArray[pos]._threadID)
        {
            _Flush(_FileArray[pos]._debugFile);
            if (_FileArray[pos]._debugFile != gcvNULL &&
                _FileArray[pos]._debugFile != File    &&
                CloseOldFile)
            {
                /* Close the earliest existing file handle. */
                fclose(_FileArray[pos]._debugFile);
                _FileArray[pos]._debugFile =  gcvNULL;
            }

            oldFile = _FileArray[pos]._debugFile;
            /* Replace old file by new file */
            _FileArray[pos]._debugFile = File;
            goto exit;
        }
    }

    /* Test if we have exhausted our thread buffers. One thread one buffer. */
    if (tmpCurPos == STACK_THREAD_NUMBER)
    {
        goto error;
    }

    /* Record this new thread */
    _FileArray[tmpCurPos]._debugFile = File;
    _FileArray[tmpCurPos]._threadID = selfThreadID;
    _currentPos = ++tmpCurPos;

    if (_usedFileSlot < STACK_THREAD_NUMBER)
    {
        _usedFileSlot++;
    }

exit:
    gcmUNLOCKSECTION();
    return oldFile;

error:
    gcmUNLOCKSECTION();
    gcmPRINT("ERROR: Not enough dump file buffers. Buffer num = %d", STACK_THREAD_NUMBER);
    return oldFile;
}

static gctFILE
_GetDumpFile()
{
    gctUINT32 selfThreadID;
    gctUINT32 pos = 0;
    gctFILE retFile = gcvNULL;

    gcmLOCKSECTION();

    if (_usedFileSlot == 0)
    {
        goto exit;
    }

    selfThreadID = gcmGETTHREADID();
    for (; pos < _usedFileSlot; pos++)
    {
        if (selfThreadID == _FileArray[pos]._threadID)
        {
            retFile = _FileArray[pos]._debugFile;
            goto exit;
        }
    }

exit:
    gcmUNLOCKSECTION();
    return retFile;
}

/******************************************************************************\
********************************* Debug Macros *********************************
\******************************************************************************/

#define gcmDEBUGPRINT(FileHandle, Message) \
{ \
    va_list arguments; \
    \
    va_start(arguments, Message); \
    _Print(FileHandle, Message, arguments); \
    va_end(arguments); \
    FFLUSH(FileHandle); \
}


/******************************************************************************\
********************************** Debug Code **********************************
\******************************************************************************/

/*******************************************************************************
**
**  gcoOS_Print
**
**  Send a message to the debugger.
**
**  INPUT:
**
**      gctCONST_STRING Message
**          Pointer to message.
**
**      ...
**          Optional arguments.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_Print(
    IN gctCONST_STRING Message,
    ...
    )
{
    gcmDEBUGPRINT(_GetDumpFile(), Message);
}

/*******************************************************************************
**
**  gcoOS_DebugTrace
**
**  Send a leveled message to the debugger.
**
**  INPUT:
**
**      gctUINT32 Level
**          Debug level of message.
**
**      gctCONST_STRING Message
**          Pointer to message.
**
**      ...
**          Optional arguments.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_DebugTrace(
    IN gctUINT32 Level,
    IN gctCONST_STRING Message,
    ...
    )
{
    if (Level > _debugLevel)
    {
        return;
    }

    gcmDEBUGPRINT(_GetDumpFile(), Message);
}

/*******************************************************************************
**
**  gcoOS_DebugTraceZone
**
**  Send a leveled and zoned message to the debugger.
**
**  INPUT:
**
**      gctUINT32 Level
**          Debug level for message.
**
**      gctUINT32 Zone
**          Debug zone for message.
**
**      gctCONST_STRING Message
**          Pointer to message.
**
**      ...
**          Optional arguments.
**
**  OUTPUT:
**
**      Nothing.
*/
static void _DumpAPI(
    IN gctUINT32 Level,
    IN gctUINT32 Zone
    );
void
gcoOS_DebugTraceZone(
    IN gctUINT32 Level,
    IN gctUINT32 Zone,
    IN gctCONST_STRING Message,
    ...
    )
{
    if(Message != gcvNULL && Message[0] == '+') _DumpAPI(Level,Zone);
    /* Verify that the debug level and zone are valid. */
    if ((Level > _debugLevel)
    ||  !(_debugZones[gcmZONE_GET_API(Zone)] & Zone & gcdZONE_MASK)
    )
    {
        return;
    }

    if (Message != gcvNULL)
    {
        gcmDEBUGPRINT(_GetDumpFile(), Message);
    }
}

/*******************************************************************************
**
**  gcoOS_DebugBreak
**
**  Break into the debugger.
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
*/
void
gcoOS_DebugBreak(
    void
    )
{
    gcoOS_DebugTrace(gcvLEVEL_ERROR, "%s(%d)", __FUNCTION__, __LINE__);
}

/*******************************************************************************
**
**  gcoOS_DebugFatal
**
**  Send a message to the debugger and break into the debugger.
**
**  INPUT:
**
**      gctCONST_STRING Message
**          Pointer to message.
**
**      ...
**          Optional arguments.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_DebugFatal(
    IN gctCONST_STRING Message,
    ...
    )
{
    gcmPRINT_VERSION();
    gcmDEBUGPRINT(_GetDumpFile(), Message);

    /* Break into the debugger. */
    gcoOS_DebugBreak();
}

/*******************************************************************************
**
**  gcoOS_SetDebugLevel
**
**  Set the debug level.
**
**  INPUT:
**
**      gctUINT32 Level
**          New debug level.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugLevel(
    IN gctUINT32 Level
    )
{
    _debugLevel = Level;
}

/*******************************************************************************
**
**  gcoOS_GetDebugLevel
**
**  Get the current debug level.
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      gctUINT32_PTR DebugLevel
**          Handle to store the debug level.
*/
void
gcoOS_GetDebugLevel(
    OUT gctUINT32_PTR DebugLevel
    )
{
    *DebugLevel = _debugLevel;
}

/*******************************************************************************
**
**  gcoOS_SetDebugZone
**
**  Set the debug zone.
**
**  INPUT:
**
**      gctUINT32 Zone
**          New debug zone.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugZone(
    IN gctUINT32 Zone
    )
{
    _debugZones[gcmZONE_GET_API(Zone)] = Zone;
}

/*******************************************************************************
**
**  gcoOS_GetDebugZone
**
**  Get the current debug zone.
**
**  INPUT:
**
**      gctUINT32 Zone
**          Debug API zone.
**
**  OUTPUT:
**
**      gctUINT32_PTR DebugZone
**          Handle to store the debug zone.
*/
void
gcoOS_GetDebugZone(
    IN gctUINT32 Zone,
    OUT gctUINT32_PTR DebugZone
    )
{
    *DebugZone = _debugZones[gcmZONE_GET_API(Zone)];
}

/*******************************************************************************
**
**  gcoOS_SetDebugLevelZone
**
**  Set the debug level and zone.
**
**  INPUT:
**
**      gctUINT32 Level
**          New debug level.
**
**      gctUINT32 Zone
**          New debug zone.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugLevelZone(
    IN gctUINT32 Level,
    IN gctUINT32 Zone
    )
{
    _debugLevel                        = Level;
    _debugZones[gcmZONE_GET_API(Zone)] = Zone;
}

/*******************************************************************************
**
**  gcoOS_SetDebugZones
**
**  Enable or disable debug zones.
**
**  INPUT:
**
**      gctUINT32 Zones
**          Debug zones to enable or disable.
**
**      gctBOOL Enable
**          Set to gcvTRUE to enable the zones (or the Zones with the current
**          zones) or gcvFALSE to disable the specified Zones.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugZones(
    IN gctUINT32 Zones,
    IN gctBOOL Enable
    )
{
    if (Enable)
    {
        /* Enable the zones. */
        _debugZones[gcmZONE_GET_API(Zones)] |= Zones;
    }
    else
    {
        /* Disable the zones. */
        _debugZones[gcmZONE_GET_API(Zones)] &= ~Zones;
    }
}

/*******************************************************************************
**
**  gcoOS_Verify
**
**  Called to verify the result of a function call.
**
**  INPUT:
**
**      gceSTATUS Status
**          Function call result.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_Verify(
    IN gceSTATUS status
    )
{
    _lastError = status;
}

/*******************************************************************************
**
**  gcoOS_SetDebugFile
**
**  Open or close the debug file.
**
**  INPUT:
**
**      gcoOS Os
**          Pointer to gcoOS object.
**
**      gctCONST_STRING FileName
**          Name of debug file to open or gcvNULL to close the current debug
**          file.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugFile(
    IN gctCONST_STRING FileName
    )
{
    gctFILE debugFile;

    if (FileName != gcvNULL)
    {
        /* Don't change it to 'w' !!!*/
        debugFile = fopen(FileName, "a");
        _SetDumpFile(debugFile, gcvTRUE);
    }
}

/*******************************************************************************
**
**  gcoOS_ReplaceDebugFile
**
**  Replace the debug file to new FILE pointer.
**
**  INPUT:
**
**
**      gctFILE fp
**          The new debug file pointer (already opened).
**
**  OUTPUT:
**
**      The old debug file pointer.
*/
gctFILE
gcoOS_ReplaceDebugFile(
    IN gctFILE fp
    )
{
    gctFILE old_fp = _SetDumpFile(fp, gcvFALSE);

    return old_fp;
}

/*******************************************************************************
**
**  gcoOS_SetDebugShaderFiles
**
**  Called to redirect shader debug output to file(s).
**  Passing gcvNULL argument closes previously open file handles.
**
**  INPUT:
**
**      gctCONST_STRING VSFileName
**          Vertex Shader Filename.
**
**      gctCONST_STRING FSFileName
**          Fragment Shader Filename.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugShaderFiles(
    IN gctCONST_STRING VSFileName,
    IN gctCONST_STRING FSFileName
    )
{
    if (_debugFileVS != gcvNULL)
    {
        fclose(_debugFileVS);
        _debugFileVS = gcvNULL;
    }

    if (_debugFileFS != gcvNULL)
    {
        fclose(_debugFileFS);
        _debugFileFS = gcvNULL;
    }

    if (VSFileName != gcvNULL)
    {
        _debugFileVS = fopen(VSFileName, "w");
    }

    if (FSFileName != gcvNULL)
    {
        _debugFileFS = fopen(FSFileName, "w");
    }
}

/*******************************************************************************
**
**  gcoOS_SetDebugShaderFileType
**
**  Called to set debugging output to vertex/fragment shader file.
**
**  INPUT:
**
**      gctUINT32 ShaderType
**          0 for Vertex Shader, 1 for Fragment Shader.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_SetDebugShaderFileType(
    IN gctUINT32 ShaderType
    )
{
    if (ShaderType <= 1)
    {
        _shaderFileType = ShaderType;
    }
}

/*******************************************************************************
**
**  gcoOS_DebugShaderTrace
**
**  Dump a message to a shader file.
**
**  INPUT:
**
**      gctCONST_STRING Message
**          Pointer to message.
**
**      ...
**          Optional arguments.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_DebugShaderTrace(
    IN gctCONST_STRING Message,
    ...
    )
{
    FILE * file;

    /* Verify that the shader file handle is valid. */
    if (_shaderFileType && (_debugFileFS != gcvNULL))
    {
        file = _debugFileFS;
    }
    else if (!_shaderFileType && (_debugFileVS != gcvNULL))
    {
        file = _debugFileVS;
    }
    else
    {
        return;
    }

    gcmDEBUGPRINT(file, Message);
}

/*******************************************************************************
**
**  gcoOS_EnableDebugBuffer
**
**  Enable internal buffering of debug output.
**
**  INPUT:
**
**      gctBOOL Enable
**          Set debug buffering.
**
**  OUTPUT:
**
**      Nothing.
*/

void
gcoOS_EnableDebugBuffer(
    IN gctBOOL Enable
    )
{
    _debugBuffering = Enable;
}

/*******************************************************************************
**
**  gcoOS_DebugStatus2Name
**
**  transform status enum to status name.
**
**  INPUT:
**
**      gceSTATUS status Message
**
**  OUTPUT:
**
**      gctCONST_STRING status name.
*/

gctCONST_STRING
gcoOS_DebugStatus2Name(
    gceSTATUS status
    )
{
    switch (status)
    {
    case gcvSTATUS_OK:
        return "gcvSTATUS_OK";
    case gcvSTATUS_TRUE:
        return "gcvSTATUS_TRUE";
    case gcvSTATUS_NO_MORE_DATA:
        return "gcvSTATUS_NO_MORE_DATA";
    case gcvSTATUS_CACHED:
        return "gcvSTATUS_CACHED";
    case gcvSTATUS_MIPMAP_TOO_LARGE:
        return "gcvSTATUS_MIPMAP_TOO_LARGE";
    case gcvSTATUS_NAME_NOT_FOUND:
        return "gcvSTATUS_NAME_NOT_FOUND";
    case gcvSTATUS_NOT_OUR_INTERRUPT:
        return "gcvSTATUS_NOT_OUR_INTERRUPT";
    case gcvSTATUS_MISMATCH:
        return "gcvSTATUS_MISMATCH";
    case gcvSTATUS_MIPMAP_TOO_SMALL:
        return "gcvSTATUS_MIPMAP_TOO_SMALL";
    case gcvSTATUS_LARGER:
        return "gcvSTATUS_LARGER";
    case gcvSTATUS_SMALLER:
        return "gcvSTATUS_SMALLER";
    case gcvSTATUS_CHIP_NOT_READY:
        return "gcvSTATUS_CHIP_NOT_READY";
    case gcvSTATUS_NEED_CONVERSION:
        return "gcvSTATUS_NEED_CONVERSION";
    case gcvSTATUS_SKIP:
        return "gcvSTATUS_SKIP";
    case gcvSTATUS_DATA_TOO_LARGE:
        return "gcvSTATUS_DATA_TOO_LARGE";
    case gcvSTATUS_INVALID_CONFIG:
        return "gcvSTATUS_INVALID_CONFIG";
    case gcvSTATUS_CHANGED:
        return "gcvSTATUS_CHANGED";
    case gcvSTATUS_NOT_SUPPORT_DITHER:
        return "gcvSTATUS_NOT_SUPPORT_DITHER";
    case gcvSTATUS_EXECUTED:
        return "gcvSTATUS_EXECUTED";
    case gcvSTATUS_TERMINATE:
        return "gcvSTATUS_TERMINATE";

    case gcvSTATUS_INVALID_ARGUMENT:
        return "gcvSTATUS_INVALID_ARGUMENT";
    case gcvSTATUS_INVALID_OBJECT:
        return "gcvSTATUS_INVALID_OBJECT";
    case gcvSTATUS_OUT_OF_MEMORY:
        return "gcvSTATUS_OUT_OF_MEMORY";
    case gcvSTATUS_MEMORY_LOCKED:
        return "gcvSTATUS_MEMORY_LOCKED";
    case gcvSTATUS_MEMORY_UNLOCKED:
        return "gcvSTATUS_MEMORY_UNLOCKED";
    case gcvSTATUS_HEAP_CORRUPTED:
        return "gcvSTATUS_HEAP_CORRUPTED";
    case gcvSTATUS_GENERIC_IO:
        return "gcvSTATUS_GENERIC_IO";
    case gcvSTATUS_INVALID_ADDRESS:
        return "gcvSTATUS_INVALID_ADDRESS";
    case gcvSTATUS_CONTEXT_LOSSED:
        return "gcvSTATUS_CONTEXT_LOSSED";
    case gcvSTATUS_TOO_COMPLEX:
        return "gcvSTATUS_TOO_COMPLEX";
    case gcvSTATUS_BUFFER_TOO_SMALL:
        return "gcvSTATUS_BUFFER_TOO_SMALL";
    case gcvSTATUS_INTERFACE_ERROR:
        return "gcvSTATUS_INTERFACE_ERROR";
    case gcvSTATUS_NOT_SUPPORTED:
        return "gcvSTATUS_NOT_SUPPORTED";
    case gcvSTATUS_MORE_DATA:
        return "gcvSTATUS_MORE_DATA";
    case gcvSTATUS_TIMEOUT:
        return "gcvSTATUS_TIMEOUT";
    case gcvSTATUS_OUT_OF_RESOURCES:
        return "gcvSTATUS_OUT_OF_RESOURCES";
    case gcvSTATUS_INVALID_DATA:
        return "gcvSTATUS_INVALID_DATA";
    case gcvSTATUS_INVALID_MIPMAP:
        return "gcvSTATUS_INVALID_MIPMAP";
    case gcvSTATUS_NOT_FOUND:
        return "gcvSTATUS_NOT_FOUND";
    case gcvSTATUS_NOT_ALIGNED:
        return "gcvSTATUS_NOT_ALIGNED";
    case gcvSTATUS_INVALID_REQUEST:
        return "gcvSTATUS_INVALID_REQUEST";
    case gcvSTATUS_GPU_NOT_RESPONDING:
        return "gcvSTATUS_GPU_NOT_RESPONDING";
    case gcvSTATUS_TIMER_OVERFLOW:
        return "gcvSTATUS_TIMER_OVERFLOW";
    case gcvSTATUS_VERSION_MISMATCH:
        return "gcvSTATUS_VERSION_MISMATCH";
    case gcvSTATUS_LOCKED:
        return "gcvSTATUS_LOCKED";
    case gcvSTATUS_INTERRUPTED:
        return "gcvSTATUS_INTERRUPTED";
    case gcvSTATUS_DEVICE:
        return "gcvSTATUS_DEVICE";
    case gcvSTATUS_NOT_MULTI_PIPE_ALIGNED:
        return "gcvSTATUS_NOT_MULTI_PIPE_ALIGNED";

    /* Linker errors. */
    case gcvSTATUS_GLOBAL_TYPE_MISMATCH:
        return "gcvSTATUS_GLOBAL_TYPE_MISMATCH";
    case gcvSTATUS_TOO_MANY_ATTRIBUTES:
        return "gcvSTATUS_TOO_MANY_ATTRIBUTES";
    case gcvSTATUS_TOO_MANY_UNIFORMS:
        return "gcvSTATUS_TOO_MANY_UNIFORMS";
    case gcvSTATUS_TOO_MANY_SAMPLER:
        return "gcvSTATUS_TOO_MANY_SAMPLER";
    case gcvSTATUS_TOO_MANY_VARYINGS:
        return "gcvSTATUS_TOO_MANY_VARYINGS";
    case gcvSTATUS_UNDECLARED_VARYING:
        return "gcvSTATUS_UNDECLARED_VARYING";
    case gcvSTATUS_VARYING_TYPE_MISMATCH:
        return "gcvSTATUS_VARYING_TYPE_MISMATCH";
    case gcvSTATUS_MISSING_MAIN:
        return "gcvSTATUS_MISSING_MAIN";
    case gcvSTATUS_NAME_MISMATCH:
        return "gcvSTATUS_NAME_MISMATCH";
    case gcvSTATUS_INVALID_INDEX:
        return "gcvSTATUS_INVALID_INDEX";
    case gcvSTATUS_UNIFORM_MISMATCH:
        return "gcvSTATUS_UNIFORM_MISMATCH";
    case gcvSTATUS_UNSAT_LIB_SYMBOL:
        return "gcvSTATUS_UNSAT_LIB_SYMBOL";
    case gcvSTATUS_TOO_MANY_SHADERS:
        return "gcvSTATUS_TOO_MANY_SHADERS";
    case gcvSTATUS_LINK_INVALID_SHADERS:
        return "gcvSTATUS_LINK_INVALID_SHADERS";
    case gcvSTATUS_CS_NO_WORKGROUP_SIZE:
        return "gcvSTATUS_CS_NO_WORKGROUP_SIZE";
    case gcvSTATUS_LINK_LIB_ERROR:
        return "gcvSTATUS_LINK_LIB_ERROR";
    case gcvSTATUS_SHADER_VERSION_MISMATCH:
        return "gcvSTATUS_SHADER_VERSION_MISMATCH";
    case gcvSTATUS_TOO_MANY_INSTRUCTION:
        return "gcvSTATUS_TOO_MANY_INSTRUCTION";
    case gcvSTATUS_SSBO_MISMATCH:
        return "gcvSTATUS_SSBO_MISMATCH";
    case gcvSTATUS_TOO_MANY_OUTPUT:
        return "gcvSTATUS_TOO_MANY_OUTPUT";
    case gcvSTATUS_TOO_MANY_INPUT:
        return "gcvSTATUS_TOO_MANY_INPUT";
    case gcvSTATUS_NOT_SUPPORT_CL:
        return "gcvSTATUS_NOT_SUPPORT_CL";
    case gcvSTATUS_NOT_SUPPORT_INTEGER:
        return "gcvSTATUS_NOT_SUPPORT_INTEGER";

    /* Compiler errors. */
    case gcvSTATUS_COMPILER_FE_PREPROCESSOR_ERROR:
        return "gcvSTATUS_COMPILER_FE_PREPROCESSOR_ERROR";
    case gcvSTATUS_COMPILER_FE_PARSER_ERROR:
        return "gcvSTATUS_COMPILER_FE_PARSER_ERROR";

    default:
        return "nil";
    }
}

/*******************************************************************************
***** Trace Stack Management ***************************************************
*******************************************************************************/

typedef struct _gcsSTACK
{
    gctINT8_PTR         identity;
    gctCONST_STRING     function;
    gctINT              line;
    gctCONST_STRING     text;
    gctPOINTER          arguments[12];
}
gcsSTACK;

typedef struct _gcsTRACE_STACK
{
    gctBOOL             initialized;
    gctHANDLE           thread;
    gcsSTACK            stack[128];
    gctINT              level;
}
gcsTRACE_STACK;

/* Allocate enough for STACK_THREAD_NUMBER threads. */
static gcsTRACE_STACK stacks[STACK_THREAD_NUMBER];
static pthread_mutex_t stacksMutex = PTHREAD_MUTEX_INITIALIZER;

/*******************************************************************************
**  _ReleaseStack
**
**  Release the trace stack when the thread will quit.
**
**  ARGUMENTS:
**
**      gcsTRACE_STACK *
**
*/

static void _ReleaseStack(gcsTRACE_STACK * TraceStack)
{
        TraceStack->thread      = gcvNULL;
        TraceStack->level       = 0;
        memset(TraceStack->stack,0,gcmSIZEOF(TraceStack->stack));
        TraceStack->initialized = gcvFALSE;
}

/*******************************************************************************
**  _FindStack
**
**  Find the trace stack belonging to the current thread.
**
**  ARGUMENTS:
**
**      <NONE>
**
**  RETURNS:
**
**      gcsTRACE_STACK *
**          Pointer to the trace stack for the current thread or gcvNULL if
**          there are not enough trace stacks.
*/
static gcsTRACE_STACK * _FindStack(void)
{
    gctSIZE_T i;
    gcsTRACE_STACK * empty = gcvNULL;
    gctHANDLE thread = gcoOS_GetCurrentThreadID();

    pthread_mutex_lock(&stacksMutex);

    /* Walk all stacks. */
    for (i = 0; i < gcmCOUNTOF(stacks); ++i)
    {
        /* Test if this stack belongs to this thread. */
        if (stacks[i].thread == thread)
        {
            pthread_mutex_unlock(&stacksMutex);

            /* It does. */
            return stacks + i;
        }

        /* Test if this stack is empty. */
        else if ((empty == gcvNULL) && !stacks[i].initialized)
        {
            /* It is. */
            empty = stacks + i;
        }
    }

    /* Test if we have exhausted our threads. */
    if (empty == gcvNULL)
    {
        gcmPRINT("ERROR: Not enough trace buffers for threads.");
    }
    else
    {
        /* Initialize stack. */
        empty->initialized = gcvTRUE;
        empty->thread      = thread;
        empty->level       = 0;
    }

    pthread_mutex_unlock(&stacksMutex);

    /* Return empty stack. */
    return empty;
}

/*******************************************************************************
**  gcoOS_StackPush
**
**  Push a function onto the trace stack.
**
**  ARGUMENTS:
**
**      gctCONST_STRING Function
**          Pointer to function name.
**
**      gctINT Line
**          Line number.
**
**      gctCONST_STRING Text OPTIONAL
**          Optional pointer to a descriptive text.
**
**      ...
**          Optional arguments to the descriptive text.
*/
void
gcoOS_StackPush(
    IN gctINT8_PTR Identity,
    IN gctCONST_STRING Function,
    IN gctINT Line,
    IN gctCONST_STRING Text OPTIONAL,
    ...
    )
{
    /* Find our stack. */
    gcsTRACE_STACK * traceStack = _FindStack();
    if (traceStack == gcvNULL)
    {
        return;
    }

    /* Test for stack overflow. */
    if (traceStack->level >= (gctINT)gcmCOUNTOF(traceStack->stack))
    {
        gcmPRINT("ERROR(%s): Trace stack overflow.", Function);
    }
    else
    {
        /* Push arguments onto the stack. */
        gcsSTACK* stack = &traceStack->stack[traceStack->level++];
        stack->identity = Identity;
        stack->function = Function;
        stack->line     = Line;
        stack->text     = Text;

        if (Text != gcvNULL)
        {
            /* Copy the arguments. */
            gctSIZE_T i;
            gctPOINTER * arguments = ((gctPOINTER *) &Text) + 1;
            for (i = 0; i < gcmCOUNTOF(stack->arguments); ++i)
            {
                stack->arguments[i] = arguments[i];
            }
        }
    }
}

/*******************************************************************************
**  gcoOS_StackPop
**
**  Pop a function from the trace stack.
**
**  ARGUMENTS:
**
**      gctCONST_STRING Function
**          Pointer to function name.
*/
void
gcoOS_StackPop(
    IN gctINT8_PTR Identity,
    IN gctCONST_STRING Function
    )
{
    /* Find our stack. */
    gcsTRACE_STACK * traceStack = _FindStack();
    if (traceStack == gcvNULL)
    {
        return;
    }

    if (traceStack->level > 0)
    {
        /* Pop arguments from the stack. */
        gcsSTACK* stack = &traceStack->stack[--traceStack->level];

        /* Check for function mismatch. */
        if (stack->identity != Identity)
        {
            gctINT32 levelCheck;
            gcsSTACK* stackTmp;

            for (levelCheck = traceStack->level; levelCheck >= 0; levelCheck--)
            {
                stackTmp = &traceStack->stack[levelCheck];
                if (stackTmp->identity == Identity)
                {
                     /* Skip all miss-footer funcions in stack */
                    traceStack->level = levelCheck;
                    break;
                }
                else
                {
                    gcmPRINT("ERROR(%s): Trace stack mismatch (%s:%d).",
                             Function, stackTmp->function, stackTmp->line);
                }
            }
        }

        /*Stack is NULL, release this buffer*/
        if (traceStack->level == 0)
        {
            _ReleaseStack(traceStack);
        }
    }
    /* Test for stack underflow. */
    else if (traceStack->level <= 0)
    {
        gcmPRINT("ERROR(%s): Trace stack underflow.", Function);
    }

}

/*******************************************************************************
**  gcoOS_StackDump
**
**  Dump the current trace stack.
**
**  ARGUMENTS:
**
**      <NONE>
*/
void
gcoOS_StackDump(
    void
    )
{
    /* Find our stack. */
    gcsTRACE_STACK * traceStack = _FindStack();
    if (traceStack == gcvNULL)
    {
        return;
    }

    if (traceStack->level > 0)
    {
        gctINT l;

        gcoOS_Print("Trace Stack Backtrace:");

        for (l = traceStack->level - 1; l >= 0; --l)
        {
            gcsSTACK* stack = &traceStack->stack[l];

            gcmPRINT("  [%d] %s(%d)", l, stack->function, stack->line);
            if (stack->text != gcvNULL)
            {
                char buffer[192] = "";
                gctUINT offset = 0;
                gctPOINTER pointer = (gctPOINTER) stack->arguments;

                gcoOS_PrintStrVSafe(buffer, gcmSIZEOF(buffer),
                                    &offset, stack->text, *(gctARGUMENTS *) &pointer);

                gcmPRINT("    (%s)", buffer);
            }
        }
    }
}


/*******************************************************************************
**  _DumpAPI
**
**  Send a leveled and zoned message to the debugger.
**
**  INPUT:
**
**      gctUINT32 Level
**          Debug level for message.
**
**      gctUINT32 Zone
**          Debug zone for message.
**
**  OUTPUT:
**
**      Nothing.
*/

static void
_DumpAPI(
    IN gctUINT32 Level,
    IN gctUINT32 Zone
    )
{
    gcsTRACE_STACK * traceStack;
    /* Verify that the debug level and zone are valid. */
    if ((Level > _debugLevel)
    ||  !(_dumpAPIZones[gcmZONE_GET_API(Zone)])
    )
    {
        return;
    }

    /* Find our stack. */
    traceStack = _FindStack();
    if (traceStack == gcvNULL)
    {
        return;
    }

    if (traceStack->level > gcvDUMP_API_DEPTH || traceStack->level<=0)
    {
        return;
    }
    else
    {
        gcsSTACK* stack = &traceStack->stack[traceStack->level-1];

        gcmPRINT("  [%d] %s(%d)", traceStack->level, stack->function, stack->line);
        if (stack->text != gcvNULL)
        {
            char buffer[192] = "";
            gctUINT offset = 0;
            gctPOINTER pointer = (gctPOINTER) stack->arguments;

                gcoOS_PrintStrVSafe(buffer, gcmSIZEOF(buffer),
                                    &offset, stack->text, *(gctARGUMENTS *) &pointer);
            gcmPRINT("    (%s)", buffer);
        }
    }
}

/*******************************************************************************
***** Binary Trace *************************************************************
*******************************************************************************/

/*******************************************************************************
**  _VerifyMessage
**
**  Verify a binary trace message, decode it to human readable string and print
**  it.
**
**  ARGUMENTS:
**
**      gctCONST_STRING Buffer
**          Pointer to buffer to store.
**
**      gctSIZE_T Bytes
**          Buffer length.
*/
void
_VerifyMessage(
    IN gctCONST_STRING Buffer,
    IN gctSIZE_T Bytes
    )
{
    char arguments[150] = {0};
    char format[100] = {0};

    gctSTRING function;
    gctPOINTER args;
    gctUINT32 numArguments;
    int i = 0;
    gctUINT32 functionBytes;

    gcsBINARY_TRACE_MESSAGE_PTR message = (gcsBINARY_TRACE_MESSAGE_PTR)Buffer;

    /* Check signature. */
    if (message->signature != 0x7FFFFFFF)
    {
        gcmPRINT("Signature error");
        return;
    }

    /* Get function name. */
    function = (gctSTRING)&message->payload;
    functionBytes = strlen(function) + 1;

    /* Get arguments number. */
    numArguments = message->numArguments;

    /* Get arguments . */
    args = function + functionBytes;

    /* Prepare format string. */
    while (numArguments--)
    {
        format[i++] = '%';
        format[i++] = 'x';
        format[i++] = ' ';
    }

    format[i] = '\0';

    if (numArguments)
    {
        vsnprintf(arguments, 150, format, *(gctARGUMENTS *) &args);
    }

    gcmPRINT("[%d](%d): %s(%d) %s",
             message->pid,
             message->tid,
             function,
             message->line,
             arguments);
}

#if gcdBINARY_TRACE_FILE_SIZE
static FILE *  _binaryTraceFile;
#endif
/*******************************************************************************
**  gcoOS_WriteToStorage
**
**  Store a buffer, as a example, raw buffer is written to a file.
**
**  ARGUMENTS:
**
**      gctCONST_STRING Buffer
**          Pointer to buffer to store.
**
**      gctSIZE_T Bytes
**          Buffer length.
*/
void
gcoOS_WriteToStorage(
    IN gctCONST_STRING Buffer,
    IN gctSIZE_T Bytes
    )
{
    /* Implement a buffer storage mechanisam to record binary trace buffer. */
#if gcdBINARY_TRACE_FILE_SIZE
    char binaryTraceFileName[64];
    static gctUINT32 fileBytes = 0;

    if (_binaryTraceFile == gcvNULL)
    {
        sprintf(binaryTraceFileName, "%x.trace", getpid());
        _binaryTraceFile = fopen(binaryTraceFileName, "w");
    }

    fwrite(Buffer, 1, Bytes, _binaryTraceFile);

    fileBytes += Bytes;

    if (fileBytes >= gcdBINARY_TRACE_FILE_SIZE)
    {
        rewind(_binaryTraceFile);
        fileBytes = 0;
    }
#endif
}

/*******************************************************************************
**  gcoOS_BinaryTrace
**
**  Output a binary trace message.
**
**  ARGUMENTS:
**
**      gctCONST_STRING Function
**          Pointer to function name.
**
**      gctINT Line
**          Line number.
**
**      gctCONST_STRING Text OPTIONAL
**          Optional pointer to a descriptive text.
**
**      ...
**          Optional arguments to the descriptive text.
*/
void
gcoOS_BinaryTrace(
    IN gctCONST_STRING Function,
    IN gctINT Line,
    IN gctCONST_STRING Text OPTIONAL,
    ...
    )
{
    static gctUINT32 messageSignature = 0x7FFFFFFF;
    char buffer[gcdBINARY_TRACE_MESSAGE_SIZE];
    gctUINT32 numArguments = 0;
    gctUINT32 functionBytes;
    gctUINT32 i = 0;
    gctSTRING payload;
    gcsBINARY_TRACE_MESSAGE_PTR message = (gcsBINARY_TRACE_MESSAGE_PTR)buffer;

    /* Calculate arguments number. */
    if (Text)
    {
        while (Text[i] != '\0')
        {
            if (Text[i] == '%')
            {
                numArguments++;
            }
            i++;
        }
    }

    message->signature    = messageSignature;
    message->pid          = gcmGETPROCESSID();
    message->tid          = gcmGETTHREADID();
    message->line         = Line;
    message->numArguments = numArguments;

    payload = (gctSTRING)&message->payload;

    /* Function name. */
    functionBytes = strlen(Function) + 1;
    memcpy(payload, Function, functionBytes);

    /* Advance to next payload. */
    payload += functionBytes;

    /* Arguments value. */
    if (numArguments)
    {
        va_list p;
        va_start(p, Text);

        for (i = 0; i < numArguments; ++i)
        {
            gctPOINTER value = va_arg(p, gctPOINTER);
            memcpy(payload, &value, gcmSIZEOF(gctPOINTER));
            payload += gcmSIZEOF(gctPOINTER);
        }

        va_end(p);
    }

    gcmASSERT(payload - buffer <= gcdBINARY_TRACE_MESSAGE_SIZE);


    /* Send buffer to ring buffer. */
    gcoOS_WriteToStorage(buffer, payload - buffer);
}

#if !defined(ANDROID) || (ANDROID_SDK_VERSION < 18)
static pthread_mutex_t ATraceMutex = PTHREAD_MUTEX_INITIALIZER;
static int ATraceFD = -1;

static gctBOOL _ATraceInit()
{
    static gctBOOL once = gcvFALSE;

    pthread_mutex_lock(&ATraceMutex);

    if ((ATraceFD == -1) && (!once))
    {
        const char* const traceFileName =
              "/sys/kernel/debug/tracing/trace_marker";

        ATraceFD = open(traceFileName, O_WRONLY);

        if (ATraceFD == -1)
        {
            gcmPRINT("error opening trace file: %s (%d)", strerror(errno), errno);
        }
        once = gcvTRUE;
    }

    pthread_mutex_unlock(&ATraceMutex);

    return (ATraceFD != -1);
}

static void _ATraceBegin(const char* name)
{
    if (_ATraceInit()) {
        char buf[1024];
        size_t len = snprintf(buf, 1024, "B|%d|%s", getpid(), name);

        if (len > 0)
        {
            if (write(ATraceFD, buf, len)) {};
        }
    }
}

static void _ATraceEnd()
{
    if (_ATraceInit()) {
        char buf = 'E';
        if (write(ATraceFD, &buf, 1)) {};
    }
}
#endif

void
gcoOS_SysTraceBegin(
    IN gctCONST_STRING FuncName
    )
{
#if defined(ANDROID) && (ANDROID_SDK_VERSION >= 18)
    ATRACE_BEGIN(FuncName);
#else
    _ATraceBegin(FuncName);
#endif

}

void
gcoOS_SysTraceEnd(
    IN void
    )
{
#if defined(ANDROID) && (ANDROID_SDK_VERSION >= 18)
    ATRACE_END();
#else
    _ATraceEnd();
#endif

}


