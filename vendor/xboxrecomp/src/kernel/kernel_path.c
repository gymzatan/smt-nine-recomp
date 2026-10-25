/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* A project may set this to watch every guest path the kernel translates (for
 * example to print who opened a file). NULL by default. */
void (*g_xbox_path_hook)(const char *xbox_path) = NULL;

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 0 = game, 1 = runtime, 2 = Partition1, 3 = U: */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    { "\\Device\\CdRom0",                     0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    2, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1",      2, NULL,         NULL          },
    /* Xbox volume paths use ordinary PC directories. Root system-volume
     * access uses SystemData; paths below it retain the game asset mapping. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 3, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/* Physical device identities are drawn from this same path table. The rule
 * index is retained in the existing object record, including across snapshots. */
unsigned xbox_path_rule_count(void) {return PATH_RULE_COUNT;}
int xbox_path_device_rule(unsigned rule,char *name,DWORD capacity,ULONG *type) {
    if(!rule || rule>PATH_RULE_COUNT)return 0;
    const char *prefix=s_rules[rule-1].prefix;
    if(!match_prefix(prefix,"\\Device\\"))return 0;
    size_t length=strlen(prefix);if(prefix[length-1]=='\\')--length;
    for(unsigned i=0;i+1<rule;i++) {
        size_t earlier=strlen(s_rules[i].prefix);
        if(earlier && s_rules[i].prefix[earlier-1]=='\\')--earlier;
        if(earlier==length) {
            size_t j=0;while(j<length && tolower((unsigned char)prefix[j])==tolower((unsigned char)s_rules[i].prefix[j]))++j;
            if(j==length)return 0;
        }
    }
    if(name) {if(length>=capacity)return 0;memcpy(name,prefix,length);name[length]=0;}
    if(type)*type=match_prefix(prefix,"\\Device\\CdRom")?2:7;
    return 1;
}
#if defined(_WIN32)
static BOOL xbox_translate_path_impl(const char*,xbox_host_char*,DWORD);
#endif
BOOL xbox_translate_device_path(unsigned rule,const char *rest,xbox_host_char *output,DWORD capacity) {
    char name[64];if(!rest || !xbox_path_device_rule(rule,name,sizeof name,NULL))return FALSE;
    size_t head=strlen(name),tail=strlen(rest);
    if(head+tail+1>UINT16_MAX)return FALSE;
    char *path=malloc(head+tail+2);if(!path)return FALSE;
    memcpy(path,name,head);if(tail)path[head++]='\\';memcpy(path+head,rest,tail+1);
#if defined(_WIN32)
    BOOL ok=xbox_translate_path_impl(path,output,capacity);
#else
    BOOL ok=xbox_translate_path(path,output,capacity);
#endif
    free(path);return ok;
}

/* Drive spellings share the same link identity in the existing I/O map. */
static int split_symlink_path(const char *path,char link[7],const char **rest) {
    if(!path)return 0;
    if(match_prefix(path,"\\??\\") && path[4] && path[5]==':')path+=4;
    if(!path[0] || path[1]!=':')return 0;
    memcpy(link,"\\??\\",4);link[4]=(char)toupper((unsigned char)path[0]);link[5]=':';link[6]=0;
    *rest=path+2;while(**rest=='\\' || **rest=='/')++*rest;return 1;
}

#if !defined(_WIN32)
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if(!out || !out_size || !split_symlink_path(xbox_path,link,&rest))return 0;

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

#endif

/* ======================================================================== */
/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. The Win32 backend fills this during translation; the POSIX one
 * does not yet, so it reads empty there. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[MAX_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static WCHAR s_user_dir[MAX_PATH];
static WCHAR s_partition1_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;
static char s_language_cache[80],s_language_title[80];
static WCHAR s_language_media_root[MAX_PATH];
void xbox_path_set_language_media(const char *language,const char *media_root)
{
    s_language_media_root[0]=0;
    s_language_cache[0]=s_language_title[0]=0;
    if(language[0]) {
        MultiByteToWideChar(CP_UTF8,0,media_root,-1,s_language_media_root,MAX_PATH);
        sprintf_s(s_language_cache,sizeof s_language_cache,"\\Cache%s",language);
        sprintf_s(s_language_title,sizeof s_language_title,"\\TitleData%s",language);
        WCHAR path[MAX_PATH];
        swprintf_s(path,MAX_PATH,L"%s%hs",s_save_dir,s_language_cache);CreateDirectoryW(path,NULL);
        swprintf_s(path,MAX_PATH,L"%s%hs",s_save_dir,s_language_title);CreateDirectoryW(path,NULL);
    }
}
BOOL xbox_path_set_user_directory(const char *directory)
{
    if(!s_initialized||!directory||!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,directory,-1,s_user_dir,MAX_PATH))return FALSE;
    wcscpy_s(s_partition1_dir,MAX_PATH,s_user_dir);return TRUE;
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';
    wcscpy_s(s_partition1_dir,MAX_PATH,s_game_dir);

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* SHCreateDirectoryExW requires an absolute save-directory path. */
    {
        WCHAR full[MAX_PATH];
        if (GetFullPathNameW(s_save_dir, MAX_PATH, full, NULL))
            wcscpy_s(s_save_dir, MAX_PATH, full);
    }

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
    }

    s_initialized = TRUE;
    wcscpy_s(s_user_dir,MAX_PATH,s_save_dir);
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path_shared, MAX_PATH, p, _TRUNCATE);
}



