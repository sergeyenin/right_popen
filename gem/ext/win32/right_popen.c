///////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2010 RightScale Inc
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////

#include "right_popen.h"

#define MAX_ERROR_DESCRIPTION_LENGTH 512
#define ASYNC_IO_BUFFER_SIZE (1 << 12)  // 4KB

typedef struct IOHandlePairType
{
    HANDLE hRead;
    HANDLE hWrite;
} IOHandlePair;

typedef struct AsyncIODataType
{
    DWORD nBytesRead;
    BOOL bPending;
    OVERLAPPED overlapped;
    char buffer[ASYNC_IO_BUFFER_SIZE + 1];  // buffer size plus nul guard byte
} AsyncIOData;

typedef struct Open3ProcessDataType
{
   struct Open3ProcessDataType* pNext;
   AsyncIOData* pStdoutAsyncIOData;
   AsyncIOData* pStderrAsyncIOData;
   DWORD nOpenFileCount;
   HANDLE hProcess;
   rb_pid_t pid;
   VALUE vStdinWrite;
   VALUE vStdoutRead;
   VALUE vStderrRead;
   IOHandlePair childStdinPair;
   IOHandlePair childStdoutPair;
   IOHandlePair childStderrPair;
} Open3ProcessData;

static const DWORD CHILD_PROCESS_EXIT_WAIT_MSECS = 500;    // 0.5 secs

static Open3ProcessData* win32_process_data_list = NULL;
static DWORD win32_named_pipe_serial_number = 1;

// Summary:
//  allocates a new Ruby I/O object.
//
// Returns:
//  partially initialized I/O object.
static VALUE allocate_ruby_io_object()
{
    VALUE klass = rb_cIO;
    NEWOBJ(io, struct RFile);
    OBJSETUP(io, klass, T_FILE);

    io->fptr = 0;

    return (VALUE)io;
}

// Summary:
//  parses the given mode string for Ruby mode flags.
//
// Returns:
//  integer representation of mode flags
static int parse_ruby_io_mode_flags(const char* szMode)
{
    int flags = 0;
    BOOL bValid = TRUE;

    switch (szMode[0])
    {
    case 'r':
         flags |= FMODE_READABLE;
         break;
    case 'w':
    case 'a':
         flags |= FMODE_WRITABLE;
         break;
    default:
        bValid = FALSE;
    }
    if (bValid)
    {
        if (szMode[1] == 'b')
        {
            flags |= FMODE_BINMODE;
            szMode++;
        }
        if (szMode[1] == '+')
        {
            if (szMode[2] == 0)
            {
                flags |= FMODE_READWRITE;
            }
            else
            {
                bValid = FALSE;
            }
        }
        else if (szMode[1] != 0)
        {
            bValid = FALSE;
        }
    }
    if (FALSE == bValid)
    {
        rb_raise(rb_eArgError, "illegal access mode %s", szMode);
    }

    return flags;
}

// Summary:
//  gets text for the given error code.
//
// Parameters:
//   dwErrorCode
//      win32 error code
//
// Returns:
//  formatted error string
static char* win32_error_description(DWORD dwErrorCode)
{
   static char ErrStr[MAX_ERROR_DESCRIPTION_LENGTH];
   HLOCAL hLocal = NULL;
   DWORD dwFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
   int length = FormatMessage(dwFlags,
                           NULL,
                           dwErrorCode,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           (char*)&hLocal,
                           0,
                           NULL);
    if (0 == length)
    {
        sprintf(ErrStr, "Unable to format message for Windows error #%d", (int)dwErrorCode);
    }
    else
    {
        strncpy(ErrStr, (LPTSTR)hLocal, length - 2);  // remove \r\n
        LocalFree(hLocal);
    }

    return ErrStr;
}

