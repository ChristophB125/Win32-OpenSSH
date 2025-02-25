/*
* Author: Manoj Ampalam <manoj.ampalam@microsoft.com>
*
* Copyright (c) 2015 Microsoft Corp.
* All rights reserved
*
* Microsoft openssh win32 port
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
* NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
* THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <io.h>
#include <errno.h>
#include <stddef.h>

#include "w32fd.h"
#include "inc\utf.h"
#include "inc\fcntl.h"
#include "misc_internal.h"
#include "debug.h"

/* internal read buffer size */
#define READ_BUFFER_SIZE 100*1024
/* internal write buffer size */
#define WRITE_BUFFER_SIZE 100*1024
#define errno_from_Win32LastError() errno_from_Win32Error(GetLastError())

struct createFile_flags {
	DWORD dwDesiredAccess;
	DWORD dwShareMode;
	SECURITY_ATTRIBUTES securityAttributes;
	DWORD dwCreationDisposition;
	DWORD dwFlagsAndAttributes;
};

int termio_initiate_read(struct w32_io* pio);
int termio_initiate_write(struct w32_io* pio, DWORD num_bytes);

/* maps Win32 error to errno */
int
errno_from_Win32Error(int win32_error)
{
	switch (win32_error) {
	case ERROR_ACCESS_DENIED:
		return EACCES;
	case ERROR_OUTOFMEMORY:
		return ENOMEM;
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_FILE_NOT_FOUND:
		return ENOENT;
	default:
		return win32_error;
	}
}

