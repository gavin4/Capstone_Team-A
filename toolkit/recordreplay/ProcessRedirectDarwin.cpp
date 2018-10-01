/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ProcessRedirect.h"

#include "mozilla/Maybe.h"

#include "HashTable.h"
#include "Lock.h"
#include "MemorySnapshot.h"
#include "ProcessRecordReplay.h"
#include "ProcessRewind.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>

#include <bsm/audit.h>
#include <bsm/audit_session.h>
#include <mach/clock.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/vm_map.h>
#include <sys/attr.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include <Carbon/Carbon.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <objc/objc-runtime.h>

namespace mozilla {
namespace recordreplay {

// Specify every function that is being redirected. MACRO is invoked with the
// function's name, followed by any hooks associated with the redirection for
// saving its output or adding a preamble.
#define FOR_EACH_REDIRECTION(MACRO)                              \
  /* System call wrappers */                                     \
  MACRO(kevent, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<3, 4, struct kevent>>) \
  MACRO(kevent64, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<3, 4, struct kevent64_s>>) \
  MACRO(mprotect, nullptr, Preamble_mprotect)                    \
  MACRO(mmap, nullptr, Preamble_mmap)                            \
  MACRO(munmap, nullptr, Preamble_munmap)                        \
  MACRO(read, RR_SaveRvalHadErrorNegative<RR_WriteBufferViaRval<1, 2>>) \
  MACRO(__read_nocancel, RR_SaveRvalHadErrorNegative<RR_WriteBufferViaRval<1, 2>>) \
  MACRO(pread, RR_SaveRvalHadErrorNegative<RR_WriteBufferViaRval<1, 2>>) \
  MACRO(write, RR_SaveRvalHadErrorNegative)                      \
  MACRO(__write_nocancel, RR_SaveRvalHadErrorNegative)           \
  MACRO(open, RR_SaveRvalHadErrorNegative)                       \
  MACRO(__open_nocancel, RR_SaveRvalHadErrorNegative)            \
  MACRO(recv, RR_SaveRvalHadErrorNegative<RR_WriteBufferViaRval<1, 2>>) \
  MACRO(recvmsg, RR_SaveRvalHadErrorNegative<RR_recvmsg>)        \
  MACRO(sendmsg, RR_SaveRvalHadErrorNegative)                    \
  MACRO(shm_open, RR_SaveRvalHadErrorNegative)                   \
  MACRO(socket, RR_SaveRvalHadErrorNegative)                     \
  MACRO(kqueue, RR_SaveRvalHadErrorNegative)                     \
  MACRO(pipe, RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<0, 2 * sizeof(int)>>) \
  MACRO(close, RR_SaveRvalHadErrorNegative)                      \
  MACRO(__close_nocancel, RR_SaveRvalHadErrorNegative)           \
  MACRO(mkdir, RR_SaveRvalHadErrorNegative)                      \
  MACRO(dup, RR_SaveRvalHadErrorNegative)                        \
  MACRO(access, RR_SaveRvalHadErrorNegative)                     \
  MACRO(lseek, RR_SaveRvalHadErrorNegative)                      \
  MACRO(socketpair, RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<3, 2 * sizeof(int)>>) \
  MACRO(fileport_makeport,                                       \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(size_t)>>) \
  MACRO(getsockopt, RR_SaveRvalHadErrorNegative<RR_getsockopt>)  \
  MACRO(gettimeofday, RR_SaveRvalHadErrorNegative<RR_Compose<    \
                        RR_WriteOptionalBufferFixedSize<0, sizeof(struct timeval)>, \
                        RR_WriteOptionalBufferFixedSize<1, sizeof(struct timezone)>>>, \
                      Preamble_gettimeofday)                     \
  MACRO(getuid, RR_ScalarRval)                                   \
  MACRO(geteuid, RR_ScalarRval)                                  \
  MACRO(getgid, RR_ScalarRval)                                   \
  MACRO(getegid, RR_ScalarRval)                                  \
  MACRO(issetugid, RR_ScalarRval)                                \
  MACRO(__gettid, RR_ScalarRval)                                 \
  MACRO(getpid, RR_ScalarRval)                                   \
  MACRO(fcntl, RR_SaveRvalHadErrorNegative, Preamble_fcntl)      \
  MACRO(getattrlist, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<2, 3>>) \
  MACRO(fstat$INODE64,                                           \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct stat)>>) \
  MACRO(lstat$INODE64,                                           \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct stat)>>) \
  MACRO(stat$INODE64,                                            \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct stat)>>) \
  MACRO(statfs$INODE64,                                          \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct statfs)>>) \
  MACRO(fstatfs$INODE64,                                         \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct statfs)>>) \
  MACRO(readlink, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<1, 2>>) \
  MACRO(__getdirentries64, RR_SaveRvalHadErrorNegative<RR_Compose< \
                             RR_WriteBuffer<1, 2>,               \
                             RR_WriteBufferFixedSize<3, sizeof(size_t)>>>) \
  MACRO(getdirentriesattr, RR_SaveRvalHadErrorNegative<RR_Compose< \
                             RR_WriteBufferFixedSize<1, sizeof(struct attrlist)>, \
                             RR_WriteBuffer<2, 3>,               \
                             RR_WriteBufferFixedSize<4, sizeof(size_t)>, \
                             RR_WriteBufferFixedSize<5, sizeof(size_t)>, \
                             RR_WriteBufferFixedSize<6, sizeof(size_t)>>>) \
  MACRO(getrusage,                                               \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct rusage)>>) \
  MACRO(__getrlimit,                                             \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<1, sizeof(struct rlimit)>>) \
  MACRO(__setrlimit, RR_SaveRvalHadErrorNegative)                \
  MACRO(sigprocmask,                                             \
        RR_SaveRvalHadErrorNegative<RR_WriteOptionalBufferFixedSize<2, sizeof(sigset_t)>>) \
  MACRO(sigaltstack,                                             \
        RR_SaveRvalHadErrorNegative<RR_WriteOptionalBufferFixedSize<2, sizeof(stack_t)>>) \
  MACRO(sigaction,                                               \
        RR_SaveRvalHadErrorNegative<RR_WriteOptionalBufferFixedSize<2, sizeof(struct sigaction)>>) \
  MACRO(__pthread_sigmask,                                       \
        RR_SaveRvalHadErrorNegative<RR_WriteOptionalBufferFixedSize<2, sizeof(sigset_t)>>) \
  MACRO(__fsgetpath, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<0, 1>>) \
  MACRO(__disable_threadsignal, nullptr, Preamble___disable_threadsignal) \
  MACRO(__sysctl, RR_SaveRvalHadErrorNegative<RR___sysctl>)      \
  MACRO(__mac_syscall, RR_SaveRvalHadErrorNegative)              \
  MACRO(getaudit_addr,                                           \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<0, sizeof(auditinfo_addr_t)>>) \
  MACRO(umask, RR_ScalarRval)                                    \
  MACRO(__select, RR_SaveRvalHadErrorNegative<RR_Compose<        \
                    RR_WriteBufferFixedSize<1, sizeof(fd_set)>,  \
                    RR_WriteBufferFixedSize<2, sizeof(fd_set)>,  \
                    RR_WriteBufferFixedSize<3, sizeof(fd_set)>,  \
                    RR_WriteOptionalBufferFixedSize<4, sizeof(timeval)>>>) \
  MACRO(__process_policy, RR_SaveRvalHadErrorNegative)           \
  MACRO(__kdebug_trace, RR_SaveRvalHadErrorNegative)             \
  MACRO(guarded_kqueue_np,                                       \
        RR_SaveRvalHadErrorNegative<RR_WriteBufferFixedSize<0, sizeof(size_t)>>) \
  MACRO(csops, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<2, 3>>) \
  MACRO(__getlogin, RR_SaveRvalHadErrorNegative<RR_WriteBuffer<0, 1>>) \
  MACRO(__workq_kernreturn, nullptr, Preamble___workq_kernreturn) \
  MACRO(start_wqthread, nullptr, Preamble_start_wqthread)        \
  /* pthreads interface functions */                             \
  MACRO(pthread_cond_wait, nullptr, Preamble_pthread_cond_wait)  \
  MACRO(pthread_cond_timedwait, nullptr, Preamble_pthread_cond_timedwait) \
  MACRO(pthread_cond_timedwait_relative_np, nullptr, Preamble_pthread_cond_timedwait_relative_np) \
  MACRO(pthread_create, nullptr, Preamble_pthread_create)        \
  MACRO(pthread_join, nullptr, Preamble_pthread_join)            \
  MACRO(pthread_mutex_init, nullptr, Preamble_pthread_mutex_init) \
  MACRO(pthread_mutex_destroy, nullptr, Preamble_pthread_mutex_destroy) \
  MACRO(pthread_mutex_lock, nullptr, Preamble_pthread_mutex_lock) \
  MACRO(pthread_mutex_trylock, nullptr, Preamble_pthread_mutex_trylock) \
  MACRO(pthread_mutex_unlock, nullptr, Preamble_pthread_mutex_unlock) \
  /* C Library functions */                                      \
  MACRO(dlclose, nullptr, Preamble_Veto<0>)                      \
  MACRO(dlopen, nullptr, Preamble_PassThrough)                   \
  MACRO(dlsym, nullptr, Preamble_PassThrough)                    \
  MACRO(fclose, RR_SaveRvalHadErrorNegative)                     \
  MACRO(fopen, RR_SaveRvalHadErrorZero)                          \
  MACRO(fread, RR_Compose<RR_ScalarRval, RR_fread>)              \
  MACRO(fseek, RR_SaveRvalHadErrorNegative)                      \
  MACRO(ftell, RR_SaveRvalHadErrorNegative)                      \
  MACRO(fwrite, RR_ScalarRval)                                   \
  MACRO(getenv, RR_CStringRval)                                  \
  MACRO(localtime_r, RR_SaveRvalHadErrorZero<RR_Compose<         \
                       RR_WriteBufferFixedSize<1, sizeof(struct tm)>, \
                       RR_RvalIsArgument<1>>>)                   \
  MACRO(gmtime_r, RR_SaveRvalHadErrorZero<RR_Compose<            \
                    RR_WriteBufferFixedSize<1, sizeof(struct tm)>, \
                    RR_RvalIsArgument<1>>>)                      \
  MACRO(localtime, nullptr, Preamble_localtime)                  \
  MACRO(gmtime, nullptr, Preamble_gmtime)                        \
  MACRO(mktime, RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<0, sizeof(struct tm)>>) \
  MACRO(setlocale, RR_CStringRval)                               \
  MACRO(strftime, RR_Compose<RR_ScalarRval, RR_WriteBufferViaRval<0, 1, 1>>) \
  MACRO(arc4random, RR_ScalarRval)                               \
  MACRO(mach_absolute_time, RR_ScalarRval, Preamble_mach_absolute_time) \
  MACRO(mach_msg, RR_Compose<RR_OrderCall, RR_ScalarRval, RR_WriteBuffer<0, 3>>) \
  MACRO(mach_timebase_info,                                      \
        RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<0, sizeof(mach_timebase_info_data_t)>>) \
  MACRO(mach_vm_allocate, nullptr, Preamble_mach_vm_allocate)    \
  MACRO(mach_vm_deallocate, nullptr, Preamble_mach_vm_deallocate) \
  MACRO(mach_vm_protect, nullptr, Preamble_mach_vm_protect)      \
  /* By passing through events when initializing the sandbox, we ensure both */ \
  /* that we actually initialize the process sandbox while replaying as well as */ \
  /* while recording, and that any activity in these calls does not interfere */ \
  /* with the replay. */                                         \
  MACRO(sandbox_init, nullptr, Preamble_PassThrough)             \
  MACRO(sandbox_init_with_parameters, nullptr, Preamble_PassThrough) \
  /* Make sure events are passed through here so that replaying processes can */ \
  /* inspect their own threads. */                               \
  MACRO(task_threads, nullptr, Preamble_PassThrough)             \
  MACRO(vm_copy, nullptr, Preamble_vm_copy)                      \
  MACRO(vm_purgable_control, nullptr, Preamble_vm_purgable_control) \
  MACRO(tzset)                                                   \
  /* NSPR functions */                                           \
  MACRO(PL_NewHashTable, nullptr, Preamble_PL_NewHashTable)      \
  MACRO(PL_HashTableDestroy, nullptr, Preamble_PL_HashTableDestroy) \
  /* Objective C functions */                                    \
  MACRO(class_getClassMethod, RR_ScalarRval)                     \
  MACRO(class_getInstanceMethod, RR_ScalarRval)                  \
  MACRO(method_exchangeImplementations)                          \
  MACRO(objc_autoreleasePoolPop)                                 \
  MACRO(objc_autoreleasePoolPush, RR_ScalarRval)                 \
  MACRO(objc_msgSend, nullptr, Preamble_objc_msgSend)            \
  /* Cocoa functions */                                          \
  MACRO(AcquireFirstMatchingEventInQueue, RR_ScalarRval)         \
  MACRO(CFArrayAppendValue)                                      \
  MACRO(CFArrayCreate, RR_ScalarRval)                            \
  MACRO(CFArrayGetCount, RR_ScalarRval)                          \
  MACRO(CFArrayGetValueAtIndex, RR_ScalarRval)                   \
  MACRO(CFArrayRemoveValueAtIndex)                               \
  MACRO(CFAttributedStringCreate, RR_ScalarRval)                 \
  MACRO(CFBundleCopyExecutableURL, RR_ScalarRval)                \
  MACRO(CFBundleCopyInfoDictionaryForURL, RR_ScalarRval)         \
  MACRO(CFBundleCreate, RR_ScalarRval)                           \
  MACRO(CFBundleGetBundleWithIdentifier, RR_ScalarRval)          \
  MACRO(CFBundleGetDataPointerForName, nullptr, Preamble_VetoIfNotPassedThrough<0>) \
  MACRO(CFBundleGetFunctionPointerForName, nullptr, Preamble_VetoIfNotPassedThrough<0>) \
  MACRO(CFBundleGetIdentifier, RR_ScalarRval)                    \
  MACRO(CFBundleGetInfoDictionary, RR_ScalarRval)                \
  MACRO(CFBundleGetMainBundle, RR_ScalarRval)                    \
  MACRO(CFBundleGetValueForInfoDictionaryKey, RR_ScalarRval)     \
  MACRO(CFDataGetBytePtr, RR_CFDataGetBytePtr)                   \
  MACRO(CFDataGetLength, RR_ScalarRval)                          \
  MACRO(CFDateFormatterCreate, RR_ScalarRval)                    \
  MACRO(CFDateFormatterGetFormat, RR_ScalarRval)                 \
  MACRO(CFDictionaryAddValue)                                    \
  MACRO(CFDictionaryCreate, RR_ScalarRval)                       \
  MACRO(CFDictionaryCreateMutable, RR_ScalarRval)                \
  MACRO(CFDictionaryCreateMutableCopy, RR_ScalarRval)            \
  MACRO(CFDictionaryGetValue, RR_ScalarRval)                     \
  MACRO(CFDictionaryGetValueIfPresent,                           \
        RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<2, sizeof(const void*)>>) \
  MACRO(CFDictionaryReplaceValue)                                \
  MACRO(CFEqual, RR_ScalarRval)                                  \
  MACRO(CFGetTypeID, RR_ScalarRval)                              \
  MACRO(CFLocaleCopyCurrent, RR_ScalarRval)                      \
  MACRO(CFLocaleCopyPreferredLanguages, RR_ScalarRval)           \
  MACRO(CFLocaleCreate, RR_ScalarRval)                           \
  MACRO(CFLocaleGetIdentifier, RR_ScalarRval)                    \
  MACRO(CFNotificationCenterAddObserver, nullptr, Preamble_CFNotificationCenterAddObserver) \
  MACRO(CFNotificationCenterGetLocalCenter, RR_ScalarRval)       \
  MACRO(CFNotificationCenterRemoveObserver)                      \
  MACRO(CFNumberCreate, RR_ScalarRval)                           \
  MACRO(CFNumberGetValue, RR_Compose<RR_ScalarRval, RR_CFNumberGetValue>) \
  MACRO(CFNumberIsFloatType, RR_ScalarRval)                      \
  MACRO(CFPreferencesAppValueIsForced, RR_ScalarRval)            \
  MACRO(CFPreferencesCopyAppValue, RR_ScalarRval)                \
  MACRO(CFPreferencesCopyValue, RR_ScalarRval)                   \
  MACRO(CFPropertyListCreateFromXMLData, RR_ScalarRval)          \
  MACRO(CFPropertyListCreateWithStream, RR_ScalarRval)           \
  MACRO(CFReadStreamClose)                                       \
  MACRO(CFReadStreamCreateWithFile, RR_ScalarRval)               \
  MACRO(CFReadStreamOpen, RR_ScalarRval)                         \
  MACRO(CFRelease, RR_ScalarRval)                                \
  MACRO(CFRetain, RR_ScalarRval)                                 \
  MACRO(CFRunLoopAddSource)                                      \
  MACRO(CFRunLoopGetCurrent, RR_ScalarRval)                      \
  MACRO(CFRunLoopRemoveSource)                                   \
  MACRO(CFRunLoopSourceCreate, RR_ScalarRval, Preamble_CFRunLoopSourceCreate) \
  MACRO(CFRunLoopSourceSignal)                                   \
  MACRO(CFRunLoopWakeUp)                                         \
  MACRO(CFStringAppendCharacters)                                \
  MACRO(CFStringCompare, RR_ScalarRval)                          \
  MACRO(CFStringCreateArrayBySeparatingStrings, RR_ScalarRval)   \
  MACRO(CFStringCreateMutable, RR_ScalarRval)                    \
  MACRO(CFStringCreateWithBytes, RR_ScalarRval)                  \
  MACRO(CFStringCreateWithBytesNoCopy, RR_ScalarRval)            \
  MACRO(CFStringCreateWithCharactersNoCopy, RR_ScalarRval)       \
  MACRO(CFStringCreateWithCString, RR_ScalarRval)                \
  MACRO(CFStringCreateWithFormat, RR_ScalarRval)                 \
  /* Argument indexes are off by one here as the CFRange argument uses two slots. */ \
  MACRO(CFStringGetBytes, RR_Compose<                            \
                            RR_ScalarRval,                       \
                            RR_WriteOptionalBuffer<6, 7>,        \
                            RR_WriteOptionalBufferFixedSize<8, sizeof(CFIndex)>>) \
  /* Argument indexes are off by one here as the CFRange argument uses two slots. */ \
  /* We also need to specify the argument register with the range's length here. */ \
  MACRO(CFStringGetCharacters, RR_WriteBuffer<3, 2, UniChar>)    \
  MACRO(CFStringGetCString, RR_Compose<RR_ScalarRval, RR_WriteBuffer<1, 2>>) \
  MACRO(CFStringGetCStringPtr, nullptr, Preamble_VetoIfNotPassedThrough<0>) \
  MACRO(CFStringGetIntValue, RR_ScalarRval)                      \
  MACRO(CFStringGetLength, RR_ScalarRval)                        \
  MACRO(CFStringGetMaximumSizeForEncoding, RR_ScalarRval)        \
  MACRO(CFStringHasPrefix, RR_ScalarRval)                        \
  MACRO(CFStringTokenizerAdvanceToNextToken, RR_ScalarRval)      \
  MACRO(CFStringTokenizerCreate, RR_ScalarRval)                  \
  MACRO(CFStringTokenizerGetCurrentTokenRange, RR_ComplexScalarRval) \
  MACRO(CFURLCreateFromFileSystemRepresentation, RR_ScalarRval)  \
  MACRO(CFURLCreateFromFSRef, RR_ScalarRval)                     \
  MACRO(CFURLCreateWithFileSystemPath, RR_ScalarRval)            \
  MACRO(CFURLCreateWithString, RR_ScalarRval)                    \
  MACRO(CFURLGetFileSystemRepresentation, RR_Compose<RR_ScalarRval, RR_WriteBuffer<2, 3>>) \
  MACRO(CFURLGetFSRef, RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<1, sizeof(FSRef)>>) \
  MACRO(CFUUIDCreate, RR_ScalarRval)                             \
  MACRO(CFUUIDCreateString, RR_ScalarRval)                       \
  MACRO(CFUUIDGetUUIDBytes, RR_ComplexScalarRval)                \
  MACRO(CGAffineTransformConcat, RR_OversizeRval<sizeof(CGAffineTransform)>) \
  MACRO(CGBitmapContextCreateImage, RR_ScalarRval)               \
  MACRO(CGBitmapContextCreateWithData,                           \
        RR_Compose<RR_ScalarRval, RR_CGBitmapContextCreateWithData>) \
  MACRO(CGBitmapContextGetBytesPerRow, RR_ScalarRval)            \
  MACRO(CGBitmapContextGetHeight, RR_ScalarRval)                 \
  MACRO(CGBitmapContextGetWidth, RR_ScalarRval)                  \
  MACRO(CGColorRelease, RR_ScalarRval)                           \
  MACRO(CGColorSpaceCopyICCProfile, RR_ScalarRval)               \
  MACRO(CGColorSpaceCreateDeviceRGB, RR_ScalarRval)              \
  MACRO(CGColorSpaceCreatePattern, RR_ScalarRval)                \
  MACRO(CGColorSpaceRelease, RR_ScalarRval)                      \
  MACRO(CGContextBeginTransparencyLayerWithRect)                 \
  MACRO(CGContextClipToRects, RR_ScalarRval)                     \
  MACRO(CGContextConcatCTM)                                      \
  MACRO(CGContextDrawImage, RR_FlushCGContext<0>)                \
  MACRO(CGContextEndTransparencyLayer)                           \
  MACRO(CGContextFillRect, RR_FlushCGContext<0>)                 \
  MACRO(CGContextGetClipBoundingBox, RR_OversizeRval<sizeof(CGRect)>) \
  MACRO(CGContextGetCTM, RR_OversizeRval<sizeof(CGAffineTransform)>) \
  MACRO(CGContextGetType, RR_ScalarRval)                         \
  MACRO(CGContextGetUserSpaceToDeviceSpaceTransform, RR_OversizeRval<sizeof(CGAffineTransform)>) \
  MACRO(CGContextRestoreGState, nullptr, Preamble_CGContextRestoreGState) \
  MACRO(CGContextSaveGState)                                     \
  MACRO(CGContextSetAllowsFontSubpixelPositioning)               \
  MACRO(CGContextSetAllowsFontSubpixelQuantization)              \
  MACRO(CGContextSetAlpha)                                       \
  MACRO(CGContextSetBaseCTM)                                     \
  MACRO(CGContextSetCTM)                                         \
  MACRO(CGContextSetGrayFillColor)                               \
  MACRO(CGContextSetRGBFillColor)                                \
  MACRO(CGContextSetShouldAntialias)                             \
  MACRO(CGContextSetShouldSmoothFonts)                           \
  MACRO(CGContextSetShouldSubpixelPositionFonts)                 \
  MACRO(CGContextSetShouldSubpixelQuantizeFonts)                 \
  MACRO(CGContextSetTextDrawingMode)                             \
  MACRO(CGContextSetTextMatrix)                                  \
  MACRO(CGContextScaleCTM)                                       \
  MACRO(CGContextTranslateCTM)                                   \
  MACRO(CGDataProviderCreateWithData, RR_Compose<RR_ScalarRval, RR_CGDataProviderCreateWithData>) \
  MACRO(CGDataProviderRelease)                                   \
  MACRO(CGDisplayCopyColorSpace, RR_ScalarRval)                  \
  MACRO(CGDisplayIOServicePort, RR_ScalarRval)                   \
  MACRO(CGEventSourceCounterForEventType, RR_ScalarRval)         \
  MACRO(CGFontCopyTableForTag, RR_ScalarRval)                    \
  MACRO(CGFontCopyTableTags, RR_ScalarRval)                      \
  MACRO(CGFontCopyVariations, RR_ScalarRval)                     \
  MACRO(CGFontCreateCopyWithVariations, RR_ScalarRval)           \
  MACRO(CGFontCreateWithDataProvider, RR_ScalarRval)             \
  MACRO(CGFontCreateWithFontName, RR_ScalarRval)                 \
  MACRO(CGFontCreateWithPlatformFont, RR_ScalarRval)             \
  MACRO(CGFontGetAscent, RR_ScalarRval)                          \
  MACRO(CGFontGetCapHeight, RR_ScalarRval)                       \
  MACRO(CGFontGetDescent, RR_ScalarRval)                         \
  MACRO(CGFontGetFontBBox, RR_OversizeRval<sizeof(CGRect)>)      \
  MACRO(CGFontGetGlyphAdvances, RR_Compose<RR_ScalarRval, RR_WriteBuffer<3, 2, int>>) \
  MACRO(CGFontGetGlyphBBoxes, RR_Compose<RR_ScalarRval, RR_WriteBuffer<3, 2, CGRect>>) \
  MACRO(CGFontGetGlyphPath, RR_ScalarRval)                       \
  MACRO(CGFontGetLeading, RR_ScalarRval)                         \
  MACRO(CGFontGetUnitsPerEm, RR_ScalarRval)                      \
  MACRO(CGFontGetXHeight, RR_ScalarRval)                         \
  MACRO(CGImageGetHeight, RR_ScalarRval)                         \
  MACRO(CGImageGetWidth, RR_ScalarRval)                          \
  MACRO(CGImageRelease, RR_ScalarRval)                           \
  MACRO(CGMainDisplayID, RR_ScalarRval)                          \
  MACRO(CGPathAddPath)                                           \
  MACRO(CGPathApply, nullptr, Preamble_CGPathApply)              \
  MACRO(CGPathContainsPoint, RR_ScalarRval)                      \
  MACRO(CGPathCreateMutable, RR_ScalarRval)                      \
  MACRO(CGPathGetBoundingBox, RR_OversizeRval<sizeof(CGRect)>)   \
  MACRO(CGPathGetCurrentPoint, RR_ComplexFloatRval)              \
  MACRO(CGPathIsEmpty, RR_ScalarRval)                            \
  MACRO(CGSSetDebugOptions, RR_ScalarRval)                       \
  MACRO(CGSShutdownServerConnections)                            \
  MACRO(CTFontCopyFamilyName, RR_ScalarRval)                     \
  MACRO(CTFontCopyFeatures, RR_ScalarRval)                       \
  MACRO(CTFontCopyFontDescriptor, RR_ScalarRval)                 \
  MACRO(CTFontCopyGraphicsFont, RR_ScalarRval)                   \
  MACRO(CTFontCopyTable, RR_ScalarRval)                          \
  MACRO(CTFontCopyVariationAxes, RR_ScalarRval)                  \
  MACRO(CTFontCreateForString, RR_ScalarRval)                    \
  MACRO(CTFontCreatePathForGlyph, RR_ScalarRval)                 \
  MACRO(CTFontCreateWithFontDescriptor, RR_ScalarRval)           \
  MACRO(CTFontCreateWithGraphicsFont, RR_ScalarRval)             \
  MACRO(CTFontCreateWithName, RR_ScalarRval)                     \
  MACRO(CTFontDescriptorCopyAttribute, RR_ScalarRval)            \
  MACRO(CTFontDescriptorCreateCopyWithAttributes, RR_ScalarRval) \
  MACRO(CTFontDescriptorCreateMatchingFontDescriptors, RR_ScalarRval) \
  MACRO(CTFontDescriptorCreateWithAttributes, RR_ScalarRval)     \
  MACRO(CTFontDrawGlyphs, RR_FlushCGContext<4>)                  \
  MACRO(CTFontGetAdvancesForGlyphs,                              \
        RR_Compose<RR_FloatRval, RR_WriteOptionalBuffer<3, 4, CGSize>>) \
  MACRO(CTFontGetAscent, RR_FloatRval)                           \
  MACRO(CTFontGetBoundingBox, RR_OversizeRval<sizeof(CGRect)>)   \
  MACRO(CTFontGetBoundingRectsForGlyphs,                         \
        /* Argument indexes here are off by one due to the oversize rval. */ \
        RR_Compose<RR_OversizeRval<sizeof(CGRect)>, RR_WriteOptionalBuffer<4, 5, CGRect>>) \
  MACRO(CTFontGetCapHeight, RR_FloatRval)                        \
  MACRO(CTFontGetDescent, RR_FloatRval)                          \
  MACRO(CTFontGetGlyphCount, RR_ScalarRval)                      \
  MACRO(CTFontGetGlyphsForCharacters,                            \
        RR_Compose<RR_ScalarRval, RR_WriteBuffer<2, 3, CGGlyph>>) \
  MACRO(CTFontGetLeading, RR_FloatRval)                          \
  MACRO(CTFontGetSize, RR_FloatRval)                             \
  MACRO(CTFontGetSymbolicTraits, RR_ScalarRval)                  \
  MACRO(CTFontGetUnderlinePosition, RR_FloatRval)                \
  MACRO(CTFontGetUnderlineThickness, RR_FloatRval)               \
  MACRO(CTFontGetUnitsPerEm, RR_ScalarRval)                      \
  MACRO(CTFontGetXHeight, RR_FloatRval)                           \
  MACRO(CTFontManagerCopyAvailableFontFamilyNames, RR_ScalarRval) \
  MACRO(CTFontManagerRegisterFontsForURLs, RR_ScalarRval)        \
  MACRO(CTFontManagerSetAutoActivationSetting)                   \
  MACRO(CTLineCreateWithAttributedString, RR_ScalarRval)         \
  MACRO(CTLineGetGlyphRuns, RR_ScalarRval)                       \
  MACRO(CTRunGetAttributes, RR_ScalarRval)                       \
  MACRO(CTRunGetGlyphCount, RR_ScalarRval)                       \
  MACRO(CTRunGetGlyphsPtr, RR_CTRunGetElements<CGGlyph, CTRunGetGlyphs>) \
  MACRO(CTRunGetPositionsPtr, RR_CTRunGetElements<CGPoint, CTRunGetPositions>) \
  MACRO(CTRunGetStringIndicesPtr, RR_CTRunGetElements<CFIndex, CTRunGetStringIndices>) \
  MACRO(CTRunGetStringRange, RR_ComplexScalarRval)               \
  /* Argument indexes are off by one here as the CFRange argument uses two slots. */ \
  MACRO(CTRunGetTypographicBounds, RR_Compose<                   \
                                     RR_FloatRval,               \
                                     RR_WriteOptionalBufferFixedSize<3, sizeof(CGFloat)>, \
                                     RR_WriteOptionalBufferFixedSize<4, sizeof(CGFloat)>, \
                                     RR_WriteOptionalBufferFixedSize<5, sizeof(CGFloat)>>) \
  MACRO(CUIDraw)                                                 \
  MACRO(FSCompareFSRefs, RR_ScalarRval)                          \
  MACRO(FSGetVolumeInfo, RR_Compose<                             \
                           RR_ScalarRval,                        \
                           RR_WriteBufferFixedSize<5, sizeof(HFSUniStr255)>, \
                           RR_WriteBufferFixedSize<6, sizeof(FSRef)>>) \
  MACRO(FSFindFolder, RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<3, sizeof(FSRef)>>) \
  MACRO(Gestalt, RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<1, sizeof(SInt32)>>) \
  MACRO(GetEventClass, RR_ScalarRval)                            \
  MACRO(GetCurrentEventQueue, RR_ScalarRval)                     \
  MACRO(GetCurrentProcess,                                       \
        RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<0, sizeof(ProcessSerialNumber)>>) \
  MACRO(GetEventAttributes, RR_ScalarRval)                       \
  MACRO(GetEventDispatcherTarget, RR_ScalarRval)                 \
  MACRO(GetEventKind, RR_ScalarRval)                             \
  MACRO(HIThemeDrawButton, RR_ScalarRval)                        \
  MACRO(HIThemeDrawFrame, RR_ScalarRval)                         \
  MACRO(HIThemeDrawGroupBox, RR_ScalarRval)                      \
  MACRO(HIThemeDrawGrowBox, RR_ScalarRval)                       \
  MACRO(HIThemeDrawMenuBackground, RR_ScalarRval)                \
  MACRO(HIThemeDrawMenuItem, RR_ScalarRval)                      \
  MACRO(HIThemeDrawMenuSeparator, RR_ScalarRval)                 \
  MACRO(HIThemeDrawSeparator, RR_ScalarRval)                     \
  MACRO(HIThemeDrawTabPane, RR_ScalarRval)                       \
  MACRO(HIThemeDrawTrack, RR_ScalarRval)                         \
  MACRO(HIThemeGetGrowBoxBounds,                                 \
        RR_Compose<RR_ScalarRval, RR_WriteBufferFixedSize<2, sizeof(HIRect)>>) \
  MACRO(HIThemeSetFill, RR_ScalarRval)                           \
  MACRO(IORegistryEntrySearchCFProperty, RR_ScalarRval)          \
  MACRO(LSCopyAllHandlersForURLScheme, RR_ScalarRval)            \
  MACRO(LSCopyApplicationForMIMEType,                            \
        RR_Compose<RR_ScalarRval, RR_WriteOptionalBufferFixedSize<2, sizeof(CFURLRef)>>) \
  MACRO(LSCopyItemAttribute,                                     \
        RR_Compose<RR_ScalarRval, RR_WriteOptionalBufferFixedSize<3, sizeof(CFTypeRef)>>) \
  MACRO(LSCopyKindStringForMIMEType,                             \
        RR_Compose<RR_ScalarRval, RR_WriteOptionalBufferFixedSize<1, sizeof(CFStringRef)>>) \
  MACRO(LSGetApplicationForInfo, RR_Compose<                     \
                                   RR_ScalarRval,                \
                                   RR_WriteOptionalBufferFixedSize<4, sizeof(FSRef)>, \
                                   RR_WriteOptionalBufferFixedSize<5, sizeof(CFURLRef)>>) \
  MACRO(LSGetApplicationForURL, RR_Compose<                     \
                                   RR_ScalarRval,                \
                                   RR_WriteOptionalBufferFixedSize<2, sizeof(FSRef)>, \
                                   RR_WriteOptionalBufferFixedSize<3, sizeof(CFURLRef)>>) \
  MACRO(NSClassFromString, RR_ScalarRval)                        \
  MACRO(NSRectFill)                                              \
  MACRO(NSSearchPathForDirectoriesInDomains, RR_ScalarRval)      \
  MACRO(NSSetFocusRingStyle, RR_ScalarRval)                      \
  MACRO(NSTemporaryDirectory, RR_ScalarRval)                     \
  MACRO(OSSpinLockLock, nullptr, Preamble_OSSpinLockLock)        \
  MACRO(ReleaseEvent, RR_ScalarRval)                             \
  MACRO(RemoveEventFromQueue, RR_ScalarRval)                     \
  MACRO(RetainEvent, RR_ScalarRval)                              \
  MACRO(SendEventToEventTarget, RR_ScalarRval)                   \
  /* These are not public APIs, but other redirected functions may be aliases for */ \
  /* these which are dynamically installed on the first call in a way that our */ \
  /* redirection mechanism doesn't completely account for. */    \
  MACRO(SLDisplayCopyColorSpace, RR_ScalarRval)                  \
  MACRO(SLDisplayIOServicePort, RR_ScalarRval)                   \
  MACRO(SLEventSourceCounterForEventType, RR_ScalarRval)         \
  MACRO(SLMainDisplayID, RR_ScalarRval)                          \
  MACRO(SLSSetDenyWindowServerConnections, RR_ScalarRval)        \
  MACRO(SLSShutdownServerConnections)