// Summary:
//  closes the Ruby I/O objects in the given array.
//
// Parameters:
//   vRubyIoObjectArray
//      array containing three I/O objects to be closed.
//
// Returns:
//  Qnil
static VALUE right_popen_close_io_array(VALUE vRubyIoObjectArray)
{
    const int iRubyIoObjectCount = 3;
    int i = 0;

    for (; i < iRubyIoObjectCount; ++i)
    {
        VALUE vRubyIoObject = RARRAY(vRubyIoObjectArray)->ptr[i];

        if (rb_funcall(vRubyIoObject, rb_intern("closed?"), 0) == Qfalse)
        {
            rb_funcall(vRubyIoObject, rb_intern("close"), 0);
        }
    }

    return Qnil;
}

// Summary:
//  creates a process using the popen pipe handles for standard I/O.
//
// Parameters:
//   szCommand
//      command to execute
//
//   hStdin
//      stdin pipe handle
//
//   hStdin
//      stdin read handle
//
//   hStdout
//      stdout write handle
//
//   hStderr
//      stderr write handle
//
//   phProcess
//      returned process handle
//
//   pPid
//      returned pid
//
//   bShowWindow
//      true if process window is initially visible, false if process has no UI or is invisible
//
// Returns:
//  true if successful, false otherwise (call GetLastError() for more information)
static BOOL win32_create_process(char*     szCommand,
                                 HANDLE    hStdin,
                                 HANDLE    hStdout,
                                 HANDLE    hStderr,
                                 HANDLE*   phProcess,
                                 rb_pid_t* pPid,
                                 BOOL      bShowWindow)
{
    PROCESS_INFORMATION pi;
    STARTUPINFO si;

    ZeroMemory(&si, sizeof(STARTUPINFO));

    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = hStdin;
    si.hStdOutput = hStdout;
    si.hStdError = hStderr;
    si.wShowWindow = bShowWindow ? SW_SHOW : SW_HIDE;
    if (CreateProcess(NULL,
                      szCommand,
                      NULL,
                      NULL,
                      TRUE,
                      0,
                      NULL,
                      NULL,
                      &si,
                      &pi))
    {
        // Close the handles now so anyone waiting is woken.
        CloseHandle(pi.hThread);

        // Return process handle
        *phProcess = pi.hProcess;
        *pPid = (rb_pid_t)pi.dwProcessId;
        return TRUE;
    }

    return FALSE;
}

// Summary:
//  closes the given pipe handle pair, if necessary.
//
// Parameters:
//  pair of handles to close.
static void win32_pipe_close(IOHandlePair* pPair)
{
   if (NULL != pPair->hRead)
   {
      CloseHandle(pPair->hRead);
      pPair->hRead = NULL;
   }
   if (NULL != pPair->hWrite)
   {
      CloseHandle(pPair->hWrite);
      pPair->hWrite = NULL;
   }
}

// Summary:
//  creates a new asynchronous I/O data structure.
//
// Returns:
//  initialized asynchronous I/O structure.
static AsyncIOData* win32_create_async_io_data()
{
    AsyncIOData* pAsyncIOData = (AsyncIOData*)malloc(sizeof(AsyncIOData));

    memset(pAsyncIOData, 0, sizeof(AsyncIOData));
    pAsyncIOData->overlapped.hEvent = CreateEvent(NULL, TRUE, TRUE, NULL);

    return pAsyncIOData;
}

