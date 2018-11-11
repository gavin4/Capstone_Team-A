#ifndef NATIVEFILEWATCHERCOMMONS_H
#define NATIVEFILEWATCHERCOMMONS_H

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


namespace mozilla {

namespace moz_filewatcher {


/**
 * An event used to notify the main thread when an error happens.
 */
class WatchedErrorEvent final : public Runnable
{
public:
    /**
   * @param aOnError The passed error callback.
   * @param aError The |nsresult| error value.
   * @param osError The error returned by GetLastError().
   */
    WatchedErrorEvent(
            const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError,
            const nsresult& anError,
            const long& osError)
        : Runnable("WatchedErrorEvent")
        , mOnError(aOnError)
        , mError(anError)
    {
        MOZ_ASSERT(!NS_IsMainThread());
    }

    NS_IMETHOD Run() override
    {
        MOZ_ASSERT(NS_IsMainThread());

        // Make sure we wrap a valid callback since it's not mandatory to provide
        // one when watching a resource.
        if (mOnError) {
            (void)mOnError->Complete(mError, mOsError);
        }

        return NS_OK;
    }

private:
    nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback> mOnError;
    nsresult mError;
    long mOsError;
};

/**
 * An event used to notify the main thread when an operation is successful.
 */
class WatchedSuccessEvent final : public Runnable
{
public:
    /**
   * @param aOnSuccess The passed success callback.
   * @param aResourcePath
   *        The path of the resource for which this event was generated.
   */
    WatchedSuccessEvent(const nsMainThreadPtrHandle<
                        nsINativeFileWatcherSuccessCallback>& aOnSuccess,
                        const nsAString& aResourcePath)
        : Runnable("WatchedSuccessEvent")
        , mOnSuccess(aOnSuccess)
        , mResourcePath(aResourcePath)
    {
        MOZ_ASSERT(!NS_IsMainThread());
    }

    NS_IMETHOD Run() override
    {
        MOZ_ASSERT(NS_IsMainThread());

        // Make sure we wrap a valid callback since it's not mandatory to provide
        // one when watching a resource.
        if (mOnSuccess) {
            (void)mOnSuccess->Complete(mResourcePath);
        }

        return NS_OK;
    }

private:
    nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback> mOnSuccess;
    nsString mResourcePath;
};

/**
 * An event used to notify the main thread of a change in a watched
 * resource.
 */
class WatchedChangeEvent final : public Runnable
{
public:
    /**
   * @param aOnChange The passed change callback.
   * @param aChangedResource The name of the changed resource.
   */
    WatchedChangeEvent(
            const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
            const nsAString& aChangedResource)
        : Runnable("WatchedChangeEvent")
        , mOnChange(aOnChange)
        , mChangedResource(aChangedResource)
    {
        MOZ_ASSERT(!NS_IsMainThread());
    }

    NS_IMETHOD Run() override
    {
        MOZ_ASSERT(NS_IsMainThread());

        // The second parameter is reserved for future uses: we use 0 as a
        // placeholder.
        (void)mOnChange->Changed(mChangedResource, 0);
        return NS_OK;
    }

private:
    nsMainThreadPtrHandle<nsINativeFileWatcherCallback> mOnChange;
    nsString mChangedResource;
};


// Define these callback array types to make the code easier to read.
typedef nsTArray<nsMainThreadPtrHandle<nsINativeFileWatcherCallback>> ChangeCallbackArray;
typedef nsTArray<nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>> ErrorCallbackArray;


static mozilla::LazyLogModule gNativeWatcherPRLog("NativeFileWatcherService");
#define FILEWATCHERLOG(...)                                                    \
    MOZ_LOG(gNativeWatcherPRLog, mozilla::LogLevel::Debug, (__VA_ARGS__))


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

/**
 *  * A structure used to pass the callbacks to the AddPathRunnableMethod() and
 *   * RemovePathRunnableMethod().
 *    */
struct PathRunnablesParametersWrapper
{
    nsString mPath;
    nsMainThreadPtrHandle<nsINativeFileWatcherCallback> mChangeCallbackHandle;
    nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback> mErrorCallbackHandle;
    nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback> mSuccessCallbackHandle;

    PathRunnablesParametersWrapper(
            const nsAString& aPath,
            const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
            const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError,
            const nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback>&
            aOnSuccess)
        : mPath(aPath)
        , mChangeCallbackHandle(aOnChange)
        , mErrorCallbackHandle(aOnError)
        , mSuccessCallbackHandle(aOnSuccess)
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

}

}

#endif // NATIVEFILEWATCHERCOMMONS_H
