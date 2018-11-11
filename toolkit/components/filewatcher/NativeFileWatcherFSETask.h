#ifndef NATIVEFILEWATCHERFSETASK_H
#define NATIVEFILEWATCHERFSETASK_H

#include "mozilla/Mutex.h"
#include "mozilla/Services.h"
#include "mozilla/UniquePtr.h"
#include "nsClassHashtable.h"
#include "nsDataHashtable.h"
#include "nsIFile.h"
#include "nsIObserverService.h"
#include "nsProxyRelease.h"
#include "nsTArray.h"
#include "mozilla/Logging.h"
#include "mozilla/Scoped.h"

#include <queue>
#include <vector>

#include "NativeFileWatcherCommons.h"
#include "NativeFileWatcherIOTask.h"


namespace mozilla {

namespace moz_filewatcher {

class NativeFileWatcherIOTask;

class NativeFileWatcherFSETask : public Runnable
{
public:
    explicit NativeFileWatcherFSETask(NativeFileWatcherIOTask* parent, CallBackEvents* cbe, std::vector<CFStringRef>& dirs)
        : Runnable("NativeFileWatcherFSETask")
        , mParent(parent)
    {
        for(int i(0); i < dirs.size(); i++) {
            mDirs.push_back(dirs[i]);
        }

        cbe_internal = cbe;
    }

    NS_IMETHOD Run() override;

private:
    FSEventStreamRef mEventStreamRef;
    CFRunLoopRef mRunLoop = nullptr;
    std::vector<CFStringRef> mDirs;
    static CallBackEvents* cbe_internal;
    NativeFileWatcherIOTask* mParent;

    static void fsevents_callback(ConstFSEventStreamRef streamRef,
                                               void *clientCallBackInfo,
                                               size_t numEvents,
                                               void *eventPaths,
                                               const FSEventStreamEventFlags eventFlags[],
                                               const FSEventStreamEventId eventIds[]);
};
}
}

#endif // NATIVEFILEWATCHERFSETASK_H
