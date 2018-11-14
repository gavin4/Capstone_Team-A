#ifndef mozilla_nativefilewatcher_h__
#define mozilla_nativefilewatcher_h__

#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsThreadUtils.h"

#include "NativeFileWatcherCommons.h"

#include <CoreServices/CoreServices.h>
#include <CoreServices/Components.k.h>

namespace mozilla {

namespace moz_filewatcher {

class NativeFileWatcherIOTask;

/**
 * A structure to hold the information about a single inotify watch descriptor.
 */
struct WatchedResourceDescriptor
{
    // The path on the file system of the watched resource.
    nsString mPath;
    WatchedResourceDescriptor(const nsAString& aPath)
        : mPath(aPath)
    {
    }
};

struct CallBackAction{
    CallBackAction(char*eventPath, const FSEventStreamEventFlags eventFlag, const FSEventStreamEventId eventIds)
        : mEventFlags(eventFlag)
        , mEventIds(eventIds)
    {
        snprintf(mEventPath, 2048, "%s", eventPath);
    }

    char mEventPath[2048]; // FIXME: find max path length
    const FSEventStreamEventFlags mEventFlags;
    const FSEventStreamEventId mEventIds;
};

struct CallBackEvents{
    CallBackEvents()
        : callBackLock("filewatcher::eventLock")
    {}
    std::queue<CallBackAction> mSavedEvents;
    Mutex callBackLock;
};

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

class NativeFileWatcherService final
  : public nsINativeFileWatcherService
  , public nsIObserver
{
public:
  NS_DECL_ISUPPORTS

  NS_DECL_NSINATIVEFILEWATCHERSERVICE
  NS_DECL_NSIOBSERVER

  NativeFileWatcherService();

  nsresult Init();

private:
  // Filewatcher IO thread for back-end inotify work.
  nsCOMPtr<nsIThread> mIOThread;
  // The instance of the runnable dealing with the I/O.
  nsCOMPtr<nsIRunnable> mWorkerIORunnable;

  static void signalHandler(int signal);

  nsresult Uninit();
  void WakeUpWorkerThread();

  // Make the dtor private to make this object only deleted via its ::Release()
  // method.
  ~NativeFileWatcherService();
  NativeFileWatcherService(const NativeFileWatcherService& other) = delete;
  void operator=(const NativeFileWatcherService& other) = delete;
};

} // namespace mozilla

#endif // mozilla_nativefilewatcher_h__
