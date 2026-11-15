/*
 * kernel_file.c - Xbox File I/O
 *
 * Implements the Nt*File kernel functions. All Xbox device paths are
 * translated through kernel_path.c before use.
 *
 * The Xbox kernel uses NT-style file I/O with ANSI strings in
 * OBJECT_ATTRIBUTES (unlike Windows NT, which uses Unicode).
 *
 * Two backends:
 *   _WIN32  -> Win32 CreateFileW / ReadFile / FindFirstFileW ...
 *   POSIX   -> open / read / write / stat / opendir ...
 * The Xbox semantics (disposition mapping, IO_STATUS_BLOCK, info classes)
 * are identical on both; only the host syscalls differ.
 */

#define _GNU_SOURCE   /* FNM_CASEFOLD */
#include "kernel.h"
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fnmatch.h>
#endif

/* Get the ANSI path from OBJECT_ATTRIBUTES (platform-independent). */
static const char* get_xbox_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    if (!ObjectAttributes || !ObjectAttributes->ObjectName ||
        !ObjectAttributes->ObjectName->Buffer)
        return NULL;
    return ObjectAttributes->ObjectName->Buffer;
}

/* Xbox volume geometry.
 *
 * FATX uses 16 KB clusters: 512-byte sectors, 32 sectors per cluster. That is
 * not cosmetic. A title's CRT startup asks for FileFsSizeInformation and
 * multiplies SectorsPerAllocationUnit by BytesPerSector, then *requires* the
 * product to equal the cluster size it was built for. Half-Life 2 checks for
 * 0x4000 and returns STATUS_DEVICE_NOT_READY (0xC000014F) otherwise, which
 * aborts CRT init before main ever runs -- the process then exits cleanly,
 * which reads as a title that did nothing rather than one that failed.
 *
 * Reporting the host's PC-typical 4 KB cluster (512 x 8) fails that check. */
#define XBOX_BYTES_PER_SECTOR       512u
#define XBOX_SECTORS_PER_CLUSTER    32u      /* 512 * 32 = 16384 */

/* ======================================================================== */
/* Shared by both backends. kernel_bridge.c calls xbox_LastFileError()
 * unconditionally, so defining it only under _WIN32 leaves every POSIX link
 * with an undefined symbol -- invisible until something links an executable,
 * because a static archive never resolves its own references.
 *
 * The POSIX backend does not populate this yet, so it reads 0 there; that is a
 * missing detail rather than a missing symbol, and it is visible here. */
uint32_t g_xbox_last_file_error;

uint32_t xbox_LastFileError(void)
{
    return g_xbox_last_file_error;
}

#if defined(_WIN32)
/* ====================  Win32 backend  =================================== */
/* ======================================================================== */

/* Convert Xbox create disposition to Win32 */
static DWORD xbox_disposition_to_win32(ULONG Disposition)
{
    switch (Disposition) {
        case XBOX_FILE_SUPERSEDE:    return CREATE_ALWAYS;
        case XBOX_FILE_OPEN:         return OPEN_EXISTING;
        case XBOX_FILE_CREATE:       return CREATE_NEW;
        case XBOX_FILE_OPEN_IF:      return OPEN_ALWAYS;
        case XBOX_FILE_OVERWRITE:    return TRUNCATE_EXISTING;
        case XBOX_FILE_OVERWRITE_IF: return CREATE_ALWAYS;
        default:                     return OPEN_EXISTING;
    }
}

/* Convert Xbox access mask to Win32 */
static DWORD xbox_access_to_win32(ACCESS_MASK Access)
{
    DWORD result = 0;
    if (Access & XBOX_GENERIC_READ)           result |= GENERIC_READ;
    if (Access & XBOX_GENERIC_WRITE)          result |= GENERIC_WRITE;
    if (Access & XBOX_GENERIC_ALL)            result |= GENERIC_ALL;
    if (Access & XBOX_FILE_READ_DATA)         result |= FILE_READ_DATA;
    if (Access & XBOX_FILE_WRITE_DATA)        result |= FILE_WRITE_DATA;
    if (Access & XBOX_FILE_APPEND_DATA)       result |= FILE_APPEND_DATA;
    if (Access & XBOX_FILE_READ_ATTRIBUTES)   result |= FILE_READ_ATTRIBUTES;
    if (Access & XBOX_FILE_WRITE_ATTRIBUTES)  result |= FILE_WRITE_ATTRIBUTES;
    if (Access & XBOX_SYNCHRONIZE)            result |= SYNCHRONIZE;
    if (Access & XBOX_DELETE)                  result |= DELETE;
    if (result == 0 || result == SYNCHRONIZE)
        result |= GENERIC_READ;
    return result;
}

/* Convert Xbox share access to Win32 */
static DWORD xbox_share_to_win32(ULONG Share)
{
    DWORD result = 0;
    if (Share & 0x01) result |= FILE_SHARE_READ;
    if (Share & 0x02) result |= FILE_SHARE_WRITE;
    if (Share & 0x04) result |= FILE_SHARE_DELETE;
    return result;
}

/* Translate an Xbox OBJECT_ATTRIBUTES path to a Win32 wide path */
static BOOL translate_obj_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
                               WCHAR* win_path, DWORD buf_size)
{
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path)
        return FALSE;
    if (ObjectAttributes->RootDirectory && xbox_path[0] != '\\' &&
        !(xbox_path[0] && xbox_path[1] == ':')) {
        /* XDeleteSaveGame opens each child relative to its save directory. */
        DWORD used = GetFinalPathNameByHandleW(ObjectAttributes->RootDirectory,
            win_path, buf_size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!used || used >= buf_size || used + 1 >= buf_size) return FALSE;
        if (win_path[used - 1] != L'\\') win_path[used++] = L'\\';
        int count = MultiByteToWideChar(CP_ACP, 0, xbox_path, -1,
            win_path + used, (int)(buf_size - used));
        return count != 0;
    }
    return xbox_translate_path(xbox_path, win_path, buf_size);
}

/* FATX stores years 2000 to 2127. 0 and -1 mean no change, and are always fine. */
static int fatx_time_ok(const LARGE_INTEGER *t)
{
    FILETIME ft;
    SYSTEMTIME st;
    if (t->QuadPart == 0 || t->QuadPart == -1) return 1;
    ft.dwLowDateTime = t->LowPart;
    ft.dwHighDateTime = (DWORD)t->HighPart;
    return FileTimeToSystemTime(&ft, &st) && st.wYear >= 2000 && st.wYear <= 2127;
}

/*
 * What the file object holds on a console and a host handle does not. Only
 * handles with something to say are listed.
 *
 * nobuf: opened with FILE_NO_INTERMEDIATE_BUFFERING. FATX wants offsets and
 * lengths in whole 512-byte sectors and answers C000000D otherwise (0x80030D30,
 * position 0x80030255). The disc only checks the position, against its 2048
 * (0x8002D988). The host's own flag is not used: it would apply the PC disk's
 * sector size, and also demand an aligned buffer, which the console does not.
 * disc: on the disc, which answers as XDVDFS (0x8003229B, 0x80032360).
 * pos: a negative position. FATX and XDVDFS just store it, the host refuses; a
 * read from there is past the end and a write a full disk.
 */
typedef struct { HANDLE h; BYTE nobuf, disc, has_pos; LONGLONG pos; } FILE_REC;
static FILE_REC *s_recs;
static int s_rec_count, s_rec_cap;
static SRWLOCK s_rec_lock = SRWLOCK_INIT;

static FILE_REC *rec_find(HANDLE h)
{
    int i;
    for (i = 0; i < s_rec_count; i++)
        if (s_recs[i].h == h)
            return &s_recs[i];
    return NULL;
}

static FILE_REC *rec_add(HANDLE h)
{
    FILE_REC *r;
    if (s_rec_count == s_rec_cap) {
        int cap = s_rec_cap ? s_rec_cap * 2 : 32;
        FILE_REC *n = (FILE_REC *)realloc(s_recs, (size_t)cap * sizeof(FILE_REC));
        if (!n) return NULL;
        s_recs = n;
        s_rec_cap = cap;
    }
    r = &s_recs[s_rec_count++];
    memset(r, 0, sizeof(*r));
    r->h = h;
    return r;
}

static void rec_note(HANDLE h, int nobuf, int disc)
{
    AcquireSRWLockExclusive(&s_rec_lock);
    {
        FILE_REC *r = rec_find(h);
        if (!r) r = rec_add(h);
        if (r) { r->nobuf = (BYTE)nobuf; r->disc = (BYTE)disc; r->has_pos = 0; }
    }
    ReleaseSRWLockExclusive(&s_rec_lock);
}