// Summary:
//  frees an existing asynchronous I/O data structure after cancelling any
//  pending I/O operations.
//
// Parameters:
//   hRead
//      pipe handle opened for asynchronous reading
//
//   pAsyncIOData
//      data to free
static void win32_free_async_io_data(HANDLE hRead, AsyncIOData* pAsyncIOData)
{
    if (pAsyncIOData->bPending)
    {
        BOOL bResult = CancelIo(hRead);

        if (FALSE != bResult || ERROR_NOT_FOUND != GetLastError())
        {
            // Wait for the I/O subsystem to acknowledge our cancellation.
            // Depending on the timing of the calls, the I/O might complete
            // with a cancellation status, or it might complete normally (if
            // the ReadFile() was in the process of completing at the time
            // CancelIo() was called, or if the device does not support
            // cancellation). This call specifies TRUE for the bWait parameter,
            // which will block until the I/O either completes or is canceled,
            // thus resuming execution, provided the underlying device driver
            // and associated hardware are functioning properly. If there is a
            // problem with the driver it is better to stop responding, or
            // "hang," here than to try to continue while masking the problem.
            DWORD nBytesRead = 0;

            GetOverlappedResult(hRead, &pAsyncIOData->overlapped, &nBytesRead, TRUE);
        }
        pAsyncIOData->bPending = FALSE;
    }
    if (NULL != pAsyncIOData->overlapped.hEvent)
    {
        CloseHandle(pAsyncIOData->overlapped.hEvent);
        pAsyncIOData->overlapped.hEvent = NULL;
    }
    free(pAsyncIOData);
}

// Summary:
//  waits, closes and frees the given process data elements.
//
// Parameters:
//  data representing a monitored child process.
static void win32_free_process_data(Open3ProcessData* pData)
{
    if (NULL != pData->pStdoutAsyncIOData)
    {
        win32_free_async_io_data(pData->childStdoutPair.hRead, pData->pStdoutAsyncIOData);
        pData->pStdoutAsyncIOData = NULL;
    }
    if (NULL != pData->pStderrAsyncIOData)
    {
        win32_free_async_io_data(pData->childStderrPair.hRead, pData->pStderrAsyncIOData);
        pData->pStderrAsyncIOData = NULL;
    }
    win32_pipe_close(&pData->childStdinPair);
    win32_pipe_close(&pData->childStdoutPair);
    win32_pipe_close(&pData->childStderrPair);
    free(pData);
}

// Summary:
//  finalizer for the given Ruby file object.
static void win32_pipe_finalize(OpenFile *file, int noraise)
{
    if (file->f)
    {
        fclose(file->f);
        file->f = NULL;
    }

    if (file->f2)
    {
        fclose(file->f2);
        file->f2 = NULL;
    }

   // update exit status for child process, etc.
   {
      Open3ProcessData* pPrevious = NULL;
      Open3ProcessData* pData = win32_process_data_list;

      for (; NULL != pData; pData = pData->pNext)
      {
         if (pData->pid == file->pid)
         {
            break;
         }
         pPrevious = pData;
      }
      if (NULL != pData)
      {
         if (pData->nOpenFileCount <= 1)
         {
            // remove data from linked list.
            pData->nOpenFileCount = 0;
            if (NULL == pPrevious)
            {
               win32_process_data_list = pData->pNext;
            }
            else
            {
               pPrevious->pNext = pData->pNext;
            }
            
            // note that the caller has the option to wait out the child process
            // externally using the returned PID instead of relying on this code
            // to ensure it is finished and return the correct exit code.
    	    CloseHandle(pData->hProcess);
            pData->hProcess = NULL;

            // forget the pipe handles owned by the Ruby I/O objects to avoid
            // attempting to again close the already-closed handles.
            pData->childStdinPair.hWrite = NULL;
            pData->childStdoutPair.hRead = NULL;
            pData->childStderrPair.hRead = NULL;

            // free process data.
            win32_free_process_data(pData);
         }
         else
         {
            --pData->nOpenFileCount;
         }
      }
   }
}