#define MAKE_CALL_EVENT(aName, ...)  CallEvent_ ##aName ,

enum CallEvent {                                \
  FOR_EACH_REDIRECTION(MAKE_CALL_EVENT)         \
  CallEvent_Count                               \
};

#undef MAKE_CALL_EVENT

///////////////////////////////////////////////////////////////////////////////
// Callbacks
///////////////////////////////////////////////////////////////////////////////

enum CallbackEvent {
  CallbackEvent_CFRunLoopPerformCallBack,
  CallbackEvent_CGPathApplierFunction
};

typedef void (*CFRunLoopPerformCallBack)(void*);

static void
CFRunLoopPerformCallBackWrapper(void* aInfo)
{
  RecordReplayCallback(CFRunLoopPerformCallBack, &aInfo);
  rrc.mFunction(aInfo);

  // Make sure we service any callbacks that have been posted for the main
  // thread whenever the main thread's message loop has any activity.
  PauseMainThreadAndServiceCallbacks();
}

static size_t
CGPathElementPointCount(CGPathElement* aElement)
{
  switch (aElement->type) {
  case kCGPathElementCloseSubpath:         return 0;
  case kCGPathElementMoveToPoint:
  case kCGPathElementAddLineToPoint:       return 1;
  case kCGPathElementAddQuadCurveToPoint:  return 2;
  case kCGPathElementAddCurveToPoint:      return 3;
  default: MOZ_CRASH();
  }
}

