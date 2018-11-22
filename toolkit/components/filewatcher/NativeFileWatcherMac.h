#ifndef mozilla_nativefilewatcher_h__
#define mozilla_nativefilewatcher_h__

#include "NativeFileWatcherCommons.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsINativeFileWatcher.h"
#include "nsIObserver.h"
#include "nsThreadUtils.h"
#include <mozilla/Mutex.h>

#include <CoreServices/Components.k.h>
#include <CoreServices/CoreServices.h>

#include <queue>
#include <vector>
#include <sys/syslimits.h>


namespace mozilla {

namespace moz_filewatcher{

/**
 * A structure to hold the information about a single watched resource.
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

/**
 * The CallBackAction struct is a specific event that occurs in the filesystem as reported by FSEvents.
 */
struct CallBackAction{
    CallBackAction(char*eventPath, const FSEventStreamEventFlags eventFlag, const FSEventStreamEventId eventIds)
        : mEventFlags(eventFlag)
        , mEventIds(eventIds)
    {
        snprintf(mEventPath, PATH_MAX, "%s", eventPath);
    }

    char mEventPath[PATH_MAX];
    const FSEventStreamEventFlags mEventFlags;
    const FSEventStreamEventId mEventIds;
};


/**
 * The CallBackEvents struct manages the FSEvents events returned to the stream.
 *
 * This is populated inside of the FSEvents callback. It is then processed on a separate
 * thread by NativeFileWatcherIOTask::RunInternal() which parses the events and calls the
 * appropriate callbacks.
 */
struct CallBackEvents{
    CallBackEvents()
        : callBackLock("filewatcher::eventLock")
    {}
    std::queue<CallBackAction> mSavedEvents; // Used to hold individual FSEvents
    Mutex callBackLock; // Used for safe access across multiple threads.
};

class NativeFileWatcherIOTask; // Forward Declaration of class.


/**
 * The NativeFileWatcherFSETask class which holds all of the functionality of the FSEventStream.
 *
 * It inherits from Runnable as we need to make a blocking call to CFRunLoopRun(). CFRunLoopRun() allows the
 * stream to report file system events. The event stream is an immutable object, therefore with every change in the
 * paths watched, the stream must be stopped and a new stream must be created with a new list of paths to be watched.
 * 
 * This class runs on a separate worker thread, mFSEThread, that is scheduled by mIOThread. This is because of the
 * blocking loop above. When the loop exits and the stream must be restarted (in the case of adding paths or removing
 * the non-last path) this task may also be rescheduled by itself through NS_DispatchToCurrentThread(this);
 */
class NativeFileWatcherFSETask : public Runnable
{
public:
    explicit NativeFileWatcherFSETask(NativeFileWatcherIOTask* parent, CallBackEvents* cbe, std::vector<CFStringRef>& dirs);

    // This is a blocking method which runs the steam on a CFRunLoop. It returns from the blocking call when the
	// owner stops the run loop.
    NS_IMETHOD Run() override;

    // Adds a file or directory path to the stream. Because our stream is immutable, we are restarting the stream with the new configuration.
    NS_IMETHOD AddPath(char* pathToAdd);

    // Removes path from the stream and restarts it. Unless it removes the last path being watched by the stream.
    // In this instance, we will return from this runnable without starting a new stream.
    NS_IMETHOD RemovePath(char* pathToRemove);

private:
	// FSEventStream is the file system events stream,
	// Create a new event stream, perform operations on the stream,
   	// and so on using functions that begin with FSEventStream
    FSEventStreamRef mEventStreamRef;
    CFRunLoopRef mRunLoop = nullptr;  // Reference to the RunLoop which is passed back to the parent class, to stop the RunLoop
    std::vector<CFStringRef> mDirs;   // A mutable list of directories which we build dynamically. Because our stream is immutable, 
									  // we are restarting the stream with the new list of paths/directories to watch, contained in 
									  // this vector, every time we need to add and remove a path.
    static CallBackEvents* cbe_internal; // Pointer to a shared callback events struct which is allocated in the NativeFileWatcherIOTask 
										 // (owner) class and used to hold events read from the stream for processing back in the owner 
										 // class. Static as we need to access this from the static callback handler in the event stream.
    NativeFileWatcherIOTask* mParent; // Pointer to the owner class to allow stopping the CFRunLoop from another thread.
                                      // Please see the variable mRunLoop documentation for more information.

    // Callback which handles events from the stream and is automatically called from the running stream
    // which we have set up to monitor the user defined paths. Static as we need a function pointer to pass
	// to the stream on creation for the callback handler, and this is a member function.
    static void fsevents_callback(ConstFSEventStreamRef streamRef,
                                               void *clientCallBackInfo,
                                               size_t numEvents,
                                               void *eventPaths,
                                               const FSEventStreamEventFlags eventFlags[],
                                               const FSEventStreamEventId eventIds[]);

    // Utility function which gets all currently watched paths from the stream except for skip, if supplied.
    std::vector<CFStringRef> GetCurrentStreamPaths(char* skip = "");
};


/**
 * This runnable is dispatched from the main thread to process the events saved
 * into the CallBackEvent structure populated in the stream callback.
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
    // Maintain an index of watch descriptors by resource path.
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

    // Structure which is populated inside of the stream FSEventStream callback and used to
    // call the correct user supplied change callbacks.
    static CallBackEvents mCallBackEvents;

    // Automatically set from the child FSETask and holds the current reference to the
    // FSEvent's RunLoop.
    CFRunLoopRef childRunLoop;

    // Locks the mCallBackEvent structure, pulls events which were added in the stream
    // callback of FSETask and calls the corresponding user supplied callbacks for that
    // watched resource.
    nsresult RunInternal();

    // Breaks down the initialPath agrument into recursively shortened paths down to the
    // root. This is required because there isn't a direct correlation between a returned
    // FSEvent and the initial watched resource. All paths returned from this are checked
    // against the callback hashtable. All callbacks will be executed for matching entries in the hashtable.
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


} // End moz_filewatcher namespace


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
  // Filewatcher IO thread for back-end FSEvent's work.
  nsCOMPtr<nsIThread> mIOThread;
  // The instance of the runnable dealing with the I/O.
  nsCOMPtr<nsIRunnable> mWorkerIORunnable;

  nsresult Uninit();

  // Make the dtor private to make this object only deleted via its ::Release()
  // method.
  ~NativeFileWatcherService();
  NativeFileWatcherService(const NativeFileWatcherService& other) = delete;
  void operator=(const NativeFileWatcherService& other) = delete;
};

} // namespace mozilla

#endif // mozilla_nativefilewatcher_h__