struct w32_io*
fileio_afunix_socket() 
{
	struct w32_io* ret = (struct w32_io*)malloc(sizeof(struct w32_io));
	if (ret == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	memset(ret, 0, sizeof(struct w32_io));
	return ret;
}

int
fileio_connect(struct w32_io* pio, char* name) 
{
	wchar_t* name_w = NULL;
	wchar_t pipe_name[PATH_MAX];
	HANDLE h = INVALID_HANDLE_VALUE;
	int ret = 0;

	if (pio->handle != 0 && pio->handle != INVALID_HANDLE_VALUE) {
		debug3("fileio_connect called in unexpected state, pio = %p", pio);
		errno = EOTHER;
		ret = -1;
		goto cleanup;
	}

	if ((name_w = utf8_to_utf16(name)) == NULL) {
		errno = ENOMEM;
		return -1;
	}
	_snwprintf(pipe_name, PATH_MAX, L"\\\\.\\pipe\\%ls", name_w);
	h = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, 
		NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	
	/* TODO - support nonblocking connect */
	/* wait until we have a server pipe instance to connect */
	while (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) {
		debug4("waiting for agent connection, retrying after 1 sec");
		if ((ret = wait_for_any_event(NULL, 0, 1000) != 0) != 0)
			goto cleanup;
	}

	if (h == INVALID_HANDLE_VALUE) {
		debug3("unable to connect to pipe %ls, error: %d", name_w, GetLastError());
		errno = errno_from_Win32LastError();
		ret = -1;
		goto cleanup;
	}

	if (SetHandleInformation(h, HANDLE_FLAG_INHERIT,
	    pio->fd_flags & FD_CLOEXEC ? 0 : HANDLE_FLAG_INHERIT) == FALSE) {
		errno = errno_from_Win32LastError();
		debug3("SetHandleInformation failed, error = %d, pio = %p", GetLastError(), pio);
		ret = -1;
		goto cleanup;
	}
	
	pio->handle = h;
	h = NULL;

cleanup:
	if (name_w)
		free(name_w);
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
	return ret;
}

/* used to name named pipes used to implement pipe() */
static int pipe_counter = 0;

/*
 * pipe() implementation. Creates an inbound named pipe, uses CreateFile to connect
 * to it. These handles are associated with read end and write end of the pipe
 */
int
fileio_pipe(struct w32_io* pio[2])
{
	HANDLE read_handle = INVALID_HANDLE_VALUE, write_handle = INVALID_HANDLE_VALUE;
	struct w32_io *pio_read = NULL, *pio_write = NULL;
	char pipe_name[PATH_MAX];
	SECURITY_ATTRIBUTES sec_attributes;

	if (pio == NULL) {
		errno = EINVAL;
		debug3("pipe - ERROR invalid parameter");
		return -1;
	}

	/* create name for named pipe */
	if (-1 == sprintf_s(pipe_name, PATH_MAX, "\\\\.\\Pipe\\W32PosixPipe.%08x.%08x",
		GetCurrentProcessId(), pipe_counter++)) {
		errno = EOTHER;
		debug3("pipe - ERROR sprintf_s %d", errno);
		goto error;
	}

	sec_attributes.bInheritHandle = TRUE;
	sec_attributes.lpSecurityDescriptor = NULL;
	sec_attributes.nLength = 0;

	/* create named pipe */
	write_handle = CreateNamedPipeA(pipe_name,
		PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_WAIT,
		1,
		4096,
		4096,
		0,
		&sec_attributes);
	if (write_handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug3("pipe - CreateNamedPipe() ERROR:%d", errno);
		goto error;
	}

	/* connect to named pipe */
	read_handle = CreateFileA(pipe_name,
		GENERIC_READ,
		0,
		&sec_attributes,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);
	if (read_handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug3("pipe - ERROR CreateFile() :%d", errno);
		goto error;
	}

	/* create w32_io objects encapsulating above handles */
	pio_read = (struct w32_io*)malloc(sizeof(struct w32_io));
	pio_write = (struct w32_io*)malloc(sizeof(struct w32_io));

	if (!pio_read || !pio_write) {
		errno = ENOMEM;
		debug3("pip - ERROR:%d", errno);
		goto error;
	}

	memset(pio_read, 0, sizeof(struct w32_io));
	memset(pio_write, 0, sizeof(struct w32_io));

	pio_read->handle = read_handle;
	pio_write->handle = write_handle;

	pio[0] = pio_read;
	pio[1] = pio_write;
	return 0;

error:
	if (read_handle)
		CloseHandle(read_handle);
	if (write_handle)
		CloseHandle(write_handle);
	if (pio_read)
		free(pio_read);
	if (pio_write)
		free(pio_write);
	return -1;
}

/* maps open() file modes and flags to ones needed by CreateFile */
static int
createFile_flags_setup(int flags, int mode, struct createFile_flags* cf_flags)
{
	/* check flags */
	int rwflags = flags & 0x3;
	int c_s_flags = flags & 0xfffffff0;

	/*
	* should be one of one of the following access modes:
	* O_RDONLY, O_WRONLY, or O_RDWR
	*/
	if ((rwflags != O_RDONLY) && (rwflags != O_WRONLY) && (rwflags != O_RDWR)) {
		debug3("open - flags ERROR: wrong rw flags: %d", flags);
		errno = EINVAL;
		return -1;
	}

	/*only following create and status flags currently supported*/
	if (c_s_flags & ~(O_NONBLOCK | O_APPEND | O_CREAT | O_TRUNC | O_EXCL | O_BINARY)) {
		debug3("open - ERROR: Unsupported flags: %d", flags);
		errno = ENOTSUP;
		return -1;
	}

	/*validate mode*/
	if (mode &~(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) {
		debug3("open - ERROR: unsupported mode: %d", mode);
		errno = ENOTSUP;
		return -1;
	}

	cf_flags->dwShareMode = 0;

	switch (rwflags) {
	case O_RDONLY:
		cf_flags->dwDesiredAccess = GENERIC_READ;
		cf_flags->dwShareMode = FILE_SHARE_READ;
		break;
	case O_WRONLY:
		cf_flags->dwDesiredAccess = GENERIC_WRITE;
		break;
	case O_RDWR:
		cf_flags->dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
		break;
	}

	cf_flags->securityAttributes.lpSecurityDescriptor = NULL;
	cf_flags->securityAttributes.bInheritHandle = TRUE;
	cf_flags->securityAttributes.nLength = 0;

	cf_flags->dwCreationDisposition = OPEN_EXISTING;
	if (c_s_flags & O_TRUNC)
		cf_flags->dwCreationDisposition = TRUNCATE_EXISTING;
	if (c_s_flags & O_CREAT) {
		if (c_s_flags & O_EXCL)
			cf_flags->dwCreationDisposition = CREATE_NEW;
		else
			cf_flags->dwCreationDisposition = CREATE_ALWAYS;
	}

	if (c_s_flags & O_APPEND)
		cf_flags->dwDesiredAccess = FILE_APPEND_DATA;

	cf_flags->dwFlagsAndAttributes = FILE_FLAG_OVERLAPPED | SECURITY_IMPERSONATION | FILE_FLAG_BACKUP_SEMANTICS;

	/*TODO - map mode */

	return 0;
}


#define NULL_DEVICE "/dev/null"
/* open() implementation. Uses CreateFile to open file, console, device, etc */
struct w32_io*
fileio_open(const char *path_utf8, int flags, int mode)
{
	struct w32_io* pio = NULL;
	struct createFile_flags cf_flags;
	HANDLE handle;
	wchar_t *path_utf16 = NULL;

	debug4("open - pathname:%s, flags:%d, mode:%d", path_utf8, flags, mode);
	/* check input params*/
	if (path_utf8 == NULL) {
		errno = EINVAL;
		debug3("open - ERROR:%d", errno);
		return NULL;
	}

	/* if opening null device, point to Windows equivalent */
	if (strncmp(path_utf8, NULL_DEVICE, strlen(NULL_DEVICE)) == 0) 
		path_utf8 = "NUL";

	if ((path_utf16 = utf8_to_utf16(path_utf8)) == NULL) {
		errno = ENOMEM;
		debug3("utf8_to_utf16 failed for file:%s error:%d", path_utf8, GetLastError());
		return NULL;
	}

	if (createFile_flags_setup(flags, mode, &cf_flags) == -1)
		return NULL;

	handle = CreateFileW(path_utf16, cf_flags.dwDesiredAccess, cf_flags.dwShareMode,
		&cf_flags.securityAttributes, cf_flags.dwCreationDisposition,
		cf_flags.dwFlagsAndAttributes, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		errno = errno_from_Win32LastError();
		debug3("failed to open file:%s error:%d", path_utf8, GetLastError());
		free(path_utf16);
		return NULL;
	}

	free(path_utf16);
	pio = (struct w32_io*)malloc(sizeof(struct w32_io));
	if (pio == NULL) {
		CloseHandle(handle);
		errno = ENOMEM;
		debug3("fileio_open(), failed to allocate memory error:%d", errno);
		return NULL;
	}

	memset(pio, 0, sizeof(struct w32_io));

	if (flags & O_NONBLOCK)
		pio->fd_status_flags = O_NONBLOCK;

	pio->handle = handle;
	return pio;
}

VOID CALLBACK 
ReadCompletionRoutine(_In_ DWORD dwErrorCode, _In_ DWORD dwNumberOfBytesTransfered, _Inout_ LPOVERLAPPED lpOverlapped)
{
	struct w32_io* pio = (struct w32_io*)((char*)lpOverlapped - offsetof(struct w32_io, read_overlapped));
	debug4("ReadCB pio:%p, pending_state:%d, error:%d, received:%d",
		pio, pio->read_details.pending, dwErrorCode, dwNumberOfBytesTransfered);
	pio->read_details.error = dwErrorCode;
	pio->read_details.remaining = dwNumberOfBytesTransfered;
	pio->read_details.completed = 0;
	pio->read_details.pending = FALSE;
	*((__int64*)&lpOverlapped->Offset) += dwNumberOfBytesTransfered;
}

/* initiate an async read */
/* TODO:  make this a void func, store error in context */
int
fileio_ReadFileEx(struct w32_io* pio, unsigned int bytes_requested)
{
	debug4("ReadFileEx io:%p", pio);

	if (pio->read_details.buf == NULL) {
		pio->read_details.buf = malloc(READ_BUFFER_SIZE);
		if (!pio->read_details.buf) {
			errno = ENOMEM;
			debug4("ReadFileEx - ERROR: %d, io:%p", errno, pio);
			return -1;
		}
	}

	if (FILETYPE(pio) == FILE_TYPE_DISK)
		pio->read_details.buf_size = min(bytes_requested, READ_BUFFER_SIZE);
	else
		pio->read_details.buf_size = READ_BUFFER_SIZE;

	if (ReadFileEx(WINHANDLE(pio), pio->read_details.buf, pio->read_details.buf_size,
		&pio->read_overlapped, &ReadCompletionRoutine))
		pio->read_details.pending = TRUE;
	else {
		errno = errno_from_Win32LastError();
		debug3("ReadFileEx() ERROR:%d, io:%p", GetLastError(), pio);
		return -1;
	}

	return 0;
}

/* read() implementation */
int
fileio_read(struct w32_io* pio, void *dst, unsigned int max)
{
	int bytes_copied;

	debug5("read - io:%p remaining:%d", pio, pio->read_details.remaining);
	/* if read is pending */
	if (pio->read_details.pending) {
		if (w32_io_is_blocking(pio)) {
			debug4("read - io is pending, blocking call made, io:%p", pio);
			while (fileio_is_io_available(pio, TRUE) == FALSE) {
				if (-1 == wait_for_any_event(NULL, 0, INFINITE))
					return -1;
			}
		}
		errno = EAGAIN;
		debug4("read - io is already pending, io:%p", pio);
		return -1;
	}

	if (fileio_is_io_available(pio, TRUE) == FALSE) {
		if (FILETYPE(pio) == FILE_TYPE_CHAR) {
			if (-1 == termio_initiate_read(pio))
				return -1;
		}
		else {
			if (-1 == fileio_ReadFileEx(pio, max)) {
				if ((FILETYPE(pio) == FILE_TYPE_PIPE)
					&& (errno == ERROR_BROKEN_PIPE)) {
					/* write end of the pipe closed */
					debug3("read - no more data, io:%p", pio);
					errno = 0;
					return 0;
				}
				/* on W2012, ReadFileEx on file throws a synchronous EOF error*/
				else if ((FILETYPE(pio) == FILE_TYPE_DISK)
					&& (errno == ERROR_HANDLE_EOF)) {
					debug3("read - no more data, io:%p", pio);
					errno = 0;
					return 0;
				}
				return -1;
			}
		}

		/* pick up APC if IO has completed */
		SleepEx(0, TRUE);

		if (w32_io_is_blocking(pio)) {
			while (fileio_is_io_available(pio, TRUE) == FALSE) {
				if (-1 == wait_for_any_event(NULL, 0, INFINITE))
					return -1;
			}
		}
		else if (pio->read_details.pending) {
			errno = EAGAIN;
			debug4("read - IO is pending, io:%p", pio);
			return -1;
		}
	}

	if (pio->read_details.error) {
		errno = errno_from_Win32Error(pio->read_details.error);
		/*write end of the pipe is closed or pipe broken or eof reached*/
		if ((pio->read_details.error == ERROR_BROKEN_PIPE) ||
			(pio->read_details.error == ERROR_HANDLE_EOF)) {
			debug4("read - (2) no more data, io:%p", pio);
			errno = 0;
			pio->read_details.error = 0;
			return 0;
		}
		debug3("read - ERROR from cb :%d, io:%p", errno, pio);
		pio->read_details.error = 0;
		return -1;
	}

	bytes_copied = min(max, pio->read_details.remaining);
	memcpy(dst, pio->read_details.buf + pio->read_details.completed, bytes_copied);
	pio->read_details.remaining -= bytes_copied;
	pio->read_details.completed += bytes_copied;
	debug4("read - io:%p read: %d remaining: %d", pio, bytes_copied,
		pio->read_details.remaining);
	return bytes_copied;
}

VOID CALLBACK 
WriteCompletionRoutine(_In_ DWORD dwErrorCode,
			_In_ DWORD dwNumberOfBytesTransfered,
			_Inout_ LPOVERLAPPED lpOverlapped)
{
	struct w32_io* pio =
		(struct w32_io*)((char*)lpOverlapped - offsetof(struct w32_io, write_overlapped));
	debug4("WriteCB - pio:%p, pending_state:%d, error:%d, transferred:%d of remaining: %d",
		pio, pio->write_details.pending, dwErrorCode, dwNumberOfBytesTransfered,
		pio->write_details.remaining);
	pio->write_details.error = dwErrorCode;
	/* TODO - assert that remaining == dwNumberOfBytesTransfered */
	if ((dwErrorCode == 0) && (pio->write_details.remaining != dwNumberOfBytesTransfered)) {
		debug3("WriteCB - ERROR: broken assumption, io:%p, wrote:%d, remaining:%d", pio,
			dwNumberOfBytesTransfered, pio->write_details.remaining);
		DebugBreak();
	}
	pio->write_details.remaining -= dwNumberOfBytesTransfered;
	pio->write_details.pending = FALSE;
	*((__int64*)&lpOverlapped->Offset) += dwNumberOfBytesTransfered;
}

/* write() implementation */
int
fileio_write(struct w32_io* pio, const void *buf, unsigned int max)
{
	int bytes_copied;
	DWORD pipe_flags = 0, pipe_instances = 0;

	debug4("write - io:%p", pio);
	if (pio->write_details.pending) {
		if (w32_io_is_blocking(pio)) {
			debug4("write - io pending, blocking call made, io:%p", pio);
			while (pio->write_details.pending)
				if (wait_for_any_event(NULL, 0, INFINITE) == -1)
					return -1;
		} else {
			errno = EAGAIN;
			debug4("write - IO is already pending, io:%p", pio);
			return -1;
		}
	}

	if (pio->write_details.error) {
		errno = errno_from_Win32Error(pio->write_details.error);
		debug3("write - ERROR:%d on prior unblocking write, io:%p", errno, pio);
		pio->write_details.error = 0;
		if ((FILETYPE(pio) == FILE_TYPE_PIPE) && (errno == ERROR_BROKEN_PIPE)) {
			debug3("write - ERROR:read end of the pipe closed, io:%p", pio);
			errno = EPIPE;
		}
		return -1;
	}

	if (pio->write_details.buf == NULL) {
		pio->write_details.buf = malloc(WRITE_BUFFER_SIZE);
		if (pio->write_details.buf == NULL) {
			errno = ENOMEM;
			debug3("write - ERROR:%d, io:%p", errno, pio);
			return -1;
		}
		pio->write_details.buf_size = WRITE_BUFFER_SIZE;
	}

	bytes_copied = min(max, pio->write_details.buf_size);
	memcpy(pio->write_details.buf, buf, bytes_copied);

	if (FILETYPE(pio) == FILE_TYPE_CHAR) {
		if (termio_initiate_write(pio, bytes_copied) == 0) {
			pio->write_details.pending = TRUE;
			pio->write_details.remaining = bytes_copied;
		}
		else
			return -1;
	} else if ( FILETYPE(pio) == FILE_TYPE_PIPE &&
	    GetNamedPipeInfo(WINHANDLE(pio), &pipe_flags, NULL, NULL, &pipe_instances) &&
	    pipe_flags == PIPE_CLIENT_END && pipe_instances == 1) {
		/* 
		 * TODO - Figure out a better solution to this problem 
		 * IO handle corresponding to this object (pio->handle) may be referring
		 * to something that isn't opened in overlapped mode. While all handles
		 * opened by this POSIX wrapper are opened in overlapped mode, other handles
		 * that are inherited (ex. via std i/o) are typically not. 
		 * Ex. When we do this in Powershell
		 * $o = ssh.exe user@target hostname
		 * Powershell creates anonymous pipes (that do not support overlapped i.o)
		 * Calling asynchronous I/O APIs (WriteFileEx) for example will not work in 
		 * those cases (the callback is never called and it typically manifests as a 
		 * hang to end user
		 *
		 * This conditional logic is put in place to specifically handle Powershell 
		 * redirection scenarios. Thinking behind these conditions
		 * - should be a pipe handle. console I/O is handled in termio.c, impacting file i/o
		 *   scenarios not found yet.
		 * - pipe should be the client end. This is to skip pipes created internally in POSIX
		 *   wrapper (by pipe() calls) - The write ends on these pipes are on server
		 * - pipe_instances == 1. This is to skip pipe handles created as part of Connect(AF_UNIX)
		 *   sockets (that typically are created for unlimited instances). 
		 * For such I/O we do a synchronous write. 
		 */
		/* DebugBreak() */;
		if (WriteFile(WINHANDLE(pio), pio->write_details.buf, bytes_copied, &bytes_copied, NULL) == FALSE) {
			errno = errno_from_Win32LastError();
			debug3("write - WriteFile() ERROR:%d, io:%p", GetLastError(), pio);
			return -1;
		}
		return bytes_copied;
	} else {
		if (WriteFileEx(WINHANDLE(pio), pio->write_details.buf, bytes_copied,
			&pio->write_overlapped, &WriteCompletionRoutine)) {
			pio->write_details.pending = TRUE;
			pio->write_details.remaining = bytes_copied;
		} else {
			errno = errno_from_Win32LastError();
			/* read end of the pipe closed ?   */
			if ((FILETYPE(pio) == FILE_TYPE_PIPE) && (errno == ERROR_BROKEN_PIPE)) {
				debug3("write - ERROR:read end of the pipe closed, io:%p", pio);
				errno = EPIPE;
			}
			debug3("write ERROR from cb(2):%d, io:%p", errno, pio);
			return -1;
		}
	}

	if (w32_io_is_blocking(pio)) {
		while (pio->write_details.pending) {
			if (wait_for_any_event(NULL, 0, INFINITE) == -1) {
				/* if interrupted but write has completed, we are good*/
				if ((errno != EINTR) || (pio->write_details.pending))
					return -1;
				errno = 0;
			}
		}
	}

	/* execute APC to give a chance for write to complete */
	SleepEx(0, TRUE);

	/* if write has completed, pick up any error reported*/
	if (!pio->write_details.pending && pio->write_details.error) {
		errno = errno_from_Win32Error(pio->write_details.error);
		debug3("write - ERROR from cb:%d, io:%p", pio->write_details.error, pio);
		pio->write_details.error = 0;
		return -1;
	}
	debug4("write - reporting %d bytes written, io:%p", bytes_copied, pio);
	return bytes_copied;

}

/* fstat() implemetation */
int
fileio_fstat(struct w32_io* pio, struct _stat64 *buf)
{
	int fd = _open_osfhandle((intptr_t)pio->handle, 0);
	debug4("fstat - pio:%p", pio);
	if (fd == -1) {
		errno = EOTHER;
		return -1;
	}

	return _fstat64(fd, buf);
}

int
fileio_stat(const char *path, struct _stat64 *buf)
{
	wchar_t* wpath = NULL;
	int r = -1;

	if ((wpath = utf8_to_utf16(path)) == NULL)
		fatal("failed to covert input arguments");

	r = _wstat64(wpath, buf);

	/*
	* If we doesn't have sufficient permissions then _wstat64() is returning "file not found"
	* TODO - Replace the above call with GetFileAttributesEx 
	*/

cleanup:
	if (wpath)
		free(wpath);
	return r;
}

long
fileio_lseek(struct w32_io* pio, long offset, int origin)
{
	debug4("lseek - pio:%p", pio);
	if (origin != SEEK_SET) {
		debug3("lseek - ERROR, origin is not supported %d", origin);
		errno = ENOTSUP;
		return -1;
	}

	pio->read_overlapped.Offset = offset;
	pio->write_overlapped.Offset = offset;
	return 0;
}

/* fdopen implementation */
FILE*
fileio_fdopen(struct w32_io* pio, const char *mode)
{
	int fd_flags = 0;
	debug4("fdopen - io:%p", pio);

	/* logic below doesn't work with overlapped file HANDLES */
	if (mode[1] == '\0') {
		switch (*mode) {
		case 'r':
			fd_flags = _O_RDONLY;
			break;
		case 'w':
			break;
		case 'a':
			fd_flags = _O_APPEND;
			break;
		default:
			errno = ENOTSUP;
			debug3("fdopen - ERROR unsupported mode %s", mode);
			return NULL;
		}
	} else {
		errno = ENOTSUP;
		debug3("fdopen - ERROR unsupported mode %s", mode);
		return NULL;
	}

	int fd = _open_osfhandle((intptr_t)pio->handle, fd_flags);

	if (fd == -1) {
		errno = EOTHER;
		debug3("fdopen - ERROR:%d _open_osfhandle()", errno);
		return NULL;
	}

	return _fdopen(fd, mode);
}

void
fileio_on_select(struct w32_io* pio, BOOL rd)
{
	if (!rd)
		return;

	if (!pio->read_details.pending && !fileio_is_io_available(pio, rd))
		/* initiate read, record any error so read() will pick up */
		if (FILETYPE(pio) == FILE_TYPE_CHAR) {
			if (termio_initiate_read(pio) != 0) {
				pio->read_details.error = errno;
				errno = 0;
				return;
			}
		} else {
			if (fileio_ReadFileEx(pio, INT_MAX) != 0) {
				pio->read_details.error = errno;
				errno = 0;
				return;
			}
		}
}

int
fileio_close(struct w32_io* pio)
{
	debug4("fileclose - pio:%p", pio);

	/* handle can be null on AF_UNIX sockets that are not yet connected */
	if (WINHANDLE(pio) == 0 || WINHANDLE(pio) == INVALID_HANDLE_VALUE) {
		free(pio);
		return 0;
	}

	CancelIo(WINHANDLE(pio));
	/* let queued APCs (if any) drain */
	SleepEx(0, TRUE);
	if (pio->type != STD_IO_FD) { /* STD handles are never explicitly closed */
		CloseHandle(WINHANDLE(pio));

		if (pio->read_details.buf)
			free(pio->read_details.buf);

		if (pio->write_details.buf)
			free(pio->write_details.buf);

		free(pio);
	}
	return 0;
}

BOOL
fileio_is_io_available(struct w32_io* pio, BOOL rd)
{
	if (rd) {
		if (pio->read_details.remaining || pio->read_details.error)
			return TRUE;
		else
			return FALSE;
	} else { /* write */
		return (pio->write_details.pending == FALSE) ? TRUE : FALSE;
	}
}