/* A copy, all zeros for a handle with nothing listed */
static FILE_REC rec_get(HANDLE h)
{
    FILE_REC out;
    memset(&out, 0, sizeof(out));
    AcquireSRWLockShared(&s_rec_lock);
    {
        FILE_REC *r = rec_find(h);
        if (r) out = *r;
    }
    ReleaseSRWLockShared(&s_rec_lock);
    return out;
}

static void rec_set_pos(HANDLE h, int has, LONGLONG pos)
{
    AcquireSRWLockExclusive(&s_rec_lock);
    {
        FILE_REC *r = rec_find(h);
        if (!r && has) r = rec_add(h);
        if (r) { r->has_pos = (BYTE)has; r->pos = pos; }
    }
    ReleaseSRWLockExclusive(&s_rec_lock);
}

static void rec_drop(HANDLE h)
{
    AcquireSRWLockExclusive(&s_rec_lock);
    {
        FILE_REC *r = rec_find(h);
        if (r) *r = s_recs[--s_rec_count];
    }
    ReleaseSRWLockExclusive(&s_rec_lock);
}

/* A duplicate is the same file object on a console, so it answers the same. */
void xbox_file_handle_duplicated(HANDLE from, HANDLE to, BOOL closed_source)
{
    FILE_REC old = rec_get(from);
    if (old.h) {
        rec_note(to, old.nobuf, old.disc);
        if (old.has_pos) rec_set_pos(to, 1, old.pos);
    }
    if (closed_source) rec_drop(from);
}

/* The XDVDFS view of a file: one time for all four, read-only, nothing else */
#define DISC_INFO(info, is_dir) do { \
        (info)->CreationTime = (info)->LastWriteTime; \
        (info)->LastAccessTime = (info)->LastWriteTime; \
        (info)->ChangeTime = (info)->LastWriteTime; \
        (info)->FileAttributes = (is_dir) ? 0x11 : 0x01; \
    } while (0)

