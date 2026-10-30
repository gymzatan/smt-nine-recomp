#ifndef NINE_ERROR_LOG_H
#define NINE_ERROR_LOG_H
#include <windows.h>

typedef void (*NineErrorContext)(EXCEPTION_POINTERS *exception);
int nine_error_log_init(const wchar_t *root, NineErrorContext context);
void nine_error_log_fatal(const char *reason, unsigned code);
void nine_error_log_exception(const char *reason, EXCEPTION_POINTERS *exception);
int nine_error_log_finish(int result);
#endif