static void
CGPathApplierFunctionWrapper(void* aInfo, CGPathElement* aElement)
{
  RecordReplayCallback(CGPathApplierFunction, &aInfo);

  CGPathElement replayElement;
  if (IsReplaying()) {
    aElement = &replayElement;
  }

  aElement->type = (CGPathElementType) RecordReplayValue(aElement->type);

  size_t npoints = CGPathElementPointCount(aElement);
  if (IsReplaying()) {
    aElement->points = new CGPoint[npoints];
  }
  RecordReplayBytes(aElement->points, npoints * sizeof(CGPoint));

  rrc.mFunction(aInfo, aElement);

  if (IsReplaying()) {
    delete[] aElement->points;
  }
}

void
ReplayInvokeCallback(size_t aCallbackId)
{
  MOZ_RELEASE_ASSERT(IsReplaying());
  switch (aCallbackId) {
  case CallbackEvent_CFRunLoopPerformCallBack:
    CFRunLoopPerformCallBackWrapper(nullptr);
    break;
  case CallbackEvent_CGPathApplierFunction:
    CGPathApplierFunctionWrapper(nullptr, nullptr);
    break;
  default:
    MOZ_CRASH();
  }
}

///////////////////////////////////////////////////////////////////////////////
// system call redirections
///////////////////////////////////////////////////////////////////////////////