// Summary:
//  creates an asynchronous pipe which allows for a single-threaded process
//  (e.g. Ruby v1.8) to read from multiple open pipes without deadlocking.
//  the write handle can be opened for any combination of synchronous/
//  asynchronous read/write. in the case of reading stdout and stderr from a
//  child process, open the pipe with synchronous write and asynchronous read
//  which means that only the caller and not the child process needs to be
//  aware that the pipe uses asynchronous read calls.
//
// Parameters:
//  pReadPipeHandle
//      receives created synchronous read pipe handle
//
//  pWritePipeHandle
//      receives opened asynchronous write pipe handle
//
//  pPipeAttributes
//      security attributes or NULL
//
//  nSize
//      suggested pipe buffer size or zero
//
//  dwReadMode
//      read mode, which can be FILE_FLAG_OVERLAPPED or zero
//
//  dwWriteMode
//      write mode, which can be FILE_FLAG_OVERLAPPED or zero
//
// Returns:
//  true if successful, false on failure (call GetLastError() for more info)
static BOOL win32_create_asynchronous_pipe(HANDLE* pReadPipeHandle,
                                           HANDLE* pWritePipeHandle,
                                           SECURITY_ATTRIBUTES* pPipeAttributes,
                                           DWORD nSize,
                                           DWORD dwReadMode,
                                           DWORD dwWriteMode)
{
    HANDLE hRead = NULL;
    HANDLE hWrite = NULL;
    char pipeNameBuffer[MAX_PATH];

    // reset.
    *pReadPipeHandle = NULL;
    *pWritePipeHandle = NULL;

    // only one valid mode flag - FILE_FLAG_OVERLAPPED
    if ((dwReadMode | dwWriteMode) & (~FILE_FLAG_OVERLAPPED))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    // default buffer size to 4 KB.
    {
        const DWORD nMinBufferSize = 1 << 12;

        nSize = max(nSize, nMinBufferSize);
    }

    // generate unique pipe name.
    sprintf(pipeNameBuffer,
            "\\\\.\\Pipe\\Ruby_Win32_Open3_gem.%d.%d",
            (int)GetCurrentProcessId(),
            (int)win32_named_pipe_serial_number++);

    // create read-end of pipe.
    hRead = CreateNamedPipeA(pipeNameBuffer,
                             PIPE_ACCESS_INBOUND | dwReadMode,
                             PIPE_TYPE_BYTE | PIPE_WAIT,
                             1,      // allowed named pipe instances
                             nSize,  // out buffer size
                             nSize,  // in buffer size
                             0,      // default timeout = 50 ms
                             pPipeAttributes);

    if (NULL == hRead || INVALID_HANDLE_VALUE == hRead)
    {
        return FALSE;
    }

    // open write-end of existing pipe.
    hWrite = CreateFileA(pipeNameBuffer,
                         GENERIC_WRITE,
                         0,  // No sharing
                         pPipeAttributes,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | dwWriteMode,
                         NULL);

    if (NULL == hWrite || INVALID_HANDLE_VALUE == hWrite)
    {
        DWORD dwErrorCode = GetLastError();
        CloseHandle(hRead);
        SetLastError(dwErrorCode);

        return FALSE;
    }
    *pReadPipeHandle = hRead;
    *pWritePipeHandle = hWrite;

    return TRUE;
}