static BOOL xbox_translate_path_impl(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char link[7];const char *rest;
        if(split_symlink_path(xbox_path,link,&rest)) {
            int resolved=xbox_SymbolicLinkPath(link,rest,host_path_buf,buf_size);
            if(resolved<0)return FALSE;
            if(resolved) {
                return TRUE;
            }
        }
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if(skip && s_rules[i].prefix[skip-1]!='\\' && xbox_path[skip] && xbox_path[skip]!='\\' && xbox_path[skip]!='/')skip=0;
        if (skip) {
            remainder = xbox_path + skip;
            const WCHAR *roots[]={s_game_dir,s_save_dir,s_partition1_dir,s_user_dir};
            base_dir=roots[s_rules[i].to_save];
            sub_dir   = s_rules[i].sub_win;
            if(s_language_media_root[0] && base_dir==s_game_dir && _strnicmp(remainder,"Media\\",6)==0)base_dir=s_language_media_root;
            if(s_language_cache[0] && sub_dir) {
                if(!strcmp(sub_dir,"\\Cache"))sub_dir=s_language_cache;
                else if(!strcmp(sub_dir,"\\TitleData"))sub_dir=s_language_title;
            }
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    xbox_log(XBOX_LOG_TRACE, XBOX_LOG_PATH, "%s", xbox_path);
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
BOOL xbox_translate_path(const char *path,xbox_host_char *output,DWORD capacity) {
    if(!xbox_translate_path_impl(path,output,capacity))return FALSE;
    if(g_xbox_path_hook)g_xbox_path_hook(path);
    xbox_remember_host_path(output);return TRUE;
}

#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static char s_user_dir[MAX_PATH];
static char s_partition1_dir[MAX_PATH];
static BOOL s_initialized = FALSE;
BOOL xbox_path_set_user_directory(const char *directory)
{
    if(!s_initialized||!directory||strlen(directory)>=sizeof s_user_dir)return FALSE;
    strcpy(s_user_dir,directory);strcpy(s_partition1_dir,directory);return TRUE;
}

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);
    strcpy(s_partition1_dir,s_game_dir);strcpy(s_user_dir,s_save_dir);

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if(skip && s_rules[i].prefix[skip-1]!='\\' && xbox_path[skip] && xbox_path[skip]!='\\' && xbox_path[skip]!='/')skip=0;
        if (skip) {
            remainder = xbox_path + skip;
            const char *roots[]={s_game_dir,s_save_dir,s_partition1_dir,s_user_dir};
            base_dir=roots[s_rules[i].to_save];
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
