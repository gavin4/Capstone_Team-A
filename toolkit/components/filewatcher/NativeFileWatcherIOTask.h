#ifndef NATIVEFILEWATCHERIOTASK_H
#define NATIVEFILEWATCHERIOTASK_H

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
#include "NativeFileWatcherFSETask.h"

namespace mozilla {

namespace moz_filewatcher {

class NativeFileWatcherFSETask;

/**
 * This runnable is dispatched from the main thread to get the notifications of
 * the changes in the watched resources by handling and acting on signals from
 * the kernel.
 *
 * This is accomplished with non-blocking reads from an inotify file descriptor
 * when the signal handler tied to the inotify file descriptor increments an
 * event counter. Callbacks are then dispatched depending on the type of event
 * the inotify_event read from the file descriptor describes.
 */
class NativeFileWatcherIOTask : public Runnable
{
public:
    explicit NativeFileWatcherIOTask()
        : Runnable("NativeFileWatcherIOTask")
        , mShuttingDown(false)
        , mWorkerFSERunnable(nullptr)
    {
    }

    NS_IMETHOD Run() override;
    nsresult AddPathRunnableMethod(
            PathRunnablesParametersWrapper* aWrappedParameters);
    nsresult RemovePathRunnableMethod(
            PathRunnablesParametersWrapper* aWrappedParameters);
    nsresult DeactivateRunnableMethod();
    void SetRef(CFRunLoopRef toSet);


private:
    // Maintain 2 indexes - one by resource path, one by inotify watch descriptor.
    // Since our watch descriptor is an int, we use nsUint32HashKey to compute the
    // hashing key. We need 2 indexes in order to quickly look up the
    // changed resource in the Worker Thread.
    // The objects are not ref counted and get destroyed by
    // mWatchedResourcesByPath on NativeFileWatcherService::Destroy or in
    // NativeFileWatcherService::RemovePath.
    nsClassHashtable<nsStringHashKey, WatchedResourceDescriptor>
    mWatchedResourcesByPath;

    // The same callback can be associated to multiple watches so we need to keep
    // them alive as long as there is a watch using them. We create two hashtables
    // to map directory names to lists of nsMainThreadPtr<callbacks>.
    nsClassHashtable<nsStringHashKey, ChangeCallbackArray> mChangeCallbacksTable;
    nsClassHashtable<nsStringHashKey, ErrorCallbackArray> mErrorCallbacksTable;

    // MacOS Runnable thread for back-end fsevents work.
    nsCOMPtr<nsIThread> mFSEThread;
    // The instance of the runnable dealing with the I/O.
    nsCOMPtr<nsIRunnable> mWorkerFSERunnable;

    // Other methods need to know that a shutdown is in progress.
    bool mShuttingDown;

    // Main inotify file descriptor (initialized in NativeFileWatcher's Init())
    int mInotifyFileDescriptor;

    static CallBackEvents mCallBackEvents;
    CFRunLoopRef childRunLoop;

    // Here is the queue for the events read from inotify file descriptor
    //std::queue<inotify_event*> mInotifyEventQueue;

    nsresult RunInternal();

    std::vector<std::string> getRecursivePaths(char* initialPath);

    nsresult DispatchChangeCallbacks(
            WatchedResourceDescriptor* aResourceDescriptor,
            const nsAString& aChangedResource);

    nsresult ReportChange(
            const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
            const nsAString& aChangedResource);

    nsresult DispatchErrorCallbacks(
            WatchedResourceDescriptor* aResourceDescriptor,
            nsresult anError,
            long anOSError);

    nsresult ReportError(
            const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError,
            nsresult anError,
            long anOSError);

    nsresult ReportSuccess(const nsMainThreadPtrHandle<
                           nsINativeFileWatcherSuccessCallback>& aOnSuccess,
                           const nsAString& aResourcePath);

    nsresult AddDirectoryToWatchList(
            WatchedResourceDescriptor* aDirectoryDescriptor);

    void AppendCallbacksToHashtables(
            const nsAString& aPath,
            const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
            const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError);

    void RemoveCallbacksFromHashtables(
            const nsAString& aPath,
            const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
            const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError);

    nsresult MakeResourcePath(WatchedResourceDescriptor* changedDescriptor,
                              const nsAString& resourceName,
                              nsAString& nativeResourcePath);
};

}

}

#endif // NATIVEFILEWATCHERIOTASK_H