// Summary:
//  creates a pipe and manages the requested inheritance for read/write. this
//  allows the inheritable handles to be passed to a created child process.
//
// Parameters:
//  bInheritRead
//      true if read handle (pair.hRead) will be inheritable.
//     
//  bInheritWrite
//      true if write handle (pair.hWrite) will be inheritable.
//
//  bAsynchronousOutput
//      true if read handle supports overlapped IO API calls, false if reads
//      are synchronous. the write handle is always synchronous so that the
//      child process can perform simple writes to stdout/stderr.
//
// Returns:
//  read,write handle pair
static IOHandlePair win32_pipe_create(BOOL bInheritRead, BOOL bInheritWrite, BOOL bAsynchronousOutput)
{
    // create pipe without inheritance, if requested.
    IOHandlePair pair;

    memset(&pair, 0, sizeof pair);
    if (0 == bInheritRead && 0 == bInheritWrite)
    {
        BOOL bResult = bAsynchronousOutput
                     ? win32_create_asynchronous_pipe(&pair.hRead, &pair.hWrite, NULL, 0, FILE_FLAG_OVERLAPPED, 0)
                     : CreatePipe(&pair.hRead, &pair.hWrite, NULL, 0);

        if (0 == bResult)
        {
            rb_raise(rb_eRuntimeError, "CreatePipe() failed: %s", win32_error_description(GetLastError()));
        }
    }
    else
    {
        HANDLE hCurrentProcess = GetCurrentProcess();
        SECURITY_ATTRIBUTES sa;

        // create pipe with inheritable flag set to TRUE.
        memset(&sa, 0, sizeof sa);
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        {
            BOOL bResult = bAsynchronousOutput
                         ? win32_create_asynchronous_pipe(&pair.hRead, &pair.hWrite, &sa, 0, FILE_FLAG_OVERLAPPED, 0)
                         : CreatePipe(&pair.hRead, &pair.hWrite, &sa, 0);

            if (0 == bResult)
            {
                rb_raise(rb_eRuntimeError, "CreatePipe() failed: %s", win32_error_description(GetLastError()));
            }
        }

        // duplicate the uninheritable handle (if any) by setting inheritance to FALSE.
        // otherwise, the child inherits the these handles which results in
        // non-closeable handles to the pipes being created.
        if (0 == bInheritRead)
        {
            HANDLE hDuplicate = NULL;
            BOOL bSuccess = DuplicateHandle(hCurrentProcess,
                                            pair.hRead,
                                            hCurrentProcess,
                                            &hDuplicate,
                                            0,
                                            FALSE,
                                            DUPLICATE_SAME_ACCESS);
            CloseHandle(pair.hRead);
            if (0 == bSuccess)
            {
                CloseHandle(pair.hWrite);
                rb_raise(rb_eRuntimeError, "DuplicateHandle() failed: %s", win32_error_description(GetLastError()));
            }
            pair.hRead = hDuplicate;
        }
        if (0 == bInheritWrite)
        {
            HANDLE hDuplicate = NULL;
            BOOL bSuccess = DuplicateHandle(hCurrentProcess,
                                            pair.hWrite,
                                            hCurrentProcess,
                                            &hDuplicate,
                                            0,
                                            FALSE,
                                            DUPLICATE_SAME_ACCESS);
            CloseHandle(pair.hWrite);
            if (0 == bSuccess)
            {
                CloseHandle(pair.hRead);
                rb_raise(rb_eRuntimeError, "DuplicateHandle() failed: %s", win32_error_description(GetLastError()));
            }
            pair.hWrite = hDuplicate;
        }
    }

    return pair;
}

// creates a Ruby I/O object from a file (pipe) handle opened for read or write.
//
// Parameters:
//     pid
//         child process id using the other end of pipe
//
//     mode
//         standard I/O file mode
//
//     hFile
//         pipe I/O connector to wrap with Ruby object
//
//     bReadMode
//         TRUE to create a readonly file object, FALSE to create a writeonly file object.
//
// Returns:
//     a Ruby I/O object
static VALUE ruby_create_io_object(rb_pid_t pid, int iFileMode, HANDLE hFile, BOOL bReadMode)
{
    BOOL bTextMode = 0 != (iFileMode & _O_TEXT);
    char* szMode = bReadMode
                 ? (bTextMode ? "r" : "rb")
                 : (bTextMode ? "w" : "wb");
    int fd = _open_osfhandle((long)hFile, iFileMode);
    FILE* pFile = _fdopen(fd, szMode);
    int iRubyModeFlags = parse_ruby_io_mode_flags(szMode);
    VALUE pRubyIOObject = allocate_ruby_io_object();
    OpenFile* pRubyOpenFile = NULL;

    MakeOpenFile(pRubyIOObject, pRubyOpenFile);
    pRubyOpenFile->finalize = win32_pipe_finalize;
    pRubyOpenFile->mode = iRubyModeFlags;
    pRubyOpenFile->pid = pid;

    if (iRubyModeFlags & FMODE_READABLE)
    {
        pRubyOpenFile->f = pFile;
    }
    if (iRubyModeFlags & FMODE_WRITABLE)
    {
        if (pRubyOpenFile->f)
        {
            pRubyOpenFile->f2 = pFile;
        }
        else
        {
            pRubyOpenFile->f = pFile;
        }
        pRubyOpenFile->mode |= FMODE_SYNC;
    }

    return pRubyIOObject;
}

