/*
 * kernel_ob.c - Object Manager
 *
 * Implements object reference counting and handle-to-object resolution.
 *
 * The Xbox kernel uses a simplified NT object manager. Objects have reference
 * counts and can be looked up by handle or name. Since we use Win32 HANDLEs
 * directly, most of these are thin wrappers.
 *
 * ObfReferenceObject/ObfDereferenceObject use __fastcall on Xbox (parameter
 * in ECX register).
 */

#include "kernel.h"

/* ============================================================================
 * Type Object Pointers
 *
 * Xbox kernel exports type objects for common types. Game code uses these
 * with ObReferenceObjectByHandle to validate handle types.
 * Native entry points translate these identities to the same guest type exports.
 * ============================================================================ */

static ULONG g_thread_object_type_data = 0x54485244; /* 'THRD' */
static ULONG g_event_object_type_data  = 0x45564E54; /* 'EVNT' */

PVOID xbox_PsThreadObjectType = &g_thread_object_type_data;
PVOID xbox_ExEventObjectType  = &g_event_object_type_data;

/* Name lookup and reference lifetime share the bridge object identity table. */