static int fatx_is_directory(HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION fi;
    return GetFileInformationByHandle(h, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* A file's attributes as FATX reports them: its own bits, 0x80 (normal) when it has none. The PC's file systems set
 * archive (0x20) on everything, so it says nothing and is dropped: a title that sets it itself will not read it back. */
static ULONG fatx_attributes(DWORD host)
{
    ULONG a = host & 0x17;
    return a ? a : 0x80;
}

/* The part of a guest path after its volume: "\\??\\T:\\", a device or partition, or a drive letter; NULL if none */
static const char *volume_relative(const char *path, size_t len, size_t *rest, int *disc)
{
    size_t skip = 0;
    *disc = 0;
    if (len >= 4 && !strncmp(path, "\\??\\", 4)) skip = 4;
    if (len >= skip + 2 && path[skip + 1] == ':') {
        /* Whatever a title linked this letter to decides; otherwise D: is the disc */
        extern const char *xbox_SymlinkTarget(const char *);
        char link[8] = "\\??\\X:";
        const char *target;
        link[4] = (char)toupper((unsigned char)path[skip]);
        target = xbox_SymlinkTarget(link);
        if (target) *disc = !_strnicmp(target, "\\Device\\CdRom0", 14);
        else if ((path[skip] | 0x20) == 'd') *disc = 1;
        skip += 2;
    } else if (skip == 0 && len >= 8 && !_strnicmp(path, "\\Device\\", 8)) {
        size_t i = 8, parts = 1;
        if (len >= 14 && !_strnicmp(path + 8, "CdRom0", 6)) *disc = 1;
        if (len >= 17 && !_strnicmp(path + 8, "Harddisk0", 9)) parts = 2;
        while (i < len && parts) {
            if (path[i] == '\\') parts--;
            if (parts) i++;
        }
        skip = i;
    } else if (skip) {
        return NULL;
    }
    *rest = len - skip;
    return path + skip;
}

/* The FATX driver's name rules: at most 42 bytes, not "." or "..", none of these characters */
static int fatx_name_ok(const char *name, size_t n)
{
    if (n == 0 || n > 42) return 0;
    if (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.'))) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || strchr("\"*+,/:;<=>?\\|", c)) return 0;
    }
    return 1;
}

static int fatx_path_ok(const char *path, size_t n)
{
    size_t i = 0;
    if (n > 250) return 0;
    if (n && path[0] == '\\') i = 1;
    if (n > i && path[n - 1] == '\\') n--;
    while (i < n) {
        size_t e = i;
        while (e < n && path[e] != '\\') e++;
        if (!fatx_name_ok(path + i, e - i)) return 0;
        i = e + 1;
    }
    return 1;
}

/* The path names a volume itself, nothing inside it */
static int volume_is_whole_path(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    const char *xp = get_xbox_path(ObjectAttributes);
    size_t rest = 0;
    int disc;
    const char *rel = xp ? volume_relative(xp, ObjectAttributes->ObjectName->Length, &rest, &disc) : NULL;
    return rel && (rest == 0 || (rest == 1 && rel[0] == '\\'));
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    int keep_nobuf = 0, keep_disc = 0;
    WCHAR win_path[MAX_PATH];
    HANDLE h;
    DWORD flags_and_attrs = FILE_ATTRIBUTE_NORMAL;
    int existed = 0;
    (void)AllocationSize;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    /* A partition device opened as a directory.
     *
     * The path layer maps \Device\Harddisk0\PartitionN to a PartitionN.img
     * backing file, but a title asking for free space opens the partition with
     * FILE_DIRECTORY_FILE | FILE_OPEN_FOR_FREE_SPACE_QUERY -- it wants the
     * volume, not the bytes. Opening a regular file as a directory fails, and
     * the title reads STATUS_OBJECT_PATH_NOT_FOUND as "no such volume".
     *
     * Half-Life 2's CRT probes partition0 this way during startup and treats
     * the failure as fatal. Redirecting to the directory that holds the image
     * gives a handle that is valid for exactly what the caller is going to do
     * with it, which is NtQueryVolumeInformationFile. */
    if ((CreateOptions & XBOX_FILE_DIRECTORY_FILE) &&
        volume_is_whole_path(ObjectAttributes) &&   /* an ordinary file opened as a directory is an error, below */
        GetFileAttributesW(win_path) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW(win_path) & FILE_ATTRIBUTE_DIRECTORY)) {
        WCHAR *slash = wcsrchr(win_path, L'\\');
        if (slash && slash != win_path) {
            *slash = 0;
            xbox_log(XBOX_LOG_INFO, XBOX_LOG_FILE,
                     "NtCreateFile: directory open of a device image, "
                     "using its containing directory instead");
        }
    }

    /* The file system drivers' checks, in their order, before the host sees it */
    {
        const char *xp = get_xbox_path(ObjectAttributes);
        size_t xn = ObjectAttributes->ObjectName->Length, rest = 0;
        int disc = 0;
        const char *rel = xp ? volume_relative(xp, xn, &rest, &disc) : NULL;
        DWORD attr = GetFileAttributesW(win_path);
        keep_nobuf = rel && (CreateOptions & XBOX_FILE_NO_INTERMEDIATE_BUFFERING) && !(CreateOptions & XBOX_FILE_DIRECTORY_FILE);
        keep_disc = rel && disc;
        size_t wn = wcslen(win_path);
        int trailing = xn && xp && xp[xn - 1] == '\\';
        (void)wn;

        if (CreateOptions & 0x2000)   /* FILE_OPEN_BY_FILE_ID */
            return (NTSTATUS)0xC0000002L;
        if (disc) {
            /* Read-only: anything that would write is refused, and it can only open what exists */
            ACCESS_MASK a = DesiredAccess;
            if (a & 0x80000000u) a |= 0x00120089;
            if (a & 0x40000000u) a |= 0x00120116;
            if (a & 0x20000000u) a |= 0x001200A0;
            if (a & 0x10000000u) a |= 0x001F01FF;
            if ((a & 0x50156u) || (CreateDisposition != XBOX_FILE_OPEN && CreateDisposition != 3 /* FILE_OPEN_IF */))
                return (NTSTATUS)0xC0000022L;
            CreateDisposition = XBOX_FILE_OPEN;   /* FILE_OPEN_IF must not create a file in the PC's copy of the disc */
        } else if (rel && rest && !fatx_path_ok(rel, rest)) {
            return (NTSTATUS)0xC0000033L;     /* STATUS_OBJECT_NAME_INVALID */
        }
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                if (CreateDisposition != XBOX_FILE_OPEN && CreateDisposition != 3)
                    return (NTSTATUS)0xC0000035L;      /* STATUS_OBJECT_NAME_COLLISION */
                if (CreateOptions & 0x40)              /* FILE_NON_DIRECTORY_FILE */
                    return (NTSTATUS)0xC00000BAL;      /* STATUS_FILE_IS_A_DIRECTORY */
            } else {
                if (CreateDisposition == XBOX_FILE_CREATE)
                    return (NTSTATUS)0xC0000035L;
                if (trailing || (CreateOptions & XBOX_FILE_DIRECTORY_FILE))
                    return (NTSTATUS)0xC0000103L;      /* STATUS_NOT_A_DIRECTORY */
                if ((attr & FILE_ATTRIBUTE_READONLY) && (CreateOptions & 0x1000))
                    return (NTSTATUS)0xC0000121L;      /* STATUS_CANNOT_DELETE */
            }
            existed = 1;
        } else if (trailing && (CreateOptions & 0x40) &&
                   CreateDisposition != XBOX_FILE_OPEN && CreateDisposition != 4) {
            return (NTSTATUS)0xC0000033L;
        }
    }

    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            CreateDirectoryW(win_path, NULL);
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, NULL);
    } else {
        if (FileAttributes & XBOX_FILE_ATTRIBUTE_READONLY)
            flags_and_attrs |= FILE_ATTRIBUTE_READONLY;
        h = CreateFileW(win_path, xbox_access_to_win32(DesiredAccess),
            xbox_share_to_win32(ShareAccess), NULL,
            xbox_disposition_to_win32(CreateDisposition), flags_and_attrs, NULL);
    }

    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        /* Kept for the caller's trace. An NTSTATUS says "it did not open";
         * only the Win32 error distinguishes a title probing for a file that
         * is genuinely absent from one it cannot open because this runtime
         * already holds it open with a share mode the second open forbids --
         * and those need opposite responses. */
        g_xbox_last_file_error = (uint32_t)err;
        /* A warning, not a compiled-out trace: a failed open is how a title
         * silently decides a volume or asset is missing, and in a Release
         * build that decision was invisible. */
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "NtCreateFile FAILED: %S (err=%u)", win_path, err);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        switch (err) {
            case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
            case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
            case ERROR_ACCESS_DENIED:  return STATUS_ACCESS_DENIED;
            case ERROR_ALREADY_EXISTS:
            case ERROR_FILE_EXISTS:    return STATUS_OBJECT_NAME_COLLISION;
            case ERROR_SHARING_VIOLATION: return (NTSTATUS)0xC0000043L;
            case ERROR_INVALID_NAME:   return (NTSTATUS)0xC0000033L;
            case ERROR_DISK_FULL:      return (NTSTATUS)0xC000007FL;
            default:                   return STATUS_UNSUCCESSFUL;
        }
    }

    *FileHandle = h;
    if (keep_nobuf || keep_disc)
        rec_note(h, keep_nobuf, keep_disc);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        /* FILE_OPENED 1, FILE_CREATED 2, FILE_OVERWRITTEN 3, as the driver: supersede and overwrite count as 3 */
        IoStatusBlock->Information = !existed ? 2
            : (CreateDisposition == 0 || CreateDisposition == 4 || CreateDisposition == 5) ? 3 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %S -> handle=%p", win_path, h);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_read = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    /* 0x80030D30: past the end is C0000011 first, which the read below answers */
    if (Length) {
        FILE_REC rec = rec_get(FileHandle);
        int from_here = !(ByteOffset && ByteOffset->QuadPart >= 0);
        if (rec.has_pos && from_here) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            IoStatusBlock->Information = 0;
            return STATUS_END_OF_FILE;
        }
        if (rec.has_pos)
            rec_set_pos(FileHandle, 0, 0);
        if (rec.nobuf && !rec.disc) {
            LARGE_INTEGER zero, at, size;
            zero.QuadPart = 0;
            at.QuadPart = 0;
            if (!from_here)
                at = *ByteOffset;
            else
                SetFilePointerEx(FileHandle, zero, &at, FILE_CURRENT);
            if (GetFileSizeEx(FileHandle, &size) && at.QuadPart < size.QuadPart &&
                ((at.LowPart | Length) & 0x1FF)) {
                IoStatusBlock->Status = (NTSTATUS)0xC000000DL;
                IoStatusBlock->Information = 0;
                return (NTSTATUS)0xC000000DL;
            }
        }
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, &ov);
    } else {
        result = ReadFile(FileHandle, Buffer, Length, &bytes_read, NULL);
    }

    if (result || GetLastError() == ERROR_HANDLE_EOF) {
        IoStatusBlock->Information = bytes_read;
        if (bytes_read == 0 && Length > 0) {
            IoStatusBlock->Status = STATUS_END_OF_FILE;
            return STATUS_END_OF_FILE;
        }
        IoStatusBlock->Status = STATUS_SUCCESS;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p, len=%u) failed err=%u",
               FileHandle, Length, GetLastError());
    {
        NTSTATUS st = fatx_is_directory(FileHandle) ? (NTSTATUS)0xC0000010L : STATUS_UNSUCCESSFUL;
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = 0;
        return st;
    }
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    DWORD bytes_written = 0;
    BOOL result;
    OVERLAPPED ov;
    (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    /* 0x80030C54: FATX files stop at 4 GB, so a write reaching past it is a full
     * disk, and only then come the sector rules (0x80030D30). */
    if (Length) {
        LARGE_INTEGER zero, at;
        NTSTATUS bad = 0;
        FILE_REC rec = rec_get(FileHandle);
        int given = ByteOffset && ByteOffset->QuadPart >= 0;
        int to_end = ByteOffset && ByteOffset->QuadPart == -1;
        zero.QuadPart = 0;
        at.QuadPart = 0;
        if (given)
            at = *ByteOffset;
        else if (to_end)
            GetFileSizeEx(FileHandle, &at);
        else
            SetFilePointerEx(FileHandle, zero, &at, FILE_CURRENT);
        if (rec.has_pos && !given && !to_end)
            at.QuadPart = rec.pos;
        if ((at.HighPart || at.LowPart + Length <= at.LowPart) && !fatx_is_directory(FileHandle))
            bad = (NTSTATUS)0xC000007FL;
        else if (((at.LowPart | Length) & 0x1FF) && rec.nobuf)
            bad = (NTSTATUS)0xC000000DL;
        if (!bad && rec.has_pos)
            rec_set_pos(FileHandle, 0, 0);
        if (bad) {
            IoStatusBlock->Status = bad;
            IoStatusBlock->Information = 0;
            return bad;
        }
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0) {
        memset(&ov, 0, sizeof(ov));
        ov.Offset = ByteOffset->LowPart;
        ov.OffsetHigh = ByteOffset->HighPart;
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, &ov);
    } else {
        /* an offset of -1: the end of the file (-2, or none: where the file is) */
        if (ByteOffset && ByteOffset->QuadPart == -1) {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            SetFilePointerEx(FileHandle, zero, NULL, FILE_END);
        }
        result = WriteFile(FileHandle, Buffer, Length, &bytes_written, NULL);
    }

    if (result) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = bytes_written;
        if (Event) SetEvent(Event);
        return STATUS_SUCCESS;
    }

    {
        DWORD err = GetLastError();
        NTSTATUS st = fatx_is_directory(FileHandle) ? (NTSTATUS)0xC0000010L   /* STATUS_INVALID_DEVICE_REQUEST */
                    : err == ERROR_ACCESS_DENIED ? (NTSTATUS)0xC0000022L      /* opened without write access */
                    : err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL ? (NTSTATUS)0xC000007FL
                    : err == ERROR_INVALID_HANDLE ? (NTSTATUS)0xC0000008L
                    : STATUS_UNSUCCESSFUL;
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p, len=%u) failed err=%u", FileHandle, Length, err);
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = 0;
        return st;
    }
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        xbox_dir_context_drop(Handle);
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    WCHAR win_path[MAX_PATH];
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %S", win_path);
    if (DeleteFileW(win_path))    return STATUS_SUCCESS;
    if (RemoveDirectoryW(win_path)) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* FileRenameInformation. The handle needs DELETE access, as on the console. */