static void
RR_recvmsg(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& msg = aArguments->Arg<1, struct msghdr*>();

  aEvents.CheckInput(msg->msg_iovlen);
  for (int i = 0; i < msg->msg_iovlen; i++) {
    aEvents.CheckInput(msg->msg_iov[i].iov_len);
  }

  aEvents.RecordOrReplayValue(&msg->msg_flags);
  aEvents.RecordOrReplayValue(&msg->msg_controllen);
  aEvents.RecordOrReplayBytes(msg->msg_control, msg->msg_controllen);

  size_t nbytes = aArguments->Rval<size_t>();
  for (int i = 0; nbytes && i < msg->msg_iovlen; i++) {
    struct iovec* iov = &msg->msg_iov[i];
    size_t iovbytes = nbytes <= iov->iov_len ? nbytes : iov->iov_len;
    aEvents.RecordOrReplayBytes(iov->iov_base, iovbytes);
    nbytes -= iovbytes;
  }
  MOZ_RELEASE_ASSERT(nbytes == 0);
}

static PreambleResult
Preamble_mprotect(CallArguments* aArguments)
{
  // Ignore any mprotect calls that occur after saving a checkpoint.
  if (!HasSavedCheckpoint()) {
    return PreambleResult::PassThrough;
  }
  aArguments->Rval<ssize_t>() = 0;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_mmap(CallArguments* aArguments)
{
  auto& address = aArguments->Arg<0, void*>();
  auto& size = aArguments->Arg<1, size_t>();
  auto& prot = aArguments->Arg<2, size_t>();
  auto& flags = aArguments->Arg<3, size_t>();
  auto& fd = aArguments->Arg<4, size_t>();
  auto& offset = aArguments->Arg<5, size_t>();

  MOZ_RELEASE_ASSERT(address == PageBase(address));

  // Make sure that fixed mappings do not interfere with snapshot state.
  if (flags & MAP_FIXED) {
    CheckFixedMemory(address, RoundupSizeToPageBoundary(size));
  }

  void* memory = nullptr;
  if ((flags & MAP_ANON) || (IsReplaying() && !AreThreadEventsPassedThrough())) {
    // Get an anonymous mapping for the result.
    if (flags & MAP_FIXED) {
      // For fixed allocations, make sure this memory region is mapped and zero.
      if (!HasSavedCheckpoint()) {
        // Make sure this memory region is writable.
        OriginalCall(mprotect, int, address, size, PROT_READ | PROT_WRITE | PROT_EXEC);
      }
      memset(address, 0, size);
      memory = address;
    } else {
      memory = AllocateMemoryTryAddress(address, RoundupSizeToPageBoundary(size),
                                        MemoryKind::Tracked);
    }
  } else {
    // We have to call mmap itself, which can change memory protection flags
    // for memory that is already allocated. If we haven't saved a checkpoint
    // then this is no problem, but after saving a checkpoint we have to make
    // sure that protection flags are what we expect them to be.
    int newProt = HasSavedCheckpoint() ? (PROT_READ | PROT_EXEC) : prot;
    memory = OriginalCall(mmap, void*, address, size, newProt, flags, fd, offset);

    if (flags & MAP_FIXED) {
      MOZ_RELEASE_ASSERT(memory == address);
      RestoreWritableFixedMemory(memory, RoundupSizeToPageBoundary(size));
    } else if (memory && memory != (void*)-1) {
      RegisterAllocatedMemory(memory, RoundupSizeToPageBoundary(size), MemoryKind::Tracked);
    }
  }

  if (!(flags & MAP_ANON) && !AreThreadEventsPassedThrough()) {
    // Include the data just mapped in the recording.
    MOZ_RELEASE_ASSERT(memory && memory != (void*)-1);
    RecordReplayBytes(memory, size);
  }

  aArguments->Rval<void*>() = memory;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_munmap(CallArguments* aArguments)
{
  auto& address = aArguments->Arg<0, void*>();
  auto& size = aArguments->Arg<1, size_t>();

  DeallocateMemory(address, size, MemoryKind::Tracked);
  aArguments->Rval<ssize_t>() = 0;
  return PreambleResult::Veto;
}

static void
RR_getsockopt(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& optval = aArguments->Arg<3, void*>();
  auto& optlen = aArguments->Arg<4, int*>();

  // The initial length has already been clobbered while recording, but make
  // sure there is enough room for the copy while replaying.
  int initLen = *optlen;
  aEvents.RecordOrReplayValue(optlen);
  MOZ_RELEASE_ASSERT(*optlen <= initLen);

  aEvents.RecordOrReplayBytes(optval, *optlen);
}

static PreambleResult
Preamble_gettimeofday(CallArguments* aArguments)
{
  // If we have diverged from the recording, just get the actual current time
  // rather than causing the current debugger operation to fail. This function
  // is frequently called via e.g. JS natives which the debugger will execute.
  if (HasDivergedFromRecording()) {
    return PreambleResult::PassThrough;
  }
  return PreambleResult::Redirect;
}

static PreambleResult
Preamble_fcntl(CallArguments* aArguments)
{
  // Make sure fcntl is only used with a limited set of commands.
  auto& cmd = aArguments->Arg<1, size_t>();
  switch (cmd) {
  case F_GETFL:
  case F_SETFL:
  case F_GETFD:
  case F_SETFD:
  case F_NOCACHE:
  case F_SETLK:
  case F_SETLKW:
    break;
  default:
    MOZ_CRASH();
  }
  return PreambleResult::Redirect;
}

static PreambleResult
Preamble___disable_threadsignal(CallArguments* aArguments)
{
  // __disable_threadsignal is called when a thread finishes. During replay a
  // terminated thread can cause problems such as changing access bits on
  // tracked memory behind the scenes.
  //
  // Ideally, threads will never try to finish when we are replaying, since we
  // are supposed to have control over all threads in the system and only spawn
  // threads which will run forever. Unfortunately, GCD might have already
  // spawned threads before we were able to install our redirections, so use a
  // fallback here to keep these threads from terminating.
  if (IsReplaying()) {
    Thread::WaitForeverNoIdle();
  }
  return PreambleResult::PassThrough;
}

static void
RR___sysctl(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& old = aArguments->Arg<2, void*>();
  auto& oldlenp = aArguments->Arg<3, size_t*>();

  aEvents.CheckInput((old ? 1 : 0) | (oldlenp ? 2 : 0));
  if (oldlenp) {
    aEvents.RecordOrReplayValue(oldlenp);
  }
  if (old) {
    aEvents.RecordOrReplayBytes(old, *oldlenp);
  }
}

static PreambleResult
Preamble___workq_kernreturn(CallArguments* aArguments)
{
  // Busy-wait until initialization is complete.
  while (!gInitialized) {
    ThreadYield();
  }

  // Make sure we know this thread exists.
  Thread::Current();

  RecordReplayInvokeCall(CallEvent___workq_kernreturn, aArguments);
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_start_wqthread(CallArguments* aArguments)
{
  // When replaying we don't want system threads to run, but by the time we
  // initialize the record/replay system GCD has already started running.
  // Use this redirection to watch for new threads being spawned by GCD, and
  // suspend them immediately.
  if (IsReplaying()) {
    Thread::WaitForeverNoIdle();
  }

  RecordReplayInvokeCall(CallEvent_start_wqthread, aArguments);
  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// pthreads redirections
///////////////////////////////////////////////////////////////////////////////

static void
DirectLockMutex(pthread_mutex_t* aMutex)
{
  AutoPassThroughThreadEvents pt;
  ssize_t rv = OriginalCall(pthread_mutex_lock, ssize_t, aMutex);
  MOZ_RELEASE_ASSERT(rv == 0);
}

static void
DirectUnlockMutex(pthread_mutex_t* aMutex)
{
  AutoPassThroughThreadEvents pt;
  ssize_t rv = OriginalCall(pthread_mutex_unlock, ssize_t, aMutex);
  MOZ_RELEASE_ASSERT(rv == 0);
}

// Handle a redirection which releases a mutex, waits in some way for a cvar,
// and reacquires the mutex before returning.
static ssize_t
WaitForCvar(pthread_mutex_t* aMutex, bool aRecordReturnValue,
            const std::function<ssize_t()>& aCallback)
{
  Lock* lock = Lock::Find(aMutex);
  if (!lock) {
    AutoEnsurePassThroughThreadEvents pt;
    return aCallback();
  }
  ssize_t rv = 0;
  if (IsRecording()) {
    AutoPassThroughThreadEvents pt;
    rv = aCallback();
  } else {
    DirectUnlockMutex(aMutex);
  }
  lock->Exit();
  lock->Enter();
  if (IsReplaying()) {
    DirectLockMutex(aMutex);
  }
  if (aRecordReturnValue) {
    return RecordReplayValue(rv);
  }
  MOZ_RELEASE_ASSERT(rv == 0);
  return 0;
}

static PreambleResult
Preamble_pthread_cond_wait(CallArguments* aArguments)
{
  auto& cond = aArguments->Arg<0, pthread_cond_t*>();
  auto& mutex = aArguments->Arg<1, pthread_mutex_t*>();
  aArguments->Rval<ssize_t>() =
    WaitForCvar(mutex, false,
                [=]() { return OriginalCall(pthread_cond_wait, ssize_t, cond, mutex); });
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_cond_timedwait(CallArguments* aArguments)
{
  auto& cond = aArguments->Arg<0, pthread_cond_t*>();
  auto& mutex = aArguments->Arg<1, pthread_mutex_t*>();
  auto& timeout = aArguments->Arg<2, timespec*>();
  aArguments->Rval<ssize_t>() =
    WaitForCvar(mutex, true,
                [=]() { return OriginalCall(pthread_cond_timedwait, ssize_t,
                                            cond, mutex, timeout); });
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_cond_timedwait_relative_np(CallArguments* aArguments)
{
  auto& cond = aArguments->Arg<0, pthread_cond_t*>();
  auto& mutex = aArguments->Arg<1, pthread_mutex_t*>();
  auto& timeout = aArguments->Arg<2, timespec*>();
  aArguments->Rval<ssize_t>() =
    WaitForCvar(mutex, true,
                [=]() { return OriginalCall(pthread_cond_timedwait_relative_np, ssize_t,
                                            cond, mutex, timeout); });
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_create(CallArguments* aArguments)
{
  if (AreThreadEventsPassedThrough()) {
    return PreambleResult::Redirect;
  }

  auto& token = aArguments->Arg<0, pthread_t*>();
  auto& attr = aArguments->Arg<1, const pthread_attr_t*>();
  auto& start = aArguments->Arg<2, void*>();
  auto& startArg = aArguments->Arg<3, void*>();

  int detachState;
  int rv = pthread_attr_getdetachstate(attr, &detachState);
  MOZ_RELEASE_ASSERT(rv == 0);

  *token = Thread::StartThread((Thread::Callback) start, startArg,
                               detachState == PTHREAD_CREATE_JOINABLE);
  aArguments->Rval<ssize_t>() = 0;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_join(CallArguments* aArguments)
{
  if (AreThreadEventsPassedThrough()) {
    return PreambleResult::Redirect;
  }

  auto& token = aArguments->Arg<0, pthread_t>();
  auto& ptr = aArguments->Arg<1, void**>();

  Thread* thread = Thread::GetByNativeId(token);
  thread->Join();

  *ptr = nullptr;
  aArguments->Rval<ssize_t>() = 0;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_mutex_init(CallArguments* aArguments)
{
  auto& mutex = aArguments->Arg<0, pthread_mutex_t*>();
  auto& attr = aArguments->Arg<1, pthread_mutexattr_t*>();

  Lock::New(mutex);
  aArguments->Rval<ssize_t>() = OriginalCall(pthread_mutex_init, ssize_t, mutex, attr);
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_mutex_destroy(CallArguments* aArguments)
{
  auto& mutex = aArguments->Arg<0, pthread_mutex_t*>();

  Lock::Destroy(mutex);
  aArguments->Rval<ssize_t>() = OriginalCall(pthread_mutex_destroy, ssize_t, mutex);
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_mutex_lock(CallArguments* aArguments)
{
  auto& mutex = aArguments->Arg<0, pthread_mutex_t*>();

  Lock* lock = Lock::Find(mutex);
  if (!lock) {
    AutoEnsurePassThroughThreadEventsUseStackPointer pt;
    aArguments->Rval<ssize_t>() = OriginalCall(pthread_mutex_lock, ssize_t, mutex);
    return PreambleResult::Veto;
  }
  ssize_t rv = 0;
  if (IsRecording()) {
    AutoPassThroughThreadEvents pt;
    rv = OriginalCall(pthread_mutex_lock, ssize_t, mutex);
  }
  rv = RecordReplayValue(rv);
  MOZ_RELEASE_ASSERT(rv == 0 || rv == EDEADLK);
  if (rv == 0) {
    lock->Enter();
    if (IsReplaying()) {
      DirectLockMutex(mutex);
    }
  }
  aArguments->Rval<ssize_t>() = rv;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_mutex_trylock(CallArguments* aArguments)
{
  auto& mutex = aArguments->Arg<0, pthread_mutex_t*>();

  Lock* lock = Lock::Find(mutex);
  if (!lock) {
    AutoEnsurePassThroughThreadEvents pt;
    aArguments->Rval<ssize_t>() = OriginalCall(pthread_mutex_trylock, ssize_t, mutex);
    return PreambleResult::Veto;
  }
  ssize_t rv = 0;
  if (IsRecording()) {
    AutoPassThroughThreadEvents pt;
    rv = OriginalCall(pthread_mutex_trylock, ssize_t, mutex);
  }
  rv = RecordReplayValue(rv);
  MOZ_RELEASE_ASSERT(rv == 0 || rv == EBUSY);
  if (rv == 0) {
    lock->Enter();
    if (IsReplaying()) {
      DirectLockMutex(mutex);
    }
  }
  aArguments->Rval<ssize_t>() = rv;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_pthread_mutex_unlock(CallArguments* aArguments)
{
  auto& mutex = aArguments->Arg<0, pthread_mutex_t*>();

  Lock* lock = Lock::Find(mutex);
  if (!lock) {
    AutoEnsurePassThroughThreadEventsUseStackPointer pt;
    aArguments->Rval<ssize_t>() = OriginalCall(pthread_mutex_unlock, ssize_t, mutex);
    return PreambleResult::Veto;
  }
  lock->Exit();
  DirectUnlockMutex(mutex);
  aArguments->Rval<ssize_t>() = 0;
  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// stdlib redirections
///////////////////////////////////////////////////////////////////////////////

static void
RR_fread(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& buf = aArguments->Arg<0, void*>();
  auto& elemSize = aArguments->Arg<1, size_t>();
  auto& capacity = aArguments->Arg<2, size_t>();
  auto& rval = aArguments->Rval<size_t>();

  aEvents.CheckInput(elemSize);
  aEvents.CheckInput(capacity);
  MOZ_RELEASE_ASSERT(rval <= capacity);
  aEvents.RecordOrReplayBytes(buf, rval * elemSize);
}

static struct tm gGlobalTM;

// localtime behaves the same as localtime_r, except it is not reentrant.
// For simplicity, define this in terms of localtime_r.
static PreambleResult
Preamble_localtime(CallArguments* aArguments)
{
  aArguments->Rval<struct tm*>() = localtime_r(aArguments->Arg<0, const time_t*>(), &gGlobalTM);
  return PreambleResult::Veto;
}

// The same concern here applies as for localtime.
static PreambleResult
Preamble_gmtime(CallArguments* aArguments)
{
  aArguments->Rval<struct tm*>() = gmtime_r(aArguments->Arg<0, const time_t*>(), &gGlobalTM);
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_mach_absolute_time(CallArguments* aArguments)
{
  // This function might be called through OSSpinLock while setting gTlsThreadKey.
  Thread* thread = Thread::GetByStackPointer(&thread);
  if (!thread || thread->PassThroughEvents()) {
    aArguments->Rval<uint64_t>() = OriginalCall(mach_absolute_time, uint64_t);
    return PreambleResult::Veto;
  }
  return PreambleResult::Redirect;
}

static PreambleResult
Preamble_mach_vm_allocate(CallArguments* aArguments)
{
  auto& address = aArguments->Arg<1, void**>();
  auto& size = aArguments->Arg<2, size_t>();
  *address = AllocateMemory(size, MemoryKind::Tracked);
  aArguments->Rval<size_t>() = KERN_SUCCESS;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_mach_vm_deallocate(CallArguments* aArguments)
{
  auto& address = aArguments->Arg<1, void*>();
  auto& size = aArguments->Arg<2, size_t>();
  DeallocateMemory(address, size, MemoryKind::Tracked);
  aArguments->Rval<size_t>() = KERN_SUCCESS;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_mach_vm_protect(CallArguments* aArguments)
{
  // Ignore any mach_vm_protect calls that occur after saving a checkpoint, as for mprotect.
  if (!HasSavedCheckpoint()) {
    return PreambleResult::PassThrough;
  }
  aArguments->Rval<size_t>() = KERN_SUCCESS;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_vm_purgable_control(CallArguments* aArguments)
{
  // Never allow purging of volatile memory, to simplify memory snapshots.
  auto& state = aArguments->Arg<3, int*>();
  *state = VM_PURGABLE_NONVOLATILE;
  aArguments->Rval<size_t>() = KERN_SUCCESS;
  return PreambleResult::Veto;
}

static PreambleResult
Preamble_vm_copy(CallArguments* aArguments)
{
  // Asking the kernel to copy memory doesn't work right if the destination is
  // non-writable, so do the copy manually.
  auto& src = aArguments->Arg<1, void*>();
  auto& size = aArguments->Arg<2, size_t>();
  auto& dest = aArguments->Arg<3, void*>();
  memcpy(dest, src, size);
  aArguments->Rval<size_t>() = KERN_SUCCESS;
  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// NSPR redirections
///////////////////////////////////////////////////////////////////////////////

// Even though NSPR is compiled as part of firefox, it is easier to just
// redirect this stuff than get record/replay related changes into the source.

static PreambleResult
Preamble_PL_NewHashTable(CallArguments* aArguments)
{
  auto& keyHash = aArguments->Arg<1, PLHashFunction>();
  auto& keyCompare = aArguments->Arg<2, PLHashComparator>();
  auto& valueCompare = aArguments->Arg<3, PLHashComparator>();
  auto& allocOps = aArguments->Arg<4, const PLHashAllocOps*>();
  auto& allocPriv = aArguments->Arg<5, void*>();

  GeneratePLHashTableCallbacks(&keyHash, &keyCompare, &valueCompare, &allocOps, &allocPriv);
  return PreambleResult::PassThrough;
}

static PreambleResult
Preamble_PL_HashTableDestroy(CallArguments* aArguments)
{
  auto& table = aArguments->Arg<0, PLHashTable*>();

  void* priv = table->allocPriv;
  OriginalCall(PL_HashTableDestroy, void, table);
  DestroyPLHashTableCallbacks(priv);
  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// Objective C redirections
///////////////////////////////////////////////////////////////////////////////

static bool
TestObjCObjectClass(id aObj, const char* aName)
{
  // When recording we can test to see what the class of an object is, but we
  // have to record the result of the test because the object "pointers" we
  // have when replaying are not valid.
  bool found = false;
  if (IsRecording()) {
    Class cls = object_getClass(aObj);
    while (cls) {
      if (!strcmp(class_getName(cls), aName)) {
        found = true;
        break;
      }
      cls = class_getSuperclass(cls);
    }
  }
  return RecordReplayValue(found);
}

// From Foundation.h, which has lots of Objective C declarations and can't be
// included here :(
struct NSFastEnumerationState
{
  unsigned long state;
  id* itemsPtr;
  unsigned long* mutationsPtr;
  unsigned long extra[5];
};

// Emulation of NSFastEnumeration on arrays does not replay any exceptions
// thrown by mutating the array while it is being iterated over.
static unsigned long gNeverChange;

static PreambleResult
Preamble_objc_msgSend(CallArguments* aArguments)
{
  Thread* thread = Thread::Current();
  if (!thread || thread->PassThroughEvents()) {
    return PreambleResult::Redirect;
  }
  EnsureNotDivergedFromRecording();

  auto& object = aArguments->Arg<0, id>();
  auto& message = aArguments->Arg<1, const char*>();

  thread->Events().RecordOrReplayThreadEvent(CallIdToThreadEvent(CallEvent_objc_msgSend));
  thread->Events().CheckInput(message);

  bool handled = false;

  // Watch for some top level NSApplication messages that can cause Gecko
  // events to be processed.
  if ((!strcmp(message, "run") ||
       !strcmp(message, "nextEventMatchingMask:untilDate:inMode:dequeue:")) &&
      TestObjCObjectClass(object, "NSApplication"))
  {
    PassThroughThreadEventsAllowCallbacks([&]() {
        RecordReplayInvokeCall(CallEvent_objc_msgSend, aArguments);
      });
    handled = true;
  }

  // Other messages are performed as normal.
  if (!handled && IsRecording()) {
    AutoPassThroughThreadEvents pt;
    RecordReplayInvokeCall(CallEvent_objc_msgSend, aArguments);
  }

  RecordReplayBytes(&aArguments->Rval<size_t>(), sizeof(size_t));
  RecordReplayBytes(&aArguments->FloatRval(), sizeof(double));

  // Do some extra recording on messages that return additional state.

  if (!strcmp(message, "countByEnumeratingWithState:objects:count:") &&
      TestObjCObjectClass(object, "NSArray"))
  {
    auto& state = aArguments->Arg<2, NSFastEnumerationState*>();
    auto& rval = aArguments->Rval<size_t>();
    if (IsReplaying()) {
      state->itemsPtr = NewLeakyArray<id>(rval);
      state->mutationsPtr = &gNeverChange;
    }
    RecordReplayBytes(state->itemsPtr, rval * sizeof(id));
  }

  if (!strcmp(message, "getCharacters:") &&
      TestObjCObjectClass(object, "NSString"))
  {
    size_t len = 0;
    if (IsRecording()) {
      AutoPassThroughThreadEvents pt;
      len = CFStringGetLength((CFStringRef) object);
    }
    len = RecordReplayValue(len);
    RecordReplayBytes(aArguments->Arg<2, void*>(), len * sizeof(wchar_t));
  }

  if ((!strcmp(message, "UTF8String") ||
       !strcmp(message, "cStringUsingEncoding:")) &&
      TestObjCObjectClass(object, "NSString"))
  {
    auto& rval = aArguments->Rval<char*>();
    size_t len = RecordReplayValue(IsRecording() ? strlen(rval) : 0);
    if (IsReplaying()) {
      rval = NewLeakyArray<char>(len + 1);
    }
    RecordReplayBytes(rval, len + 1);
  }

  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// Cocoa redirections
///////////////////////////////////////////////////////////////////////////////

static void
RR_CFDataGetBytePtr(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& rval = aArguments->Rval<UInt8*>();

  size_t len = 0;
  if (IsRecording()) {
    len = OriginalCall(CFDataGetLength, size_t, aArguments->Arg<0, CFDataRef>());
  }
  aEvents.RecordOrReplayValue(&len);
  if (IsReplaying()) {
    rval = NewLeakyArray<UInt8>(len);
  }
  aEvents.RecordOrReplayBytes(rval, len);
}

static void DummyCFNotificationCallback(CFNotificationCenterRef aCenter, void* aObserver,
                                        CFStringRef aName, const void* aObject,
                                        CFDictionaryRef aUserInfo)
{
  // FIXME
  //MOZ_CRASH();
}

static PreambleResult
Preamble_CFNotificationCenterAddObserver(CallArguments* aArguments)
{
  auto& callback = aArguments->Arg<2, CFNotificationCallback>();
  if (!AreThreadEventsPassedThrough()) {
    callback = DummyCFNotificationCallback;
  }
  return PreambleResult::Redirect;
}

static size_t
CFNumberTypeBytes(CFNumberType aType)
{
  switch (aType) {
  case kCFNumberSInt8Type: return sizeof(int8_t);
  case kCFNumberSInt16Type: return sizeof(int16_t);
  case kCFNumberSInt32Type: return sizeof(int32_t);
  case kCFNumberSInt64Type: return sizeof(int64_t);
  case kCFNumberFloat32Type: return sizeof(float);
  case kCFNumberFloat64Type: return sizeof(double);
  case kCFNumberCharType: return sizeof(char);
  case kCFNumberShortType: return sizeof(short);
  case kCFNumberIntType: return sizeof(int);
  case kCFNumberLongType: return sizeof(long);
  case kCFNumberLongLongType: return sizeof(long long);
  case kCFNumberFloatType: return sizeof(float);
  case kCFNumberDoubleType: return sizeof(double);
  case kCFNumberCFIndexType: return sizeof(CFIndex);
  case kCFNumberNSIntegerType: return sizeof(long);
  case kCFNumberCGFloatType: return sizeof(CGFloat);
  default: MOZ_CRASH();
  }
}

static void
RR_CFNumberGetValue(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& type = aArguments->Arg<1, CFNumberType>();
  auto& value = aArguments->Arg<2, void*>();

  aEvents.CheckInput(type);
  aEvents.RecordOrReplayBytes(value, CFNumberTypeBytes(type));
}

static PreambleResult
Preamble_CFRunLoopSourceCreate(CallArguments* aArguments)
{
  if (!AreThreadEventsPassedThrough()) {
    auto& cx = aArguments->Arg<2, CFRunLoopSourceContext*>();

    RegisterCallbackData(BitwiseCast<void*>(cx->perform));
    RegisterCallbackData(cx->info);

    if (IsRecording()) {
      CallbackWrapperData* wrapperData = new CallbackWrapperData(cx->perform, cx->info);
      cx->perform = CFRunLoopPerformCallBackWrapper;
      cx->info = wrapperData;
    }
  }
  return PreambleResult::Redirect;
}

struct ContextDataInfo
{
  CGContextRef mContext;
  void* mData;
  size_t mDataSize;

  ContextDataInfo(CGContextRef aContext, void* aData, size_t aDataSize)
    : mContext(aContext), mData(aData), mDataSize(aDataSize)
  {}
};

static StaticInfallibleVector<ContextDataInfo> gContextData;

static void
RR_CGBitmapContextCreateWithData(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& data = aArguments->Arg<0, void*>();
  auto& height = aArguments->Arg<2, size_t>();
  auto& bytesPerRow = aArguments->Arg<4, size_t>();
  auto& rval = aArguments->Rval<CGContextRef>();

  MOZ_RELEASE_ASSERT(Thread::CurrentIsMainThread());
  gContextData.emplaceBack(rval, data, height * bytesPerRow);
}

template <size_t ContextArgument>
static void
RR_FlushCGContext(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& context = aArguments->Arg<ContextArgument, CGContextRef>();

  for (int i = gContextData.length() - 1; i >= 0; i--) {
    if (context == gContextData[i].mContext) {
      RecordReplayBytes(gContextData[i].mData, gContextData[i].mDataSize);
      return;
    }
  }
  MOZ_CRASH();
}

static PreambleResult
Preamble_CGContextRestoreGState(CallArguments* aArguments)
{
  return IsRecording() ? PreambleResult::PassThrough : PreambleResult::Veto;
}

static void
RR_CGDataProviderCreateWithData(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& info = aArguments->Arg<0, void*>();
  auto& data = aArguments->Arg<1, const void*>();
  auto& size = aArguments->Arg<2, size_t>();
  auto& releaseData = aArguments->Arg<3, CGDataProviderReleaseDataCallback>();

  if (IsReplaying()) {
    // Immediately release the data, since there is no data provider to do it for us.
    releaseData(info, data, size);
  }
}

static PreambleResult
Preamble_CGPathApply(CallArguments* aArguments)
{
  if (AreThreadEventsPassedThrough()) {
    return PreambleResult::Redirect;
  }

  auto& path = aArguments->Arg<0, CGPathRef>();
  auto& data = aArguments->Arg<1, void*>();
  auto& function = aArguments->Arg<2, CGPathApplierFunction>();

  RegisterCallbackData(BitwiseCast<void*>(function));
  RegisterCallbackData(data);
  PassThroughThreadEventsAllowCallbacks([&]() {
      CallbackWrapperData wrapperData(function, data);
      OriginalCall(CGPathApply, void, path, &wrapperData, CGPathApplierFunctionWrapper);
    });
  RemoveCallbackData(data);

  return PreambleResult::Veto;
}

// Note: We only redirect CTRunGetGlyphsPtr, not CTRunGetGlyphs. The latter may
// be implemented with a loop that jumps back into the code we overwrite with a
// jump, a pattern which ProcessRedirect.cpp does not handle. For the moment,
// Gecko code only calls CTRunGetGlyphs if CTRunGetGlyphsPtr fails, so make
// sure that CTRunGetGlyphsPtr always succeeds when it is being recorded.
// The same concerns apply to CTRunGetPositionsPtr and CTRunGetStringIndicesPtr,
// so they are all handled here.
template <typename ElemType, void (*GetElemsFn)(CTRunRef, CFRange, ElemType*)>
static void
RR_CTRunGetElements(Stream& aEvents, CallArguments* aArguments, ErrorType* aError)
{
  auto& run = aArguments->Arg<0, CTRunRef>();
  auto& rval = aArguments->Rval<ElemType*>();

  size_t count;
  if (IsRecording()) {
    AutoPassThroughThreadEvents pt;
    count = CTRunGetGlyphCount(run);
    if (!rval) {
      rval = NewLeakyArray<ElemType>(count);
      GetElemsFn(run, CFRangeMake(0, 0), rval);
    }
  }
  aEvents.RecordOrReplayValue(&count);
  if (IsReplaying()) {
    rval = NewLeakyArray<ElemType>(count);
  }
  aEvents.RecordOrReplayBytes(rval, count * sizeof(ElemType));
}

static PreambleResult
Preamble_OSSpinLockLock(CallArguments* aArguments)
{
  auto& lock = aArguments->Arg<0, OSSpinLock*>();

  // These spin locks never need to be recorded, but they are used by malloc
  // and can end up calling other recorded functions like mach_absolute_time,
  // so make sure events are passed through here. Note that we don't have to
  // redirect OSSpinLockUnlock, as it doesn't have these issues.
  AutoEnsurePassThroughThreadEventsUseStackPointer pt;
  OriginalCall(OSSpinLockLock, void, lock);

  return PreambleResult::Veto;
}

///////////////////////////////////////////////////////////////////////////////
// Redirection generation
///////////////////////////////////////////////////////////////////////////////

#define MAKE_REDIRECTION_ENTRY(aName, ...)          \
  { #aName, nullptr, nullptr, __VA_ARGS__ },

Redirection gRedirections[] = {
  FOR_EACH_REDIRECTION(MAKE_REDIRECTION_ENTRY)
  { }
};

#undef MAKE_REDIRECTION_ENTRY

///////////////////////////////////////////////////////////////////////////////
// Direct system call API
///////////////////////////////////////////////////////////////////////////////

const char*
SymbolNameRaw(void* aPtr)
{
  Dl_info info;
  return (dladdr(aPtr, &info) && info.dli_sname) ? info.dli_sname : "???";
}

void*
DirectAllocateMemory(void* aAddress, size_t aSize)
{
  void* res = OriginalCall(mmap, void*,
                           aAddress, aSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_ANON | MAP_PRIVATE, -1, 0);
  MOZ_RELEASE_ASSERT(res && res != (void*)-1);
  return res;
}

void
DirectDeallocateMemory(void* aAddress, size_t aSize)
{
  ssize_t rv = OriginalCall(munmap, int, aAddress, aSize);
  MOZ_RELEASE_ASSERT(rv >= 0);
}

void
DirectWriteProtectMemory(void* aAddress, size_t aSize, bool aExecutable,
                         bool aIgnoreFailures /* = false */)
{
  ssize_t rv = OriginalCall(mprotect, int, aAddress, aSize,
                            PROT_READ | (aExecutable ? PROT_EXEC : 0));
  MOZ_RELEASE_ASSERT(aIgnoreFailures || rv == 0);
}

void
DirectUnprotectMemory(void* aAddress, size_t aSize, bool aExecutable,
                      bool aIgnoreFailures /* = false */)
{
  ssize_t rv = OriginalCall(mprotect, int, aAddress, aSize,
                            PROT_READ | PROT_WRITE | (aExecutable ? PROT_EXEC : 0));
  MOZ_RELEASE_ASSERT(aIgnoreFailures || rv == 0);
}

// From chromium/src/base/eintr_wrapper.h
#define HANDLE_EINTR(x) ({ \
  typeof(x) __eintr_result__; \
  do { \
    __eintr_result__ = x; \
  } while (__eintr_result__ == -1 && errno == EINTR); \
  __eintr_result__;\
})

void
DirectSeekFile(FileHandle aFd, uint64_t aOffset)
{
  static_assert(sizeof(uint64_t) == sizeof(off_t), "off_t should have 64 bits");
  ssize_t rv = HANDLE_EINTR(OriginalCall(lseek, int, aFd, aOffset, SEEK_SET));
  MOZ_RELEASE_ASSERT(rv >= 0);
}

FileHandle
DirectOpenFile(const char* aFilename, bool aWriting)
{
  int flags = aWriting ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY;
  int perms = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
  int fd = HANDLE_EINTR(OriginalCall(open, int, aFilename, flags, perms));
  MOZ_RELEASE_ASSERT(fd > 0);
  return fd;
}

void
DirectCloseFile(FileHandle aFd)
{
  ssize_t rv = HANDLE_EINTR(OriginalCall(close, int, aFd));
  MOZ_RELEASE_ASSERT(rv >= 0);
}

void
DirectDeleteFile(const char* aFilename)
{
  ssize_t rv = unlink(aFilename);
  MOZ_RELEASE_ASSERT(rv >= 0 || errno == ENOENT);
}

void
DirectWrite(FileHandle aFd, const void* aData, size_t aSize)
{
  ssize_t rv = HANDLE_EINTR(OriginalCall(write, int, aFd, aData, aSize));
  MOZ_RELEASE_ASSERT((size_t) rv == aSize);
}

void
DirectPrint(const char* aString)
{
  DirectWrite(STDERR_FILENO, aString, strlen(aString));
}

size_t
DirectRead(FileHandle aFd, void* aData, size_t aSize)
{
  // Clear the memory in case it is write protected by the memory snapshot
  // mechanism.
  memset(aData, 0, aSize);
  ssize_t rv = HANDLE_EINTR(OriginalCall(read, int, aFd, aData, aSize));
  MOZ_RELEASE_ASSERT(rv >= 0);
  return (size_t) rv;
}

void
DirectCreatePipe(FileHandle* aWriteFd, FileHandle* aReadFd)
{
  int fds[2];
  ssize_t rv = OriginalCall(pipe, int, fds);
  MOZ_RELEASE_ASSERT(rv >= 0);
  *aWriteFd = fds[1];
  *aReadFd = fds[0];
}

static double gAbsoluteToNanosecondsRate;

void
InitializeCurrentTime()
{
  AbsoluteTime time = { 1000000, 0 };
  Nanoseconds rate = AbsoluteToNanoseconds(time);
  MOZ_RELEASE_ASSERT(!rate.hi);
  gAbsoluteToNanosecondsRate = rate.lo / 1000000.0;
}

double
CurrentTime()
{
  return OriginalCall(mach_absolute_time, int64_t) * gAbsoluteToNanosecondsRate / 1000.0;
}

void
DirectSpawnThread(void (*aFunction)(void*), void* aArgument)
{
  MOZ_RELEASE_ASSERT(IsMiddleman() || AreThreadEventsPassedThrough());

  pthread_attr_t attr;
  int rv = pthread_attr_init(&attr);
  MOZ_RELEASE_ASSERT(rv == 0);

  rv = pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
  MOZ_RELEASE_ASSERT(rv == 0);

  rv = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  MOZ_RELEASE_ASSERT(rv == 0);

  pthread_t pthread;
  rv = OriginalCall(pthread_create, int, &pthread, &attr, (void* (*)(void*)) aFunction, aArgument);
  MOZ_RELEASE_ASSERT(rv == 0);

  rv = pthread_attr_destroy(&attr);
  MOZ_RELEASE_ASSERT(rv == 0);
}

} // recordreplay
} // mozilla
