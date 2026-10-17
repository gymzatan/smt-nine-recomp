#ifndef SMT9_LANGUAGE_H
#define SMT9_LANGUAGE_H
#include <windows.h>

#define NINE_LANGUAGE_ID 32
typedef struct NineLanguage {
    wchar_t id[NINE_LANGUAGE_ID], name[80];
    wchar_t directory[MAX_PATH], image[MAX_PATH], executable[MAX_PATH];
    unsigned font_pages;
    int external;
} NineLanguage;
int nine_language_id_valid(const wchar_t *id);
int nine_language_resolve(const wchar_t *id,NineLanguage *language,wchar_t *error,size_t count);
unsigned nine_language_list(NineLanguage *languages,unsigned capacity);
/* Return 1 in the matching native build, 0 after launching it, -1 on error. */
int nine_language_enter(const NineLanguage *language,wchar_t *error,size_t count);
#endif