NTSTATUS xbox_RenameFile(HANDLE FileHandle, const char* xbox_path, HANDLE Root, BOOLEAN Replace)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    WCHAR win_path[MAX_PATH];
    union { FILE_RENAME_INFO info; char bytes[sizeof(FILE_RENAME_INFO) + sizeof(WCHAR) * MAX_PATH]; } u;
    DWORD chars, err;

    name.Buffer = (PCHAR)xbox_path;
    name.Length = (USHORT)strlen(xbox_path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    memset(&oa, 0, sizeof oa);
    oa.RootDirectory = Root;
    oa.ObjectName = &name;
    {   /* As FATX: the new name's last part must be a good name; an existing file in the way must be replaceable, and
         * not a directory or read-only even then */
        const char *end = xbox_path + name.Length, *last = end;
        DWORD attr;
        if (end > xbox_path && end[-1] == '\\') end--, last--;
        while (last > xbox_path && last[-1] != '\\') last--;
        if (!fatx_name_ok(last, (size_t)(end - last)))
            return (NTSTATUS)0xC0000033L;
        if (!translate_obj_path(&oa, win_path, MAX_PATH))
            return STATUS_OBJECT_PATH_NOT_FOUND;
        attr = GetFileAttributesW(win_path);
        if (attr != INVALID_FILE_ATTRIBUTES &&
            (!Replace || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY))))
            return STATUS_OBJECT_NAME_COLLISION;
    }
    chars = (DWORD)wcslen(win_path);
    memset(&u, 0, sizeof u);
    u.info.ReplaceIfExists = Replace;
    u.info.FileNameLength = chars * sizeof(WCHAR);
    memcpy(u.info.FileName, win_path, (chars + 1) * sizeof(WCHAR));
    if (SetFileInformationByHandle(FileHandle, FileRenameInfo, &u, sizeof u))
        return STATUS_SUCCESS;
    err = GetLastError();
    return err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS ? STATUS_OBJECT_NAME_COLLISION
         : err == ERROR_ACCESS_DENIED  ? STATUS_ACCESS_DENIED
         : err == ERROR_INVALID_HANDLE ? (NTSTATUS)0xC0000008L
         : err == ERROR_PATH_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND
                                       : STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FileInformationClass) {
        /* FATX answers only these three; the I/O manager handles mode and alignment before us. Basic and standard
         * come back C000000D on a console. */
        case 6: {   /* FileInternalInformation: an id for the file */
            BY_HANDLE_FILE_INFORMATION fi;
            ULONG *out = (ULONG *)FileInformation;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return (NTSTATUS)0xC0000008L;
            out[0] = fi.nFileIndexLow;
            out[1] = fi.nFileIndexHigh;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 8;
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            LARGE_INTEGER pos, zero;
            FILE_REC rec = rec_get(FileHandle);
            zero.QuadPart = 0;
            if (rec.has_pos)
                pos.QuadPart = rec.pos;
            else if (!SetFilePointerEx(FileHandle, zero, &pos, FILE_CURRENT))
                return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            BY_HANDLE_FILE_INFORMATION fi;
            if (!GetFileInformationByHandle(FileHandle, &fi))
                return STATUS_UNSUCCESSFUL;
            info->CreationTime.LowPart    = fi.ftCreationTime.dwLowDateTime;
            info->CreationTime.HighPart   = fi.ftCreationTime.dwHighDateTime;
            info->LastAccessTime.LowPart  = fi.ftLastAccessTime.dwLowDateTime;
            info->LastAccessTime.HighPart = fi.ftLastAccessTime.dwHighDateTime;
            info->LastWriteTime.LowPart   = fi.ftLastWriteTime.dwLowDateTime;
            info->LastWriteTime.HighPart  = fi.ftLastWriteTime.dwHighDateTime;
            info->ChangeTime = info->LastWriteTime;
            /* The file's size in both, 0 for a directory, as FATX */
            info->EndOfFile.QuadPart = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
                : ((LONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
            info->AllocationSize = info->EndOfFile;
            info->FileAttributes = fatx_attributes(fi.dwFileAttributes);
            if (rec_get(FileHandle).disc)
                DISC_INFO(info, fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
            return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    if (FileInformationClass != XboxFilePositionInformation && rec_get(FileHandle).disc) {
        IoStatusBlock->Status = (NTSTATUS)0xC000000DL;   /* the disc sets nothing else: 0x8002D97E */
        return (NTSTATUS)0xC000000DL;
    }

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            FILE_REC rec = rec_get(FileHandle);
            if (rec.nobuf && (info->CurrentByteOffset.LowPart & (rec.disc ? 0x7FFu : 0x1FFu)))
                return (NTSTATUS)0xC000000DL;   /* 0x80030255, 0x8002D988 */
            if (info->CurrentByteOffset.QuadPart < 0) {
                rec_set_pos(FileHandle, 1, info->CurrentByteOffset.QuadPart);
                IoStatusBlock->Status = STATUS_SUCCESS;
                return STATUS_SUCCESS;
            }
            if (rec.has_pos)
                rec_set_pos(FileHandle, 0, 0);
            if (!SetFilePointerEx(FileHandle, info->CurrentByteOffset, NULL, FILE_BEGIN)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetFilePosition failed: offset=%lld err=%u",
                    (long long)info->CurrentByteOffset.QuadPart, GetLastError());
                return STATUS_UNSUCCESSFUL;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            if (fatx_is_directory(FileHandle)) return STATUS_INVALID_PARAMETER;
            if (info->EndOfFile.HighPart) return (NTSTATUS)0xC000007FL;   /* FATX sizes are 32 bits: disk full */
            LARGE_INTEGER cur, zero = {0};
            SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
            SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
            if (!SetEndOfFile(FileHandle)) {
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                    "SetEndOfFile failed: size=%lld err=%u",
                    (long long)info->EndOfFile.QuadPart, GetLastError());
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
                return STATUS_UNSUCCESSFUL;
            }
            if (cur.QuadPart <= info->EndOfFile.QuadPart)
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileAllocationInformation: {
            /* Reserve space for a file. Halo's save path calls this before
             * writing, and an unimplemented class here returned
             * STATUS_NOT_IMPLEMENTED, which the title turned into DOS error
             * 317 and reported as "couldn't open or create saved game file".
             *
             * Same payload shape as EndOfFile: one LARGE_INTEGER. Windows
             * FileAllocationInfo is the direct equivalent; if the filesystem
             * declines it, fall back to setting the size, since the caller
             * only needs the space to exist. */
            PXBOX_FILE_END_OF_FILE_INFORMATION info =
                (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            FILE_ALLOCATION_INFO fai;
            if (fatx_is_directory(FileHandle)) return STATUS_INVALID_PARAMETER;
            if (info->EndOfFile.HighPart) return (NTSTATUS)0xC000007FL;
            fai.AllocationSize = info->EndOfFile;
            if (!SetFileInformationByHandle(FileHandle, FileAllocationInfo,
                                            &fai, sizeof(fai))) {
                LARGE_INTEGER cur, zero = {0};
                SetFilePointerEx(FileHandle, zero, &cur, FILE_CURRENT);
                SetFilePointerEx(FileHandle, info->EndOfFile, NULL, FILE_BEGIN);
                SetEndOfFile(FileHandle);
                SetFilePointerEx(FileHandle, cur, NULL, FILE_BEGIN);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            FILE_DISPOSITION_INFO fdi;
            BY_HANDLE_FILE_INFORMATION fi;
            if (info->DeleteFile && GetFileInformationByHandle(FileHandle, &fi) &&
                (fi.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
                return (NTSTATUS)0xC0000121L;   /* STATUS_CANNOT_DELETE */
            fdi.DeleteFile = info->DeleteFile;
            if (!SetFileInformationByHandle(FileHandle, FileDispositionInfo, &fdi, sizeof(fdi))) {
                DWORD err = GetLastError();
                NTSTATUS st = err == ERROR_INVALID_HANDLE ? (NTSTATUS)0xC0000008L
                            : err == ERROR_ACCESS_DENIED  ? (NTSTATUS)0xC0000022L
                            : err == ERROR_DIR_NOT_EMPTY  ? (NTSTATUS)0xC0000101L
                                                          : STATUS_UNSUCCESSFUL;
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                         "SetFileDispositionInfo failed: err=%u", err);
                IoStatusBlock->Status = st;
                return st;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            /* As FATX: a time of 0 or -1 changes nothing. Attributes of 0 change nothing, otherwise its own bits; a
             * directory stays one, and a file cannot become one. */
            FILE_BASIC_INFO fbi;
            BY_HANDLE_FILE_INFORMATION fi;
            memset(&fbi, 0, sizeof fbi);
            if (!fatx_time_ok(&info->CreationTime) || !fatx_time_ok(&info->LastAccessTime) ||
                !fatx_time_ok(&info->LastWriteTime))
                return STATUS_INVALID_PARAMETER;
#define FATX_TIME(t) ((t).QuadPart == -1 ? 0 : (t).QuadPart)
            fbi.CreationTime.QuadPart = FATX_TIME(info->CreationTime);
            fbi.LastAccessTime.QuadPart = FATX_TIME(info->LastAccessTime);
            fbi.LastWriteTime.QuadPart = FATX_TIME(info->LastWriteTime);
#undef FATX_TIME
            if (info->FileAttributes && GetFileInformationByHandle(FileHandle, &fi)) {
                ULONG a = info->FileAttributes & 0x37;
                if (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) a |= 0x10;
                else if (a & 0x10) return STATUS_INVALID_PARAMETER;
                fbi.FileAttributes = a ? a : FILE_ATTRIBUTE_NORMAL;
            }
            SetFileInformationByHandle(FileHandle, FileBasicInfo, &fbi, sizeof fbi);
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            IoStatusBlock->Status = STATUS_INVALID_PARAMETER;   /* what FATX says to any other class */
            return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    /* 0x8003229B: the disc's own answers */
    if (rec_get(FileHandle).disc) {
        ULONG *out = (ULONG *)FsInformation;
        NTSTATUS st = STATUS_SUCCESS;
        ULONG info = 0;
        memset(FsInformation, 0, Length);
        switch ((int)FsInformationClass) {
            case 1: info = 0x11; break;   /* volume: all zeros */
            case 3:                       /* size: the disc's sectors, none free */
                out[0] = 3820880;         /* a console says this disc's own; here a dual-layer game partition */
                out[4] = 1;
                out[5] = 0x800;
                info = 0x18;
                break;
            case 4: out[0] = 2; info = 8; break;   /* FILE_DEVICE_CD_ROM */
            case 5:
                out[1] = 0xFF;
                out[2] = 4;
                info = 0xC;
                if (Length >= 0x10) { memcpy(out + 3, "GDFX", 4); info = 0x10; }
                else st = (NTSTATUS)0x80000005L;
                break;
            default: st = (NTSTATUS)0xC000000DL; break;
        }
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = info;
        return st;
    }

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                info->TotalAllocationUnits.QuadPart = total_bytes.QuadPart / cs;
                info->AvailableAllocationUnits.QuadPart = free_bytes.QuadPart / cs;
            } else {
                info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
                info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        case 7: {  /* FileFsFullSizeInformation: the same, with two free counts */
            ULONGLONG *q = (ULONGLONG *)FsInformation;
            ULONG *d = (ULONG *)(q + 3);
            ULARGE_INTEGER free_bytes, total_bytes, total_free;
            ULONGLONG cs = (ULONGLONG)XBOX_BYTES_PER_SECTOR * XBOX_SECTORS_PER_CLUSTER;
            if (GetDiskFreeSpaceExW(NULL, &free_bytes, &total_bytes, &total_free)) {
                q[0] = total_bytes.QuadPart / cs;
                q[1] = free_bytes.QuadPart / cs;
                q[2] = total_free.QuadPart / cs;
            } else {
                q[0] = 1048576;
                q[1] = q[2] = 524288;
            }
            d[0] = XBOX_SECTORS_PER_CLUSTER;
            d[1] = XBOX_BYTES_PER_SECTOR;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 32;
            return STATUS_SUCCESS;
        }
        /* The rest as the FATX driver answers. */
        case 1: {   /* FileFsVolumeInformation: no creation time, a serial, no label */
            memset(FsInformation, 0, Length);
            ((ULONG *)FsInformation)[2] = 0x58524543;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0x11;
            return STATUS_SUCCESS;
        }
        case 4: {   /* FileFsDeviceInformation */
            memset(FsInformation, 0, Length);
            ((ULONG *)FsInformation)[0] = 7;   /* FILE_DEVICE_DISK */
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 8;
            return STATUS_SUCCESS;
        }
        case 5: {   /* FileFsAttributeInformation */
            memset(FsInformation, 0, Length);
            ((ULONG *)FsInformation)[1] = 42;   /* longest name */
            ((ULONG *)FsInformation)[2] = 4;
            if (Length < 0x10) {   /* 0x8002FA8C: no room for the name */
                IoStatusBlock->Status = (NTSTATUS)0x80000005L;
                IoStatusBlock->Information = 0xC;
                return (NTSTATUS)0x80000005L;
            }
            memcpy((ULONG *)FsInformation + 3, "FATX", 4);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 16;
            return STATUS_SUCCESS;
        }
        default:   /* the driver refuses what it does not know */
            IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
            IoStatusBlock->Information = 0;
            return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    if (rec_get(FileHandle).disc) {   /* no flush in its table: invalid device request */
        if (IoStatusBlock) {
            IoStatusBlock->Status = (NTSTATUS)0xC0000010L;
            IoStatusBlock->Information = 0;
        }
        return (NTSTATUS)0xC0000010L;
    }
    FlushFileBuffers(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    WCHAR win_path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    if (!translate_obj_path(ObjectAttributes, win_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    if (!GetFileAttributesExW(win_path, GetFileExInfoStandard, &fad)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        return STATUS_UNSUCCESSFUL;
    }

    FileInformation->CreationTime.LowPart    = fad.ftCreationTime.dwLowDateTime;
    FileInformation->CreationTime.HighPart   = fad.ftCreationTime.dwHighDateTime;
    FileInformation->LastAccessTime.LowPart  = fad.ftLastAccessTime.dwLowDateTime;
    FileInformation->LastAccessTime.HighPart = fad.ftLastAccessTime.dwHighDateTime;
    FileInformation->LastWriteTime.LowPart   = fad.ftLastWriteTime.dwLowDateTime;
    FileInformation->LastWriteTime.HighPart  = fad.ftLastWriteTime.dwHighDateTime;
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0
        : ((LONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    FileInformation->AllocationSize = FileInformation->EndOfFile;
    FileInformation->FileAttributes = fatx_attributes(fad.dwFileAttributes);
    {
        const char *xp = get_xbox_path(ObjectAttributes);
        size_t rest = 0;
        int disc = 0;
        if (xp && volume_relative(xp, ObjectAttributes->ObjectName->Length, &rest, &disc) && disc)
            DISC_INFO(FileInformation, fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    }
    return STATUS_SUCCESS;
}

#define MAX_DIR_CONTEXTS 1024
typedef struct {
    HANDLE file_handle;
    HANDLE find_handle;
    BOOL   first_done;
    WIN32_FIND_DATAW find_data;
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

static DIR_CONTEXT* find_or_create_dir_context(HANDLE FileHandle, BOOL create)
{
    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle && s_dir_contexts[i].find_handle != NULL) {
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    if (!create) { LeaveCriticalSection(&s_dir_cs); return NULL; }
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].find_handle == NULL) {
            s_dir_contexts[i].file_handle = FileHandle;
            s_dir_contexts[i].first_done = FALSE;
            LeaveCriticalSection(&s_dir_cs);
            return &s_dir_contexts[i];
        }
    }
    LeaveCriticalSection(&s_dir_cs);
    return NULL;
}

/* A closed directory handle takes its enumeration with it. Host handle values
 * are reused, and a search abandoned part-way (a title that stops at the first
 * match) otherwise carried on under the next directory opened at the same
 * value: its first answer was the old search's next entry. */
void xbox_dir_context_drop(HANDLE FileHandle)
{
    rec_drop(FileHandle);
    if (!s_dir_cs_init)
        return;
    EnterCriticalSection(&s_dir_cs);
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++) {
        if (s_dir_contexts[i].file_handle == FileHandle) {
            if (s_dir_contexts[i].find_handle &&
                s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
                FindClose(s_dir_contexts[i].find_handle);
            s_dir_contexts[i].find_handle = NULL;
            s_dir_contexts[i].file_handle = NULL;
            s_dir_contexts[i].first_done = FALSE;
        }
    }
    LeaveCriticalSection(&s_dir_cs);
}

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    DIR_CONTEXT* ctx;
    PXBOX_FILE_DIRECTORY_INFORMATION entry;
    int first_scan = 0;
    (void)Event; (void)ApcRoutine; (void)ApcContext;

    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    /* As FATX: only a directory, and directory or names information (12) */
    if (!fatx_is_directory(FileHandle)) {
        IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }
    if (FileInformationClass != XboxFileDirectoryInformation && (int)FileInformationClass != 12) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    ctx = find_or_create_dir_context(FileHandle, TRUE);
    if (!ctx)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (RestartScan || !ctx->first_done) {
        if (ctx->find_handle && ctx->find_handle != INVALID_HANDLE_VALUE) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
        }
        WCHAR search_path[MAX_PATH];
        WCHAR dir_path[MAX_PATH];
        DWORD path_len = GetFinalPathNameByHandleW(FileHandle, dir_path, MAX_PATH,
                                                   FILE_NAME_NORMALIZED);
        if (path_len == 0 || path_len >= MAX_PATH) {
            IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        WCHAR* clean_path = dir_path;
        if (wcsncmp(clean_path, L"\\\\?\\", 4) == 0)
            clean_path += 4;
        /* an empty pattern is every entry, as FATX answers it (and the POSIX backend): "dir\\" with nothing after
         * it is no search at all to FindFirstFileW, so HP:PoA's Load Game found its own save folder empty */
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            WCHAR pattern_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, FileName->Buffer, FileName->Length,
                                pattern_wide, MAX_PATH);
            pattern_wide[FileName->Length] = L'\0';
            swprintf_s(search_path, MAX_PATH, L"%s\\%s", clean_path, pattern_wide);
        } else {
            swprintf_s(search_path, MAX_PATH, L"%s\\*", clean_path);
        }
        first_scan = !ctx->first_done;
        ctx->find_handle = FindFirstFileW(search_path, &ctx->find_data);
        if (ctx->find_handle == INVALID_HANDLE_VALUE) {
            NTSTATUS none = first_scan ? (NTSTATUS)0xC000000FL : STATUS_NO_MORE_FILES;   /* STATUS_NO_SUCH_FILE */
            ctx->find_handle = NULL;
            IoStatusBlock->Status = none;
            return none;
        }
        ctx->first_done = TRUE;
    } else {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
    }

    /* FATX enumeration never exposes the host's dot directories. */
    while (!wcscmp(ctx->find_data.cFileName, L".") ||
           !wcscmp(ctx->find_data.cFileName, L"..")) {
        if (!FindNextFileW(ctx->find_handle, &ctx->find_data)) {
            FindClose(ctx->find_handle);
            ctx->find_handle = NULL;
            ctx->file_handle = NULL;
            {
                NTSTATUS none = first_scan ? (NTSTATUS)0xC000000FL : STATUS_NO_MORE_FILES;
                IoStatusBlock->Status = none;
                return none;
            }
        }
    }

    entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    char filename_ansi[MAX_PATH];
    int name_len = WideCharToMultiByte(CP_ACP, 0, ctx->find_data.cFileName, -1,
                                       filename_ansi, MAX_PATH, NULL, NULL);
    if (name_len > 0) name_len--;

    {
        /* The header, then as much of the name as fits; a name cut short is STATUS_BUFFER_OVERFLOW. FATX reports the
         * full name length regardless, and only the header's fields of the buffer are cleared. */
        ULONG header_size = (int)FileInformationClass == 12 ? 0xC : 0x40;
        ULONG room = Length > header_size ? (Length - header_size) & ~3u : 0;
        ULONG copy = (ULONG)name_len <= room ? (ULONG)name_len : room;
        NTSTATUS st = (ULONG)name_len <= room ? STATUS_SUCCESS : (NTSTATUS)0x80000005L;

        memset(entry, 0, Length < header_size ? Length : header_size);
        if ((int)FileInformationClass == 12) {
            ((ULONG *)FileInformation)[2] = (ULONG)name_len;
            if (copy) memcpy((BYTE *)FileInformation + 0xC, filename_ansi, copy);
        } else {
            int is_dir = (ctx->find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry->CreationTime.LowPart   = ctx->find_data.ftCreationTime.dwLowDateTime;
            entry->CreationTime.HighPart  = ctx->find_data.ftCreationTime.dwHighDateTime;
            entry->LastAccessTime.LowPart = ctx->find_data.ftLastAccessTime.dwLowDateTime;
            entry->LastAccessTime.HighPart = ctx->find_data.ftLastAccessTime.dwHighDateTime;
            entry->LastWriteTime.LowPart  = ctx->find_data.ftLastWriteTime.dwLowDateTime;
            entry->LastWriteTime.HighPart = ctx->find_data.ftLastWriteTime.dwHighDateTime;
            entry->ChangeTime = entry->LastWriteTime;
            entry->EndOfFile.QuadPart = is_dir ? 0
                : ((LONGLONG)ctx->find_data.nFileSizeHigh << 32) | ctx->find_data.nFileSizeLow;
            entry->AllocationSize = entry->EndOfFile;
            entry->FileAttributes = fatx_attributes(ctx->find_data.dwFileAttributes);
            if (rec_get(FileHandle).disc)
                DISC_INFO(entry, is_dir);
            entry->FileNameLength = name_len;
            if (copy) memcpy(entry->FileName, filename_ansi, copy);
        }
        IoStatusBlock->Status = st;
        IoStatusBlock->Information = header_size + copy;
        return st;
    }
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  POSIX backend  =================================== */
/* ======================================================================== */

/* Convert Xbox access mask + disposition to POSIX open() flags. */
static int posix_open_flags(ACCESS_MASK access, ULONG disposition)
{
    int wantWrite = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                               XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    int rw = wantWrite ? O_RDWR : O_RDONLY;
    int extra;

    switch (disposition) {
        case XBOX_FILE_SUPERSEDE:    extra = O_CREAT | O_TRUNC; break;
        case XBOX_FILE_OPEN:         extra = 0;                 break;
        case XBOX_FILE_CREATE:       extra = O_CREAT | O_EXCL;  break;
        case XBOX_FILE_OPEN_IF:      extra = O_CREAT;           break;
        case XBOX_FILE_OVERWRITE:    extra = O_TRUNC;           break;
        case XBOX_FILE_OVERWRITE_IF: extra = O_CREAT | O_TRUNC; break;
        default:                     extra = 0;                 break;
    }
    /* O_TRUNC / O_CREAT imply write intent */
    if ((extra & (O_TRUNC | O_CREAT)) && rw == O_RDONLY)
        rw = O_RDWR;
    if (access & XBOX_FILE_APPEND_DATA)
        extra |= O_APPEND;
    return rw | extra;
}

static void unix_to_filetime(time_t sec, long nsec, LARGE_INTEGER* out)
{
    /* 100-ns ticks since 1601-01-01 */
    ULONGLONG t = 116444736000000000ULL
                + (ULONGLONG)sec * 10000000ULL
                + (ULONGLONG)nsec / 100ULL;
    out->LowPart  = (DWORD)(t & 0xFFFFFFFFULL);
    out->HighPart = (LONG)(t >> 32);
}

static ULONG mode_to_xbox_attrs(mode_t m)
{
    ULONG a = 0;
    if (S_ISDIR(m))      a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    if (!(m & S_IWUSR))  a |= XBOX_FILE_ATTRIBUTE_READONLY;
    if (a == 0)          a = XBOX_FILE_ATTRIBUTE_NORMAL;
    return a;
}

static NTSTATUS errno_to_status(int e)
{
    switch (e) {
        case ENOENT:  return STATUS_OBJECT_NAME_NOT_FOUND;
        case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
        case EACCES:
        case EPERM:   return STATUS_ACCESS_DENIED;
        case EEXIST:  return STATUS_OBJECT_NAME_COLLISION;
        case ENOMEM:  return STATUS_NO_MEMORY;
        default:      return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS __stdcall xbox_NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG CreateDisposition, ULONG CreateOptions)
{
    char host_path[MAX_PATH];
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;

    if (!FileHandle || !ObjectAttributes)
        return STATUS_INVALID_PARAMETER;

    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_FILE, "NtCreateFile: path translation failed");
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    int fd;
    if (CreateOptions & XBOX_FILE_DIRECTORY_FILE) {
        if (CreateDisposition == XBOX_FILE_CREATE || CreateDisposition == XBOX_FILE_OPEN_IF)
            mkdir(host_path, 0755);   /* EEXIST is fine */
        fd = open(host_path, O_RDONLY | O_DIRECTORY);
    } else {
        fd = open(host_path, posix_open_flags(DesiredAccess, CreateDisposition), 0644);
    }

    if (fd < 0) {
        int e = errno;
        XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile FAILED: %s (errno=%d)", host_path, e);
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
            IoStatusBlock->Information = 0;
        }
        return errno_to_status(e);
    }

    *FileHandle = w32_open_handle(fd, host_path);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = (CreateDisposition == XBOX_FILE_CREATE) ? 2 : 1;
    }
    XBOX_TRACE(XBOX_LOG_FILE, "NtCreateFile: %s -> handle=%p", host_path, *FileHandle);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtReadFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = read(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtReadFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Information = (ULONG_PTR)n;
    if (n == 0 && Length > 0) {
        IoStatusBlock->Status = STATUS_END_OF_FILE;
        return STATUS_END_OF_FILE;
    }
    IoStatusBlock->Status = STATUS_SUCCESS;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtWriteFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset)
{
    (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0) {
        IoStatusBlock->Status = STATUS_INVALID_HANDLE;
        return STATUS_INVALID_HANDLE;
    }

    if (ByteOffset && ByteOffset->QuadPart >= 0)
        lseek(fd, (off_t)ByteOffset->QuadPart, SEEK_SET);

    ssize_t n = write(fd, Buffer, Length);
    if (n < 0) {
        XBOX_TRACE(XBOX_LOG_FILE, "NtWriteFile(handle=%p) errno=%d", FileHandle, errno);
        IoStatusBlock->Status = STATUS_UNSUCCESSFUL;
        IoStatusBlock->Information = 0;
        return STATUS_UNSUCCESSFUL;
    }

    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = (ULONG_PTR)n;
    if (Event) SetEvent(Event);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtClose(HANDLE Handle)
{
    XBOX_TRACE(XBOX_LOG_FILE, "NtClose(handle=%p)", Handle);
    if (Handle && Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Handle);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_HANDLE;
}

NTSTATUS __stdcall xbox_NtDeleteFile(PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    char host_path[MAX_PATH];
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    XBOX_TRACE(XBOX_LOG_FILE, "NtDeleteFile: %s", host_path);
    if (unlink(host_path) == 0) return STATUS_SUCCESS;
    if (rmdir(host_path)  == 0) return STATUS_SUCCESS;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* FileRenameInformation. */
NTSTATUS xbox_RenameFile(HANDLE FileHandle, const char* xbox_path, HANDLE Root, BOOLEAN Replace)
{
    char host_path[MAX_PATH];
    const char* from = w32_handle_path(FileHandle);
    struct stat st;

    (void)Root;
    if (!from)
        return (NTSTATUS)0xC0000008L;
    if (!xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (!Replace && stat(host_path, &st) == 0)
        return STATUS_OBJECT_NAME_COLLISION;
    return rename(from, host_path) == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

NTSTATUS __stdcall xbox_NtQueryInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    struct stat st;
    if (FileInformationClass != XboxFilePositionInformation) {
        if (fstat(fd, &st) != 0)
            return STATUS_UNSUCCESSFUL;
    }

    switch (FileInformationClass) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION info = (PXBOX_FILE_BASIC_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_BASIC_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION info = (PXBOX_FILE_STANDARD_INFORMATION)FileInformation;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->NumberOfLinks = (ULONG)st.st_nlink;
            info->DeletePending = FALSE;
            info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_STANDARD_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            off_t pos = lseek(fd, 0, SEEK_CUR);
            if (pos < 0) return STATUS_UNSUCCESSFUL;
            info->CurrentByteOffset.QuadPart = pos;
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            return STATUS_SUCCESS;
        }
        case XboxFileNetworkOpenInformation: {
            PXBOX_FILE_NETWORK_OPEN_INFORMATION info = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)FileInformation;
            unix_to_filetime(st.st_ctime, 0, &info->CreationTime);
            unix_to_filetime(st.st_atime, 0, &info->LastAccessTime);
            unix_to_filetime(st.st_mtime, 0, &info->LastWriteTime);
            info->ChangeTime = info->LastWriteTime;
            info->EndOfFile.QuadPart = st.st_size;
            info->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
            info->FileAttributes = mode_to_xbox_attrs(st.st_mode);
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryInformationFile: unhandled class %d", FileInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtSetInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation, ULONG Length, XBOX_FILE_INFORMATION_CLASS FileInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;

    int fd = w32_handle_fd(FileHandle);
    if (fd < 0)
        return STATUS_INVALID_HANDLE;

    switch (FileInformationClass) {
        case XboxFilePositionInformation: {
            PXBOX_FILE_POSITION_INFORMATION info = (PXBOX_FILE_POSITION_INFORMATION)FileInformation;
            if (lseek(fd, (off_t)info->CurrentByteOffset.QuadPart, SEEK_SET) < 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileEndOfFileInformation: {
            PXBOX_FILE_END_OF_FILE_INFORMATION info = (PXBOX_FILE_END_OF_FILE_INFORMATION)FileInformation;
            if (ftruncate(fd, (off_t)info->EndOfFile.QuadPart) != 0)
                return STATUS_UNSUCCESSFUL;
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileDispositionInformation: {
            PXBOX_FILE_DISPOSITION_INFORMATION info = (PXBOX_FILE_DISPOSITION_INFORMATION)FileInformation;
            /* POSIX: unlinking an open file removes it on last close -- this
             * matches NT "delete on close" semantics exactly. */
            if (info->DeleteFile) {
                const char* p = w32_handle_path(FileHandle);
                if (p) unlink(p);
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        }
        case XboxFileBasicInformation:
            /* Setting file times is non-essential for the game; accept it. */
            IoStatusBlock->Status = STATUS_SUCCESS;
            return STATUS_SUCCESS;
        default:
            /* stderr, not xbox_log: WARN is filtered out by default, and an
             * unimplemented info class is exactly the kind of silent gap that
             * surfaces far away. Halo's save path hits one, gets
             * STATUS_NOT_IMPLEMENTED, converts it to DOS error 317 and asserts
             * "couldn't open or create saved game file". */
            fprintf(stderr, "  [FILE] NtSetInformationFile: unhandled class %d\n",
                    (int)FileInformationClass);
            fflush(stderr);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtQueryVolumeInformationFile(
    HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PVOID FsInformation, ULONG Length, XBOX_FS_INFORMATION_CLASS FsInformationClass)
{
    (void)Length;
    if (!IoStatusBlock || !FsInformation)
        return STATUS_INVALID_PARAMETER;

    switch (FsInformationClass) {
        case XboxFileFsSizeInformation: {
            PXBOX_FILE_FS_SIZE_INFORMATION info = (PXBOX_FILE_FS_SIZE_INFORMATION)FsInformation;
            struct statvfs vfs;
            int fd = w32_handle_fd(FileHandle);
            /* Xbox geometry, not the host's -- see the note on
             * XBOX_SECTORS_PER_CLUSTER above. */
            info->BytesPerSector = XBOX_BYTES_PER_SECTOR;
            info->SectorsPerAllocationUnit = XBOX_SECTORS_PER_CLUSTER;
            if (fd >= 0 && fstatvfs(fd, &vfs) == 0) {
                ULONGLONG cs = (ULONGLONG)info->BytesPerSector * info->SectorsPerAllocationUnit;
                ULONGLONG total = (ULONGLONG)vfs.f_blocks * vfs.f_frsize;
                ULONGLONG avail = (ULONGLONG)vfs.f_bavail * vfs.f_frsize;
                info->TotalAllocationUnits.QuadPart = total / cs;
                info->AvailableAllocationUnits.QuadPart = avail / cs;
            } else {
                info->TotalAllocationUnits.QuadPart = 1048576;
                info->AvailableAllocationUnits.QuadPart = 524288;
            }
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = sizeof(XBOX_FILE_FS_SIZE_INFORMATION);
            return STATUS_SUCCESS;
        }
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                "NtQueryVolumeInformationFile: unhandled class %d", FsInformationClass);
            return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS __stdcall xbox_NtFlushBuffersFile(HANDLE FileHandle, PXBOX_IO_STATUS_BLOCK IoStatusBlock)
{
    int fd = w32_handle_fd(FileHandle);
    if (fd >= 0) fsync(fd);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQueryFullAttributesFile(
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes,
    PXBOX_FILE_NETWORK_OPEN_INFORMATION FileInformation)
{
    char host_path[MAX_PATH];
    struct stat st;

    if (!FileInformation)
        return STATUS_INVALID_PARAMETER;
    const char* xbox_path = get_xbox_path(ObjectAttributes);
    if (!xbox_path || !xbox_translate_path(xbox_path, host_path, MAX_PATH))
        return STATUS_OBJECT_PATH_NOT_FOUND;
    if (stat(host_path, &st) != 0)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    unix_to_filetime(st.st_ctime, 0, &FileInformation->CreationTime);
    unix_to_filetime(st.st_atime, 0, &FileInformation->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &FileInformation->LastWriteTime);
    FileInformation->ChangeTime = FileInformation->LastWriteTime;
    FileInformation->EndOfFile.QuadPart = st.st_size;
    FileInformation->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    FileInformation->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    return STATUS_SUCCESS;
}

/* Directory enumeration state, keyed by the directory's Nt handle. */
#define MAX_DIR_CONTEXTS 1024
typedef struct {
    HANDLE handle;
    DIR*   dir;
    char   pattern[64];
} DIR_CONTEXT;

static DIR_CONTEXT s_dir_contexts[MAX_DIR_CONTEXTS];
static CRITICAL_SECTION s_dir_cs;
static BOOL s_dir_cs_init = FALSE;

NTSTATUS __stdcall xbox_NtQueryDirectoryFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    XBOX_FILE_INFORMATION_CLASS FileInformationClass,
    PXBOX_ANSI_STRING FileName, BOOLEAN RestartScan)
{
    (void)Event; (void)ApcRoutine; (void)ApcContext;
    if (!IoStatusBlock || !FileInformation)
        return STATUS_INVALID_PARAMETER;
    IoStatusBlock->Information = 0;
    if (FileInformationClass != XboxFileDirectoryInformation) {
        IoStatusBlock->Status = STATUS_INVALID_INFO_CLASS;
        return STATUS_INVALID_INFO_CLASS;
    }

    if (!s_dir_cs_init) { InitializeCriticalSection(&s_dir_cs); s_dir_cs_init = TRUE; }
    EnterCriticalSection(&s_dir_cs);

    /* Locate or create the per-handle enumeration context. */
    DIR_CONTEXT* ctx = NULL;
    for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
        if (s_dir_contexts[i].handle == FileHandle) { ctx = &s_dir_contexts[i]; break; }
    if (!ctx) {
        for (int i = 0; i < MAX_DIR_CONTEXTS; i++)
            if (s_dir_contexts[i].handle == NULL) { ctx = &s_dir_contexts[i]; break; }
        if (!ctx) { LeaveCriticalSection(&s_dir_cs); return STATUS_INSUFFICIENT_RESOURCES; }
        ctx->handle = FileHandle;
        ctx->dir = NULL;
    }

    if (RestartScan || ctx->dir == NULL) {
        if (ctx->dir) { closedir(ctx->dir); ctx->dir = NULL; }
        const char* dpath = w32_handle_path(FileHandle);
        if (!dpath) { LeaveCriticalSection(&s_dir_cs); return STATUS_UNSUCCESSFUL; }
        ctx->dir = opendir(dpath);
        if (!ctx->dir) {
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (FileName && FileName->Buffer && FileName->Length > 0) {
            USHORT n = FileName->Length;
            if (n >= sizeof(ctx->pattern)) n = sizeof(ctx->pattern) - 1;
            memcpy(ctx->pattern, FileName->Buffer, n);
            ctx->pattern[n] = '\0';
        } else {
            strcpy(ctx->pattern, "*");
        }
    }

    /* Advance to the next entry matching the search pattern. */
    struct dirent* de;
    const char* dpath = w32_handle_path(FileHandle);
    struct stat st;
    for (;;) {
        de = readdir(ctx->dir);
        if (!de) {
            closedir(ctx->dir);
            ctx->dir = NULL;
            ctx->handle = NULL;
            LeaveCriticalSection(&s_dir_cs);
            IoStatusBlock->Status = STATUS_NO_MORE_FILES;
            return STATUS_NO_MORE_FILES;
        }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0)
            break;
    }

    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s/%s", dpath ? dpath : ".", de->d_name);
    if (stat(full, &st) != 0)
        memset(&st, 0, sizeof(st));
    LeaveCriticalSection(&s_dir_cs);

    PXBOX_FILE_DIRECTORY_INFORMATION entry = (PXBOX_FILE_DIRECTORY_INFORMATION)FileInformation;
    memset(entry, 0, Length);

    int name_len = (int)strlen(de->d_name);
    entry->NextEntryOffset = 0;
    entry->FileIndex = 0;
    unix_to_filetime(st.st_ctime, 0, &entry->CreationTime);
    unix_to_filetime(st.st_atime, 0, &entry->LastAccessTime);
    unix_to_filetime(st.st_mtime, 0, &entry->LastWriteTime);
    entry->ChangeTime = entry->LastWriteTime;
    entry->EndOfFile.QuadPart = st.st_size;
    entry->AllocationSize.QuadPart = (st.st_size + 4095) & ~4095LL;
    entry->FileAttributes = mode_to_xbox_attrs(st.st_mode);
    entry->FileNameLength = name_len;

    ULONG header_size = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    if (name_len > 0 && (header_size + (ULONG)name_len) <= Length)
        memcpy(entry->FileName, de->d_name, name_len);
    IoStatusBlock->Status = STATUS_SUCCESS;
    IoStatusBlock->Information = header_size + name_len;
    return STATUS_SUCCESS;
}

#endif /* _WIN32 */

/* ======================================================================== */
/* ====================  Platform-independent  ============================ */
/* ======================================================================== */

NTSTATUS __stdcall xbox_NtOpenFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess, ULONG OpenOptions)
{
    /* NtOpenFile is NtCreateFile with FILE_OPEN disposition */
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
        IoStatusBlock, NULL, 0, ShareAccess, XBOX_FILE_OPEN, OpenOptions);
}

NTSTATUS __stdcall xbox_IoCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    PXBOX_OBJECT_ATTRIBUTES ObjectAttributes, PXBOX_IO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize, ULONG FileAttributes, ULONG ShareAccess,
    ULONG Disposition, ULONG CreateOptions, ULONG Options)
{
    (void)Options;
    return xbox_NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
        AllocationSize, FileAttributes, ShareAccess, Disposition, CreateOptions);
}

NTSTATUS __stdcall xbox_NtFsControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    /* FSCTL_DISMOUNT_VOLUME after XAPI formats a cache partition: nothing is
     * mounted to drop, see bridge_NtFsControlFile */
    if (FsControlCode == 0x00090020u) {
        if (IoStatusBlock) {
            IoStatusBlock->Status = STATUS_SUCCESS;
            IoStatusBlock->Information = 0;
        }
        return STATUS_SUCCESS;
    }
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtFsControlFile(0x%X) - stub", FsControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtDeviceIoControlFile(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PXBOX_IO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength)
{
    (void)FileHandle; (void)Event; (void)ApcRoutine; (void)ApcContext;
    (void)InputBuffer; (void)InputBufferLength; (void)OutputBuffer; (void)OutputBufferLength;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE, "NtDeviceIoControlFile(0x%X) - stub", IoControlCode);
    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_NOT_IMPLEMENTED;
        IoStatusBlock->Information = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS __stdcall xbox_NtOpenSymbolicLinkObject(
    PHANDLE LinkHandle, PXBOX_OBJECT_ATTRIBUTES ObjectAttributes)
{
    /*
     * Xbox uses symbolic links for drive-letter mapping (D: -> \Device\CdRom0).
     * Path translation handles this transparently, so return a dummy handle.
     */
    if (LinkHandle)
        *LinkHandle = (HANDLE)(ULONG_PTR)0xDEAD0001;
    XBOX_TRACE(XBOX_LOG_FILE, "NtOpenSymbolicLinkObject(%s) - stub",
        get_xbox_path(ObjectAttributes) ? get_xbox_path(ObjectAttributes) : "?");
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_NtQuerySymbolicLinkObject(
    HANDLE LinkHandle, PXBOX_ANSI_STRING LinkTarget, PULONG ReturnedLength)
{
    (void)LinkHandle;
    const char* target = "\\Device\\CdRom0";
    if (LinkTarget && LinkTarget->Buffer) {
        USHORT len = (USHORT)strlen(target);
        if (len < LinkTarget->MaximumLength) {
            memcpy(LinkTarget->Buffer, target, len + 1);
            LinkTarget->Length = len;
        }
    }
    if (ReturnedLength)
        *ReturnedLength = (ULONG)strlen(target);
    return STATUS_SUCCESS;
}