// Summary:
//  creates a child process using the given command string and creates pipes for
//  use by the child's standard I/O methods. the pipes can be read either
//  synchronously or asynchronously, the latter being recommended for child
//  processes which potentially produce a large amount of output. reading
//  asynchronously also prevents a deadlock condition where the child is blocked
//  writing to a full pipe cache because the other pipe has not been flushed and
//  therefore cannot be read by the calling process, which is blocked reading.
//
// Parameters:
//   variable arguments, as follows:
//      szCommand
//          command to execute including any command-line arguments (required).
//
//      iMode
//          standard I/O file mode (e.g. _O_TEXT or _O_BINARY)
//
//      bShowWindow
//          false to hide child process, true to show
//
//      bAsynchronousOutput
//          false to read synchronously, true to read asynchronously. see
//          also RightPopen::async_read() (defaults to Qfalse).
//
// Returns:
//  a Ruby array containing [stdin write, stdout read, stderr read, pid]
//
// Throws:
//  raises a Ruby RuntimeError on failure
static VALUE win32_popen4(char* szCommand, int iMode, BOOL bShowWindow, BOOL bAsynchronousOutput)
{
    VALUE vReturnArray = Qnil;
    HANDLE hProcess = NULL;
    rb_pid_t pid = 0;
    Open3ProcessData* pData = (Open3ProcessData*)malloc(sizeof(Open3ProcessData));

    memset(pData, 0, sizeof(Open3ProcessData));
    pData->childStdinPair = win32_pipe_create(TRUE, FALSE, FALSE);
    pData->childStdoutPair = win32_pipe_create(FALSE, TRUE, bAsynchronousOutput);
    pData->childStderrPair = win32_pipe_create(FALSE, TRUE, bAsynchronousOutput);

    if (0 == win32_create_process(szCommand,
                                  pData->childStdinPair.hRead,
                                  pData->childStdoutPair.hWrite,
                                  pData->childStderrPair.hWrite,
                                  &hProcess,
                                  &pid,
                                  bShowWindow))
    {
        DWORD dwLastError = GetLastError();
        win32_free_process_data(pData);
        rb_raise(rb_eRuntimeError, "CreateProcess() failed: %s", win32_error_description(dwLastError));
    }

    // wrap piped I/O handles as ruby I/O objects in an array for return.
    pData->vStdinWrite = ruby_create_io_object(pid, iMode, pData->childStdinPair.hWrite, FALSE);
    pData->vStdoutRead = ruby_create_io_object(pid, iMode, pData->childStdoutPair.hRead, TRUE);
    pData->vStderrRead = ruby_create_io_object(pid, iMode, pData->childStderrPair.hRead, TRUE);
    pData->nOpenFileCount = 3;

    // allocate asynchronous I/O buffers, etc., if necessary.
    if (bAsynchronousOutput)
    {
        pData->pStdoutAsyncIOData = win32_create_async_io_data();
        pData->pStderrAsyncIOData = win32_create_async_io_data();
    }

    // create array for returning open3/open4 values.
    {
        const int iArraySize = 4;

        vReturnArray = rb_ary_new2(iArraySize);
        rb_ary_push(vReturnArray, pData->vStdinWrite);
        rb_ary_push(vReturnArray, pData->vStdoutRead);
        rb_ary_push(vReturnArray, pData->vStderrRead);
        rb_ary_push(vReturnArray, UINT2NUM((DWORD)pid));
    }

    // Child is launched. Close the parents copy of those pipe handles that only
    // the child should have open.  You need to make sure that no handles to the
    // write end of the output pipe are maintained in this process or else the
    // pipe will not close when the child process exits and the ReadFile() will
    // hang.
    CloseHandle(pData->childStdinPair.hRead);
    pData->childStdinPair.hRead = NULL;
    CloseHandle(pData->childStdoutPair.hWrite);
    pData->childStdoutPair.hWrite = NULL;
    CloseHandle(pData->childStderrPair.hWrite);
    pData->childStderrPair.hWrite = NULL;

    // insert data into static linked list.
    pData->pNext = win32_process_data_list;
    win32_process_data_list = pData;

    return vReturnArray;
}

// Summary:
//  creates a child process using the given command string and creates pipes for
//  use by the child's standard I/O methods. the pipes can be read either
//  synchronously or asynchronously, the latter being recommended for child
//  processes which potentially produce a large amount of output. reading
//  asynchronously also prevents a deadlock condition where the child is blocked
//  writing to a full pipe cache because the other pipe has not been flushed and
//  therefore cannot be read by the calling process, which is blocked reading.
//
// Parameters:
//   variable arguments, as follows:
//      vCommand
//          command to execute including any command-line arguments (required).
//
//      vMode
//          text ("t") or binary ("b") mode (defaults to "t").
//
//      vShowWindowFlag
//          Qfalse to hide child process, Qtrue to show (defaults to Qfalse)
//
//      vAsynchronousOutputFlag
//          Qfalse to read synchronously, Qtrue to read asynchronously. see
//          also RightPopen::async_read() (defaults to Qfalse).
//
// Returns:
//  a Ruby array containing [stdin write, stdout read, stderr read, pid]
//
// Throws:
//  raises a Ruby exception on failure
static VALUE right_popen_popen4(int argc, VALUE *argv, VALUE klass)
{
    VALUE vCommand = Qnil;
    VALUE vMode = Qnil;
    VALUE vReturnArray = Qnil;
    VALUE vShowWindowFlag = Qfalse;
    VALUE vAsynchronousOutputFlag = Qfalse;
    int iMode = 0;
    char* mode = "t";

    rb_scan_args(argc, argv, "13", &vCommand, &vMode, &vShowWindowFlag, &vAsynchronousOutputFlag);

    if (!NIL_P(vMode))
    {
        mode = StringValuePtr(vMode);
    }
    if (*mode == 't')
    {
        iMode = _O_TEXT;
    }
    else if (*mode != 'b')
    {
        rb_raise(rb_eArgError, "RightPopen::popen4() arg 2 must be 't' or 'b'");
    }
    else
    {
        iMode = _O_BINARY;
    }

    vReturnArray = win32_popen4(StringValuePtr(vCommand),
                                iMode,
                                Qfalse != vShowWindowFlag,
                                Qfalse != vAsynchronousOutputFlag);

    // ensure handles are closed in block form.
    if (rb_block_given_p())
    {
        return rb_ensure(rb_yield_splat, vReturnArray, right_popen_close_io_array, vReturnArray);
    }

    return vReturnArray;
}

// Summary:
//  reads asynchronously from a pipe opened for overlapped I/O.
//
// Parameters:
//   vSelf
//      should be Qnil since this is a module method.
//
//   vRubyIoObject
//      Ruby I/O object created by previous call to one of this module's popen
//      methods. I/O object should be opened for asynchronous reading or else
//      the behavior of this method is undefined.
//
// Returns:
//  Ruby string object representing a completed asynchronous read OR
//  the empty string to indicate the read is pending OR
//  Qnil to indicate data is not available and no further attempt to read should be made
static VALUE right_popen_async_read(VALUE vSelf, VALUE vRubyIoObject)
{
    if (NIL_P(vRubyIoObject))
    {
        rb_raise(rb_eRuntimeError, "RightPopen::async_read() parameter cannot be nil.");
    }

    // attempt to find corresponding asynchronous I/O data.
    {
        HANDLE hRead = NULL;
        AsyncIOData* pAsyncIOData = NULL;
        Open3ProcessData* pData = win32_process_data_list;

        for (; NULL != pData; pData = pData->pNext)
        {
            if (pData->vStdoutRead == vRubyIoObject)
            {
                hRead = pData->childStdoutPair.hRead;
                pAsyncIOData = pData->pStdoutAsyncIOData;
                break;
            }
            if (pData->vStderrRead == vRubyIoObject)
            {
                hRead = pData->childStderrPair.hRead;
                pAsyncIOData = pData->pStderrAsyncIOData;
                break;
            }
        }
        if (NULL == hRead)
        {
            rb_raise(rb_eRuntimeError, "RightPopen::async_read() parameter refers to an I/O object which was not created by this class.");
        }

        // perform asynchronous read.
        if (WAIT_OBJECT_0 == WaitForSingleObject(pAsyncIOData->overlapped.hEvent, 0))
        {
            // attempt to complete last read without waiting if pending.
            if (pAsyncIOData->bPending)
            {
                if (0 == GetOverlappedResult(hRead, &pAsyncIOData->overlapped, &pAsyncIOData->nBytesRead, FALSE)) 
                {
                    DWORD dwErrorCode = GetLastError();

                    switch (dwErrorCode)
                    {
                    case ERROR_IO_INCOMPLETE:
                        break;
                    default:
                        // doesn't matter why read failed; read is no longer
                        // pending and was probably cancelled.
                        pAsyncIOData->bPending = FALSE;
                        return Qnil;
                    }
                }
                else
                {
                    // delayed read completed, set guard byte to nul.
                    pAsyncIOData->bPending = FALSE;
                    pAsyncIOData->buffer[pAsyncIOData->nBytesRead] = 0;
                }
            }
            else if (0 == ReadFile(hRead,
                                   pAsyncIOData->buffer,
                                   sizeof(pAsyncIOData->buffer) - 1,
                                   &pAsyncIOData->nBytesRead,
                                   &pAsyncIOData->overlapped))
            {
                DWORD dwErrorCode = GetLastError();

                switch (dwErrorCode)
                {
                case ERROR_IO_PENDING: 
                    pAsyncIOData->bPending = TRUE;
                    break;
                default:
                    // doesn't matter why read failed; data is no longer available.
                    return Qnil;
                }
            }
            else
            {
                // read completed immediately, set guard byte to nul.
                pAsyncIOData->buffer[pAsyncIOData->nBytesRead] = 0;
            }
        }

        // the overlapped I/O appears to pass \r\n literally from the child
        // process' output stream whereas the synchronous stdio alternative
        // replaces \r\n with \n. for the sake of homogeneosity of text data
        // and the fact that Ruby code rarely uses the \r\n idiom, quickly
        // remove all \r characters from the text.
        if (pAsyncIOData->nBytesRead > 0)
        {
            char* pszInsert = pAsyncIOData->buffer;
            char* pszParse = pAsyncIOData->buffer;
            char* pszStop = pAsyncIOData->buffer + pAsyncIOData->nBytesRead;

            while (pszParse < pszStop)
            {
                char chNext = *pszParse++;
                if ('\r' != chNext)
                {
                    *pszInsert++ = chNext;
                }
            }
            pAsyncIOData->nBytesRead = (DWORD)(pszInsert - pAsyncIOData->buffer);
        }

        // create string for return value, if necessary. the empty string signals
        // that the caller should keep trying (i.e. pending).
        return rb_str_new(pAsyncIOData->buffer, (long)pAsyncIOData->nBytesRead);
    }
}

// Summary:
//  'RightPopen' module entry point
void Init_right_popen()
{
    VALUE vModule = rb_define_module("RightPopen");

    rb_define_module_function(vModule, "popen4", (VALUE(*)(ANYARGS))right_popen_popen4, -1);
    rb_define_module_function(vModule, "async_read", (VALUE(*)(ANYARGS))right_popen_async_read, 1);
}
