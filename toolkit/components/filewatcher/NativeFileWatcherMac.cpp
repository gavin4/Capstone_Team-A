#include "NativeFileWatcherMac.h"
#include "NativeFileWatcherIOTask.h"
#include "NativeFileWatcherFSETask.h"
#include "NativeFileWatcherCommons.h"
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

#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>
#include <queue>
#include <cstring>
#include <errno.h>
#include <sys/stat.h>

#include <iostream>

#include <CoreServices/Components.k.h>

namespace mozilla {

namespace moz_filewatcher {


class NativeFileWatcherFSETask;
CallBackEvents* NativeFileWatcherFSETask::cbe_internal;

NS_IMETHODIMP
NativeFileWatcherFSETask::Run()
{
    MOZ_ASSERT(!NS_IsMainThread());

    //Making stream context for stream creation.
    auto *context = new FSEventStreamContext();/*
    context->version = 0;
    context->info = this;
    context->retain = nullptr;
    context->release = nullptr;
    context->copyDescription = nullptr;*/

    FSEventStreamEventId streamEventId = kFSEventStreamEventIdSinceNow;

    /*
    // Check if we already have an existing stream. If there is one already, we need to copy the existing watch paths from it
    // and add to our temp vector of paths before creating the new stream. There's only ever one stream, and they're immutable.
//    if (mEventStreamRef) {
//            CFArrayRef TempPath = FSEventStreamCopyPathsBeingWatched(mEventStreamRef);
//            for(int i = 0 ; i < CFArrayGetCount(TempPath); ++i){
//               mDirs.push_back((CFStringRef)CFArrayGetValueAtIndex(TempPath,i));
//            }
//            FSEventStreamStop(mEventStreamRef);
//            streamEventId = FSEventStreamGetLatestEventId(mEventStreamRef);
//            FSEventStreamInvalidate(mEventStreamRef);
//            FSEventStreamRelease(mEventStreamRef);
//            mEventStreamRef = nullptr;
//    }
*/

    // Creating an (immutable) list of paths to watch for stream creation from our (mutable) vector.
    CFArrayRef PathsToWatch = CFArrayCreate(nullptr,
                                            reinterpret_cast<const void**> (&mDirs[0]),
                                            mDirs.size(),
                                            &kCFTypeArrayCallBacks);

    // Setting flags for stream creation.
    FSEventStreamCreateFlags streamFlags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer;

    // Create the actual FS Event Stream.
    mEventStreamRef = FSEventStreamCreate(nullptr,
                                          &NativeFileWatcherFSETask::fsevents_callback,
                                          context,
                                          PathsToWatch,
                                          streamEventId,
                                          (CFAbsoluteTime)3.0,
                                          streamFlags);

    // Get and schedule the FSEvents run loop,
    mRunLoop = CFRunLoopGetCurrent();

    mParent->SetRef(mRunLoop);

    FSEventStreamScheduleWithRunLoop(mEventStreamRef,mRunLoop,kCFRunLoopDefaultMode);
    FSEventStreamStart(mEventStreamRef);

    CFRunLoopRun();

    return NS_OK;
}

void NativeFileWatcherFSETask::fsevents_callback(ConstFSEventStreamRef streamRef,
                                           void *clientCallBackInfo,
                                           size_t numEvents,
                                           void *eventPaths,
                                           const FSEventStreamEventFlags eventFlags[],
                                           const FSEventStreamEventId eventIds[])
{
    cbe_internal->callBackLock.Lock();

    std::cout << "We have " << numEvents << " events.\n";

    // Build the notification objects
    for (size_t i = 0; i < numEvents; ++i)
    {
        std::cout << "\n\nWhat we're working with:\n" << "eventFlags[" << i << "]: "
                  << eventFlags[i] << "\neventPaths[" << i << "]: " << ((char**)(eventPaths))[i] << "\n\n";

        std::cout << "<<<<<<<<<<<<<>>>>>>>>>>>>>>EventID in handler for event [" << i << "]: " << eventIds[i] << std::endl;

        if(eventFlags[i] & kFSEventStreamEventFlagItemIsFile) {
            char* singlePath = ((char**) eventPaths)[i];
            CallBackAction callBackAction(singlePath, eventFlags[i], eventIds[i]);
            cbe_internal->mSavedEvents.push(callBackAction);
        }
    }

    cbe_internal->callBackLock.Unlock();
}


CallBackEvents NativeFileWatcherIOTask::mCallBackEvents;

void
NativeFileWatcherIOTask::SetRef(CFRunLoopRef toSet) {
    childRunLoop = toSet;
}

/**
 * The watching thread logic.
 *
 * @return NS_OK if the watcher loop must be rescheduled, a failure code
 *         if it must not.
 */
nsresult
NativeFileWatcherIOTask::RunInternal()
{
    sleep(0.01);
    mCallBackEvents.callBackLock.Lock();

    //If there are no callBack events to handle we release the lock and return.
    if (mCallBackEvents.mSavedEvents.empty()){
        mCallBackEvents.callBackLock.Unlock();
        return NS_OK;
    }

    for(int i = 0; i < mCallBackEvents.mSavedEvents.size(); i++) {
        CallBackAction eventToHandle = mCallBackEvents.mSavedEvents.front();
        char testBuffer[2048];
        snprintf(testBuffer, 2048, "%s", eventToHandle.mEventPath);
        std::vector<std::string> returnedPaths = getRecursivePaths(eventToHandle.mEventPath);
        std::cout << "Returned Paths, length is: " << returnedPaths.size() << std::endl;
        for(std::string path : returnedPaths) {
            nsString resourcePath = NS_ConvertASCIItoUTF16(path.c_str());
            WatchedResourceDescriptor* changedRes = mWatchedResourcesByPath.Get(resourcePath);

            if (changedRes){
                nsString changedPath = NS_ConvertASCIItoUTF16(testBuffer);
                nsresult rv = DispatchChangeCallbacks(changedRes, changedPath);
                if (NS_FAILED(rv)) {
                    // Log that we failed to dispatch the change callbacks.
                    FILEWATCHERLOG(
                        "NativeFileWatcherIOTask::Run - Failed to dispatch change callbacks(%x).", rv);
                    return rv;
                }
            }
        }
        returnedPaths.clear();
        mCallBackEvents.mSavedEvents.pop();
    }

    mCallBackEvents.callBackLock.Unlock();

    return NS_OK;
}

/**
 * Wraps the watcher logic and takes care of rescheduling
 * the watcher loop based on the return code of |RunInternal|
 * in order to help with code readability.
 *
 * @return NS_OK or a failure error code from |NS_DispatchToCurrentThread|.
 */
NS_IMETHODIMP
NativeFileWatcherIOTask::Run()
{
    MOZ_ASSERT(!NS_IsMainThread());

    // We return immediately if |mShuttingDown| is true (see below for
    // details about the shutdown protocol being followed).
    if (mShuttingDown) {
        return NS_OK;
    }

    nsresult rv = RunInternal();
    if (NS_FAILED(rv)) {
        // A critical error occurred in the watcher loop, don't reschedule.
        FILEWATCHERLOG(
                    "NativeFileWatcherIOTask::Run - Stopping the watcher loop (error %S)",
                    rv);

        // We log the error but return NS_OK instead: we don't want to
        // propagate an exception through XPCOM.
        return NS_OK;
    }

    // No error occurred, reschedule. // FIXME: this is fucked? Maybe not.
    return NS_DispatchToCurrentThread(this); // This caused the double call, and MOZ_CRASh.
                                             // *** SO WE'RE RE-SCHEDULING AS PART OF NORMAL OPERATION. <<<<<
}

std::vector<std::string> NativeFileWatcherIOTask::getRecursivePaths(char* initialPath) {
    std::vector<std::string> pathsToReturn;
    std::vector<char*> pathComponents;
    char * pch;
    std::string aPath;

   printf ("Splitting string \"%s\" into tokens:\n", initialPath);

    pch = strtok (initialPath, "/");
    while (pch != NULL)
    {
      pathComponents.push_back(pch);
      printf ("%s\n",pch);
      pch = strtok (NULL, "/");
    }

    printf ("Second Looop\n");
    printf ("PathComponents size = %d\n", pathComponents.size());
    for (int i=0; i < pathComponents.size(); ++i) {
        aPath.append("/");
        aPath.append(pathComponents[i]);
        pathsToReturn.push_back(aPath);
        printf("A Path %d: %s\n", i, aPath.c_str());
    }
    printf ("End of the Second Looop!!!!!!\n");
    return pathsToReturn;
}

/**
 * Adds the resource to the watched list. This function is enqueued on the
 * worker thread by NativeFileWatcherService::AddPath. All the errors are
 * reported to the main thread using the error callback function mErrorCallback.
 *
 * @param pathToWatch
 *        The path of the resource to watch for changes.
 *
 * @return NS_ERROR_FILE_NOT_FOUND if the path is invalid or does not exist.
 *         Returns NS_ERROR_UNEXPECTED if OS |int|s are unexpectedly closed.
 *         If the ReadDirectoryChangesW call fails, returns NS_ERROR_FAILURE,
 *         otherwise NS_OK.
 */
nsresult
NativeFileWatcherIOTask::AddPathRunnableMethod(
        PathRunnablesParametersWrapper* aWrappedParameters)
{
    MOZ_ASSERT(!NS_IsMainThread());

    nsAutoPtr<PathRunnablesParametersWrapper> wrappedParameters(
                aWrappedParameters);

    // We return immediately if |mShuttingDown| is true (see below for
    // details about the shutdown protocol being followed).
    if (mShuttingDown) {
        return NS_OK;
    }

    // Check for valid parameters and callbacks.
    if (!wrappedParameters || !wrappedParameters->mChangeCallbackHandle) {
        FILEWATCHERLOG(
                    "NativeFileWatcherIOTask::AddPathRunnableMethod - Invalid arguments.");
        return NS_ERROR_NULL_POINTER;
    }

    // Convert path to watch to a c-string.
    char* localPath = ToNewCString(wrappedParameters->mPath);

    std::cout << "**** IN FILEWATCHER ***** Path passed in: " << localPath << std::endl;

    // Does the path exist? Notify and exit if not.
    struct stat buffer;
    if(stat(localPath, &buffer)) {
        std::cout << "Fatal error checking path: " << std::strerror(errno) << std::endl;
        FILEWATCHERLOG("NativeFileWatcherIOTask::AddPathRunnableMethod - File does not exist.");

        return NS_ERROR_FILE_NOT_FOUND;
    }

    // Is aPathToWatch already being watched?
    WatchedResourceDescriptor* watchedResource = mWatchedResourcesByPath.Get(wrappedParameters->mPath);
    if (watchedResource) {
        // If it exists, append new callbacks the hash tables.
        AppendCallbacksToHashtables(watchedResource->mPath,
                                    wrappedParameters->mChangeCallbackHandle,
                                    wrappedParameters->mErrorCallbackHandle);
        return NS_OK;
    }


    // Need a vector because the CFArray the stream requires is immutable, so we use this as a temp
    // structure to build our list of watch paths.
    std::vector<CFStringRef> dirs;

    // Add the single watch directory that was passed into this method.
    dirs.push_back(CFStringCreateWithCString(nullptr, localPath, kCFStringEncodingASCII));






//    // Check that adding the path to the inotify instance was sucessfull. Report error if not.
//    if (!mEventStreamRef) {
//        FILEWATCHERLOG("NativeFileWatcherIOTask::AddPathRunnableMethod - Fail to add watch");

//        nsresult rv =
//          ReportError(wrappedParameters->mErrorCallbackHandle, NS_ERROR_UNEXPECTED, (long)(mEventStreamRef));
//        if (NS_FAILED(rv)) {
//          FILEWATCHERLOG(
//            "NativeFileWatcherIOTask::AddPathRunnableMethod - "
//            "Failed to dispatch the error callback (%x).",
//            rv);
//          return rv;
//        }

//        return NS_ERROR_FAILURE;
//    }

    // If the run loop thread has not been created yet (which calls CFRunLoopRun()), then
    if(!mWorkerFSERunnable) {
        // Make a new thread for the MacOS watcher run loop.
        mWorkerFSERunnable = new NativeFileWatcherFSETask(this, &mCallBackEvents, dirs);
        nsresult fsResult = NS_NewNamedThread("FileWatcher FSE", getter_AddRefs(mFSEThread),
                                              mWorkerFSERunnable);
        if (NS_FAILED(fsResult)) {
            FILEWATCHERLOG("NativeFileWatcherFSETask::Init - Unable to create and dispatch the workerthread (%x).", fsResult);
            return fsResult;
        }
    }

    // Initialise the resource descriptor.
    UniquePtr<WatchedResourceDescriptor> resourceDesc(
                new WatchedResourceDescriptor(wrappedParameters->mPath));

    // Append the callbacks to the hash tables. We do this now since
    // AddDirectoryToWatchList could use the error callback, but we
    // need to make sure to remove them if AddDirectoryToWatchList fails.
    AppendCallbacksToHashtables(wrappedParameters->mPath,
                                wrappedParameters->mChangeCallbackHandle,
                                wrappedParameters->mErrorCallbackHandle);

    nsresult rv = AddDirectoryToWatchList(resourceDesc.get());
    if (NS_SUCCEEDED(rv)) {
        // Add the resource pointer to both indexes.
        WatchedResourceDescriptor* resource = resourceDesc.release();
        mWatchedResourcesByPath.Put(wrappedParameters->mPath, resource);

        // Dispatch the success callback.
        nsresult rv = ReportSuccess(wrappedParameters->mSuccessCallbackHandle,
                                    wrappedParameters->mPath);
        if (NS_FAILED(rv)) {
            FILEWATCHERLOG("NativeFileWatcherIOTask::AddPathRunnableMethod - "
                           "Failed to dispatch the success callback (%x).",
                           rv);
            return rv;
        }

        return NS_OK;
    }

    // We failed to watch the folder. Remove the callbacks
    // from the hash tables.
    RemoveCallbacksFromHashtables(wrappedParameters->mPath,
                                  wrappedParameters->mChangeCallbackHandle,
                                  wrappedParameters->mErrorCallbackHandle);

    if (rv != NS_ERROR_ABORT) {
        // Just don't add the descriptor to the watch list.
        return NS_OK;
    }

    // We failed to dispatch the error callbacks as well.
    FILEWATCHERLOG(
                "NativeFileWatcherIOTask::AddPathRunnableMethod - Failed to watch %s and"
                " to dispatch the related error callbacks",
                resourceDesc->mPath.get());

    return rv;
}

/**
 * Removes the path from the list of watched resources. Silently ignores the
 * request if the path was not being watched.
 *
 * Remove Protocol:
 *
 * 1. Find the resource to unwatch through the provided path.
 * 2. Remove the error and change callbacks from the list of callbacks
 *    associated with the resource.
 * 3. Remove the error and change callbacks from the callback hash maps.
 * 4. If there are no more change callbacks for the resource, close
 *    its file |int|. We do not free the buffer memory just yet, it's
 *    still needed for the next call to GetQueuedCompletionStatus. That
 *    memory will be freed in NativeFileWatcherIOTask::Run.
 *
 * @param aWrappedParameters
 *        The structure containing the resource path, the error and change
 * callback handles.
 */
nsresult
NativeFileWatcherIOTask::RemovePathRunnableMethod(
        PathRunnablesParametersWrapper* aWrappedParameters)
{
    MOZ_ASSERT(!NS_IsMainThread());

    nsAutoPtr<PathRunnablesParametersWrapper> wrappedParameters(
                aWrappedParameters);

    CFRunLoopStop(childRunLoop);
    nsresult rv = ReportSuccess(wrappedParameters->mSuccessCallbackHandle,
                                wrappedParameters->mPath);
/*

    // We return immediately if |mShuttingDown| is true (see below for
    // details about the shutdown protocol being followed).
    if (mShuttingDown) {
        return NS_OK;
    }

    if (!wrappedParameters || !wrappedParameters->mChangeCallbackHandle) {
        return NS_ERROR_NULL_POINTER;
    }

    WatchedResourceDescriptor* toRemove =
            mWatchedResourcesByPath.Get(wrappedParameters->mPath);
    if (!toRemove) {
        // We are trying to remove a path which wasn't being watched. Silently
        // ignore and dispatch the success callback.
        nsresult rv = ReportSuccess(wrappedParameters->mSuccessCallbackHandle,
                                    wrappedParameters->mPath);
        if (NS_FAILED(rv)) {
            FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - "
                           "Failed to dispatch the success callback (%x).",
                           rv);
            return rv;
        }
        return NS_OK;
    }

    // Hash table points to an array of callbacks.
    ChangeCallbackArray* changeCallbackArray =
            mChangeCallbacksTable.Get(toRemove->mPath);

    // This should always be valid.
    MOZ_ASSERT(changeCallbackArray);

    bool removed = changeCallbackArray->RemoveElement(
                wrappedParameters->mChangeCallbackHandle);

    if (!removed) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - Unable "
                       "to remove the change "
                       "callback from the change callback hash map for %S.",
                       wrappedParameters->mPath.get());
        MOZ_CRASH();
    }

    ErrorCallbackArray* errorCallbackArray =
            mErrorCallbacksTable.Get(toRemove->mPath);

    MOZ_ASSERT(errorCallbackArray);

    removed =
            errorCallbackArray->RemoveElement(wrappedParameters->mErrorCallbackHandle);
    if (!removed) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - Unable "
                       "to remove the error "
                       "callback from the error callback hash map for %S.",
                       wrappedParameters->mPath.get());
        MOZ_CRASH();
    }

    // If there are still callbacks left, keep the descriptor.
    // We don't check for error callbacks since there's no point in keeping
    // the descriptor if there are no change callbacks but some error callbacks.
    if (changeCallbackArray->Length()) {
        // Dispatch the success callback.
        nsresult rv = ReportSuccess(wrappedParameters->mSuccessCallbackHandle,
                                    wrappedParameters->mPath);
        if (NS_FAILED(rv)) {
            FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - "
                           "Failed to dispatch the success callback (%x).",
                           rv);
            return rv;
        }
        return NS_OK;
    }

    if (!mEventStreamRef) {
        //If the stream doesn't exist, exit quietly.
        return NS_OK;
    }

    std::vector<CFStringRef> dirs;

    const int copyBufferSize = 1024;
    char* PathtoRemove = ToNewCString(wrappedParameters->mPath);
    char copyBuffer[copyBufferSize] = {'\0'};

    //If the stream exists, continue...
    CFArrayRef TempPath = FSEventStreamCopyPathsBeingWatched(mEventStreamRef);

    for(int i = 0 ; i < CFArrayGetCount(TempPath); ++i){
        CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(TempPath,i), copyBuffer, copyBufferSize, kCFStringEncodingASCII);

        // Check to make sure that we don't copy over our path being removed.
        if(strncmp(PathtoRemove, copyBuffer, copyBufferSize)){
            dirs.push_back((CFStringRef)CFArrayGetValueAtIndex(TempPath,i));
        }
    }

    FSEventStreamStop(mEventStreamRef);
    FSEventStreamEventId streamEventId = FSEventStreamGetLatestEventId(mEventStreamRef);
    FSEventStreamInvalidate(mEventStreamRef);
    FSEventStreamRelease(mEventStreamRef);

    //Making stream context for stream creation.
    auto *context = new FSEventStreamContext();
    context->version = 0;
    context->info = this;
    context->retain = nullptr;
    context->release = nullptr;
    context->copyDescription = nullptr;

    // Creating an (immutable) list of paths to watch for stream creation from our (mutable) vector.
    CFArrayRef PathsToWatch = CFArrayCreate(nullptr,
                                            reinterpret_cast<const void**> (&dirs[0]),
                                            dirs.size(),
                                            &kCFTypeArrayCallBacks);

    // Setting flags for stream creation.
    FSEventStreamCreateFlags streamFlags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer;

    // Create the actual FS Event Stream.
    mEventStreamRef = FSEventStreamCreate(nullptr,
                                          &NativeFileWatcherIOTask::fsevents_callback,
                                          context,
                                          PathsToWatch,
                                          streamEventId,
                                          3,
                                          streamFlags);

    // TODO: We need to restart the stream here.
    // Get and schedule the FSEvents run loop,
    mRunLoop = CFRunLoopGetCurrent();
    FSEventStreamScheduleWithRunLoop(mEventStreamRef,mRunLoop,kCFRunLoopDefaultMode);
    FSEventStreamStart(mEventStreamRef);

    // Removing a watched descriptor from the inotify instance and acting if the removal was a failure
    // TODO: Do we need to call error callback here? (See addPathRunnable for same behavior, ln 525.)
    if(!mEventStreamRef){
        FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - Unable "
                       "to remove the Watch Descriptor from the inotify instance %S.",
                       wrappedParameters->mPath.get());
        return NS_ERROR_ABORT;
    }

    // Since there's no callbacks left, let's remove entries from the hash tables.
    mWatchedResourcesByPath.Remove(toRemove->mPath);

    // Dispatch the success callback.
    nsresult rv = ReportSuccess(wrappedParameters->mSuccessCallbackHandle,
                                wrappedParameters->mPath);
    if (NS_FAILED(rv)) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::RemovePathRunnableMethod - "
                       "Failed to dispatch the success callback (%x).",
                       rv);
        return rv;
    }
*/
    return NS_OK;
}

/**
 * Removes all the watched resources from the watch list and stops the
 * watcher thread. Frees all the used resources.
 */
nsresult
NativeFileWatcherIOTask::DeactivateRunnableMethod()
{
    MOZ_ASSERT(!NS_IsMainThread());
    // We return immediately if |mShuttingDown| is true (see below for
    // details about the shutdown protocol being followed).
    if (mShuttingDown) {
        // If this happens, we are in a strange situation.
        FILEWATCHERLOG("NativeFileWatcherIOTask::DeactivateRunnableMethod - We are "
                       "already shutting down.");
        MOZ_CRASH();
        return NS_OK;
    }


    // Deactivate all the non-shutdown methods of this object.
    mShuttingDown = true;



    // Clear frees the memory associated with each element and clears the table.
    // Since we are using Scoped |int|s, they get automatically closed as well.
    mWatchedResourcesByPath.Clear();

    // Now that all the descriptors are closed, release the callback hahstables.
    mChangeCallbacksTable.Clear();
    mErrorCallbacksTable.Clear();

    // Now we just need to reschedule a final call to Shutdown() back to the main
    // thread.
    // RefPtr<NativeWatcherIOShutdownTask> shutdownRunnable = //FIXME: Tegan,
    // removed to build.
    //  new NativeWatcherIOShutdownTask();

    return NS_OK; // NS_DispatchToMainThread(shutdownRunnable);
}

/**
 * Helper function to dispatch a change notification to all the registered
 * callbacks.
 * @param aResourceDescriptor
 *        The resource descriptor.
 * @param aChangedResource
 *        The path of the changed resource.
 * @return NS_OK if all the callbacks are dispatched correctly, a |nsresult|
 * error code otherwise.
 */
nsresult
NativeFileWatcherIOTask::DispatchChangeCallbacks(
        WatchedResourceDescriptor* aResourceDescriptor,
        const nsAString& aChangedResource)
{
    MOZ_ASSERT(aResourceDescriptor);

    // Retrieve the change callbacks array.
    ChangeCallbackArray* changeCallbackArray =
            mChangeCallbacksTable.Get(aResourceDescriptor->mPath);

    // This should always be valid.
    MOZ_ASSERT(changeCallbackArray);

    for (size_t i = 0; i < changeCallbackArray->Length(); i++) {
        nsresult rv = ReportChange((*changeCallbackArray)[i], aChangedResource);
        if (NS_FAILED(rv)) {
            return rv;
        }
    }

    return NS_OK;
}

/**
 * Helper function to post a change runnable to the main thread.
 *
 * @param aOnChange
 *        The change callback handle.
 * @param aChangedResource
 *        The resource name to dispatch thorough the change callback.
 *
 * @return NS_OK if the callback is dispatched correctly.
 */
nsresult
NativeFileWatcherIOTask::ReportChange(
        const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChange,
        const nsAString& aChangedResource)
{
    RefPtr<WatchedChangeEvent> changeRunnable =
            new WatchedChangeEvent(aOnChange, aChangedResource);
    return NS_DispatchToMainThread(changeRunnable);
}

/**
 * Helper function to dispatch a error notification to all the registered
 * callbacks.
 * @param aResourceDescriptor
 *        The resource descriptor.
 * @param anError
 *        The error to dispatch thorough the error callback.
 * @param anOSError
 *        An OS specific error code to send with the callback.
 * @return NS_OK if all the callbacks are dispatched correctly, a |nsresult|
 * error code otherwise.
 */
nsresult
NativeFileWatcherIOTask::DispatchErrorCallbacks(
        WatchedResourceDescriptor* aResourceDescriptor,
        nsresult anError,
        long anOSError)
{
    MOZ_ASSERT(aResourceDescriptor);

    // Retrieve the error callbacks array.
    ErrorCallbackArray* errorCallbackArray =
            mErrorCallbacksTable.Get(aResourceDescriptor->mPath);

    // This must be valid.
    MOZ_ASSERT(errorCallbackArray);

    for (size_t i = 0; i < errorCallbackArray->Length(); i++) {
        nsresult rv = ReportError((*errorCallbackArray)[i], anError, anOSError);
        if (NS_FAILED(rv)) {
            return rv;
        }
    }

    return NS_OK;
}

/**
 * Helper function to post an error runnable to the main thread.
 *
 * @param aOnError
 *        The error callback handle.
 * @param anError
 *        The error to dispatch thorough the error callback.
 * @param anOSError
 *        An OS specific error code to send with the callback.
 *
 * @return NS_OK if the callback is dispatched correctly.
 */
nsresult
NativeFileWatcherIOTask::ReportError(
        const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>& aOnError,
        nsresult anError,
        long anOSError)
{
    RefPtr<WatchedErrorEvent> errorRunnable =
            new WatchedErrorEvent(aOnError, anError, anOSError);
    return NS_DispatchToMainThread(errorRunnable);
}

/**
 * Helper function to post a success runnable to the main thread.
 *
 * @param aOnSuccess
 *        The success callback handle.
 * @param aResource
 *        The resource name to dispatch thorough the success callback.
 *
 * @return NS_OK if the cal
 lback is dispatched correctly.
 */
nsresult
NativeFileWatcherIOTask::ReportSuccess(
        const nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback>& aOnSuccess,
        const nsAString& aResource)
{
    RefPtr<WatchedSuccessEvent> successRunnable =
            new WatchedSuccessEvent(aOnSuccess, aResource);
    return NS_DispatchToMainThread(successRunnable);
}

/**
 * Instructs the OS to report the changes concerning the directory of interest.
 *
 * @param aDirectoryDescriptor
 *        A |WatchedResourceDescriptor| instance describing the directory to
 * watch.
 * @param aDispatchErrorCode
 *        If |ReadDirectoryChangesW| fails and dispatching an error callback to
 * the main thread fails as well, the error code is stored here. If the OS API
 * call does not fail, it gets set to NS_OK.
 * @return |true| if |ReadDirectoryChangesW| returned no error, |false|
 * otherwise.
 */
nsresult
NativeFileWatcherIOTask::AddDirectoryToWatchList(
        WatchedResourceDescriptor* aDirectoryDescriptor)
{
    MOZ_ASSERT(!mShuttingDown);

    long dwPlaceholder;

    // FIXME: Change to comlpy to inotify conventions
    // Tells the OS to watch out on mWatchedResourceDescriptor for the changes
    // specified with the FILE_NOTIFY_* flags. We monitor the creation, renaming
    // and deletion of a file (FILE_NOTIFY_CHANGE_FILE_NAME), changes to the last
    // modification time (FILE_NOTIFY_CHANGE_LAST_WRITE) and the creation and
    // deletion of a folder (FILE_NOTIFY_CHANGE_DIR_NAME). Moreover, when you
    // first call this function, the system allocates a buffer to store change
    // information for the watched directory.
    if (false /*!ReadDirectoryChangesW(aDirectoryDescriptor->mWatchedResourceDescriptor,
                                     aDirectoryDescriptor->mNotificationBuffer.get(),
                                     NOTIFICATION_BUFFER_SIZE,
                                     true, // watch subtree (recurse)
                                     FILE_NOTIFY_CHANGE_LAST_WRITE
                                     | FILE_NOTIFY_CHANGE_FILE_NAME
                                     | FILE_NOTIFY_CHANGE_DIR_NAME,
                                     &dwPlaceholder,
                                     &aDirectoryDescriptor->mOverlappedInfo,
                                     nullptr)*/) {

        // NOTE: GetLastError() could return ERROR_INVALID_PARAMETER if the buffer
        // length is greater than 64 KB and the application is monitoring a
        // directory over the network. The same error could be returned when trying
        // to watch a file instead of a directory. It could return ERROR_NOACCESS if
        // the buffer is not aligned on a long boundary.
        long dwError = 0; // GetLastError(); // FIXME: Tegan, removed to compile.

        // TOFIX: Log message needs to change from ReadDirectoryChangesW
        FILEWATCHERLOG("NativeFileWatcherIOTask::AddDirectoryToWatchList "
                       " - ReadDirectoryChangesW failed (error %x) for %S.",
                       dwError,
                       aDirectoryDescriptor->mPath.get());

        nsresult rv =
                DispatchErrorCallbacks(aDirectoryDescriptor, NS_ERROR_FAILURE, dwError);
        if (NS_FAILED(rv)) {
            // That's really bad. We failed to watch the directory and failed to
            // dispatch the error callbacks.
            return NS_ERROR_ABORT;
        }

        // We failed to watch the directory, but we correctly dispatched the error
        // callbacks.
        return NS_ERROR_FAILURE;
    }

    return NS_OK;
}

/**
 * Appends the change and error callbacks to their respective hash tables.
 * It also checks if the callbacks are already attached to them.
 * @param aPath
 *        The watched directory path.
 * @param aOnChangeHandle
 *        The callback to invoke when a change is detected.
 * @param aOnErrorHandle
 *        The callback to invoke when an error is detected.
 */
void
NativeFileWatcherIOTask::AppendCallbacksToHashtables(
        const nsAString& aPath,
        const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChangeHandle,
        const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>&
        aOnErrorHandle)
{
    // First check to see if we've got an entry already.
    ChangeCallbackArray* callbacksArray = mChangeCallbacksTable.Get(aPath);
    if (!callbacksArray) {
        // We don't have an entry. Create an array and put it into the hash table.
        callbacksArray = new ChangeCallbackArray();
        mChangeCallbacksTable.Put(aPath, callbacksArray);
    }

    // We do have an entry for that path. Check to see if the callback is
    // already there.
    ChangeCallbackArray::index_type changeCallbackIndex =
            callbacksArray->IndexOf(aOnChangeHandle);

    // If the callback is not attached to the descriptor, append it.
    if (changeCallbackIndex == ChangeCallbackArray::NoIndex) {
        callbacksArray->AppendElement(aOnChangeHandle);
    }

    // Same thing for the error callback.
    ErrorCallbackArray* errorCallbacksArray = mErrorCallbacksTable.Get(aPath);
    if (!errorCallbacksArray) {
        // We don't have an entry. Create an array and put it into the hash table.
        errorCallbacksArray = new ErrorCallbackArray();
        mErrorCallbacksTable.Put(aPath, errorCallbacksArray);
    }

    ErrorCallbackArray::index_type errorCallbackIndex =
            errorCallbacksArray->IndexOf(aOnErrorHandle);

    if (errorCallbackIndex == ErrorCallbackArray::NoIndex) {
        errorCallbacksArray->AppendElement(aOnErrorHandle);
    }
}

/**
 * Removes the change and error callbacks from their respective hash tables.
 * @param aPath
 *        The watched directory path.
 * @param aOnChangeHandle
 *        The change callback to remove.
 * @param aOnErrorHandle
 *        The error callback to remove.
 */
void
NativeFileWatcherIOTask::RemoveCallbacksFromHashtables(
        const nsAString& aPath,
        const nsMainThreadPtrHandle<nsINativeFileWatcherCallback>& aOnChangeHandle,
        const nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback>&
        aOnErrorHandle)
{
    // Find the change callback array for |aPath|.
    ChangeCallbackArray* callbacksArray = mChangeCallbacksTable.Get(aPath);
    if (callbacksArray) {
        // Remove the change callback.
        callbacksArray->RemoveElement(aOnChangeHandle);
    }

    // Find the error callback array for |aPath|.
    ErrorCallbackArray* errorCallbacksArray = mErrorCallbacksTable.Get(aPath);
    if (errorCallbacksArray) {
        // Remove the error callback.
        errorCallbacksArray->RemoveElement(aOnErrorHandle);
    }
}

/**
 * Creates a string representing the native path for the changed resource.
 * It appends the resource name to the path of the changed descriptor by
 * using nsIFile.
 * @param changedDescriptor
 *        The descriptor of the watched resource.
 * @param resourceName
 *        The resource which triggered the change.
 * @param nativeResourcePath
 *        The full path to the changed resource.
 * @return NS_OK if nsIFile succeeded in building the path.
 */
nsresult
NativeFileWatcherIOTask::MakeResourcePath(
        WatchedResourceDescriptor* changedDescriptor,
        const nsAString& resourceName,
        nsAString& nativeResourcePath)
{
    nsCOMPtr<nsIFile> localPath(do_CreateInstance("@mozilla.org/file/local;1"));
    if (!localPath) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::MakeResourcePath - Failed to "
                       "create a nsIFile instance.");
        return NS_ERROR_FAILURE;
    }

    nsresult rv = localPath->InitWithPath(changedDescriptor->mPath);
    if (NS_FAILED(rv)) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::MakeResourcePath - Failed to init "
                       "nsIFile with %S (%x).",
                       changedDescriptor->mPath.get(),
                       rv);
        return rv;
    }

    rv = localPath->AppendRelativePath(resourceName);
    if (NS_FAILED(rv)) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::MakeResourcePath - Failed to "
                       "append to %S (%x).",
                       changedDescriptor->mPath.get(),
                       rv);
        return rv;
    }

    rv = localPath->GetPath(nativeResourcePath);
    if (NS_FAILED(rv)) {
        FILEWATCHERLOG("NativeFileWatcherIOTask::MakeResourcePath - Failed to get "
                       "native path from nsIFile (%x).",
                       rv);
        return rv;
    }

    return NS_OK;
}


} // namespace

// The NativeFileWatcherService component

NS_IMPL_ISUPPORTS(NativeFileWatcherService,
                  nsINativeFileWatcherService,
                  nsIObserver);

NativeFileWatcherService::NativeFileWatcherService() {}

NativeFileWatcherService::~NativeFileWatcherService() {}

void
NativeFileWatcherService::signalHandler(int signal) // FIXME: DELETE THIS
{
//    if(signal == SIGIO) {
//        ++mEventWaiting;
//    }
}

/**
 * Sets the required resources and starts the watching IO thread.
 *
 * @return NS_OK if there was no error with thread creation and execution.
 */
nsresult
NativeFileWatcherService::Init()
{
    // Add an observer for the shutdown.
    nsCOMPtr<nsIObserverService> observerService =
            mozilla::services::GetObserverService();
    if (!observerService) {
        return NS_ERROR_FAILURE;
    }

    observerService->AddObserver(this, "xpcom-shutdown-threads", false);

    // Start the IO worker thread.
    mWorkerIORunnable = new mozilla::moz_filewatcher::NativeFileWatcherIOTask();
    nsresult rv = NS_NewNamedThread("FileWatcher IO", getter_AddRefs(mIOThread),
                                    mWorkerIORunnable);
    if (NS_FAILED(rv)) {
        //moz_filewatcher::FILEWATCHERLOG("NativeFileWatcherIOTask::Init - Unable to create and dispatch the workerthread (%x).", rv);
        return rv;
    }

    return NS_OK;
}

/**
 * Watches a path for changes: monitors the creations, name changes and
 * content changes to the files contained in the watched path.
 *
 * @param aPathToWatch
 *        The path of the resource to watch for changes.
 * @param aOnChange
 *        The callback to invoke when a change is detected.
 * @param aOnError
 *        The optional callback to invoke when there's an error.
 * @param aOnSuccess
 *        The optional callback to invoke when the file watcher starts
 *        watching the resource for changes.
 *
 * @return NS_OK or NS_ERROR_NOT_INITIALIZED if the instance was not
 * initialized. Other errors are reported by the error callback function.
 */
NS_IMETHODIMP
NativeFileWatcherService::AddPath(
        const nsAString& aPathToWatch,
        nsINativeFileWatcherCallback* aOnChange,
        nsINativeFileWatcherErrorCallback* aOnError,
        nsINativeFileWatcherSuccessCallback* aOnSuccess)
{
    // Make sure the instance was initialized.
    if (!mIOThread) {
        return NS_ERROR_NOT_INITIALIZED;
    }

    // Be sure a valid change callback was passed.
    if (!aOnChange) {
        return NS_ERROR_NULL_POINTER;
    }

    nsMainThreadPtrHandle<nsINativeFileWatcherCallback> changeCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherCallback>(
                    "nsINativeFileWatcherCallback", aOnChange));

    nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback> errorCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherErrorCallback>(
                    "nsINativeFileWatcherErrorCallback", aOnError));

    nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback>
            successCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherSuccessCallback>(
                    "nsINativeFileWatcherSuccessCallback", aOnSuccess));

    // Wrap the path and the callbacks in order to pass them using
    // NewRunnableMethod.
    UniquePtr<moz_filewatcher::PathRunnablesParametersWrapper> wrappedCallbacks(
                new moz_filewatcher::PathRunnablesParametersWrapper(aPathToWatch,
                                                   changeCallbackHandle,
                                                   errorCallbackHandle,
                                                   successCallbackHandle));

    // Since this function does a bit of I/O stuff , run it in the IO thread.
    nsresult rv = mIOThread->Dispatch(
                NewRunnableMethod<moz_filewatcher::PathRunnablesParametersWrapper*>(
                    "NativeFileWatcherIOTask::AddPathRunnableMethod",
                    static_cast<moz_filewatcher::NativeFileWatcherIOTask*>(mWorkerIORunnable.get()),
                    &moz_filewatcher::NativeFileWatcherIOTask::AddPathRunnableMethod,
                    wrappedCallbacks.get()),
                nsIEventTarget::DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // Since the dispatch succeeded, we let the runnable own the pointer.
    Unused << wrappedCallbacks.release();

    WakeUpWorkerThread();

    return NS_OK;
}

/**
 * Removes the path from the list of watched resources. Silently ignores the
 * request if the path was not being watched or the callbacks were not
 * registered.
 *
 * @param aPathToRemove
 *        The path of the resource to remove from the watch list.
 * @param aOnChange
 *        The callback to invoke when a change is detected.
 * @param aOnError
 *        The optionally registered callback invoked when there's an error.
 * @param aOnSuccess
 *        The optional callback to invoke when the file watcher stops
 *        watching the resource for changes.
 *
 * @return NS_OK or NS_ERROR_NOT_INITIALIZED if the instance was not
 * initialized. Other errors are reported by the error callback function.
 */
NS_IMETHODIMP
NativeFileWatcherService::RemovePath(
        const nsAString& aPathToRemove,
        nsINativeFileWatcherCallback* aOnChange,
        nsINativeFileWatcherErrorCallback* aOnError,
        nsINativeFileWatcherSuccessCallback* aOnSuccess)
{

    // Make sure the instance was initialized.
    if (!mIOThread) {
        return NS_ERROR_NOT_INITIALIZED;
    }

    // Be sure a valid change callback was passed.
    if (!aOnChange) {
        return NS_ERROR_NULL_POINTER;
    }

    nsMainThreadPtrHandle<nsINativeFileWatcherCallback> changeCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherCallback>(
                    "nsINativeFileWatcherCallback", aOnChange));

    nsMainThreadPtrHandle<nsINativeFileWatcherErrorCallback> errorCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherErrorCallback>(
                    "nsINativeFileWatcherErrorCallback", aOnError));

    nsMainThreadPtrHandle<nsINativeFileWatcherSuccessCallback> successCallbackHandle(
                new nsMainThreadPtrHolder<nsINativeFileWatcherSuccessCallback>(
                    "nsINativeFileWatcherSuccessCallback", aOnSuccess));

    // Wrap the path and the callbacks in order to pass them using NewRunnableMethod.
    UniquePtr<moz_filewatcher::PathRunnablesParametersWrapper> wrappedCallbacks(
                new moz_filewatcher::PathRunnablesParametersWrapper(
                    aPathToRemove,
                    changeCallbackHandle,
                    errorCallbackHandle,
                    successCallbackHandle));

    // Since this function does a bit of I/O stuff , run it in the IO thread.
    nsresult rv = mIOThread->Dispatch(
                NewRunnableMethod<moz_filewatcher::PathRunnablesParametersWrapper*>(
                    "NativeFileWatcherIOTask::RemovePathRunnableMethod",
                    static_cast<moz_filewatcher::NativeFileWatcherIOTask*>(mWorkerIORunnable.get()),
                    &moz_filewatcher::NativeFileWatcherIOTask::RemovePathRunnableMethod,
                    wrappedCallbacks.get()),
                nsIEventTarget::DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
        return rv;
    }

    // Since the dispatch succeeded, we let the runnable own the pointer.
    Unused << wrappedCallbacks.release();

    WakeUpWorkerThread();

    return NS_OK;
}

/**
 * Removes all the watched resources from the watch list and stops the
 * watcher thread. Frees all the used resources.
 *
 * To avoid race conditions, we need a Shutdown Protocol:
 *
 * 1. [MainThread]
 *    When the "xpcom-shutdown-threads" event is detected, Uninit() gets called.
 * 2. [MainThread]
 *    Uninit sends DeactivateRunnableMethod() to the WorkerThread.
 * 3. [WorkerThread]
 *    DeactivateRunnableMethod makes it clear to other methods that shutdown is
 *    in progress, stops the IO completion port wait and schedules the rest of
 * the deactivation for after every currently pending method call is complete.
 */
nsresult
NativeFileWatcherService::Uninit()
{
    // Make sure the instance was initialized (and not de-initialized yet).
    if (!mIOThread) {
        return NS_OK;
    }

    // We need to be sure that there will be no calls to 'mIOThread' once we have entered
    // 'Uninit()', even if we exit due to an error.
    nsCOMPtr<nsIThread> ioThread;
    ioThread.swap(mIOThread);

    // Since this function does a bit of I/O stuff (close file handle), run it
    // in the IO thread.
    nsresult rv =
            ioThread->Dispatch(
                NewRunnableMethod(
                    "NativeFileWatcherIOTask::DeactivateRunnableMethod",
                    static_cast<moz_filewatcher::NativeFileWatcherIOTask*>(mWorkerIORunnable.get()),
                    &moz_filewatcher::NativeFileWatcherIOTask::DeactivateRunnableMethod),
                nsIEventTarget::DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
        return rv;
    }

    return NS_OK;
}

/**
 * Tells |NativeFileWatcherIOTask| to quit and to reschedule itself in order to
 * execute the other runnables enqueued in the worker tread.
 * This works by posting a bogus event to the blocking
 * |GetQueuedCompletionStatus| call in |NativeFileWatcherIOTask::Run()|.
 */
void
NativeFileWatcherService::WakeUpWorkerThread()
{
    // The last 3 parameters represent the number of transferred bytes, the
    // changed resource |HANDLE| and the address of the |OVERLAPPED| structure
    // passed to GetQueuedCompletionStatus: we set them to nullptr so that we can
    // recognize that we requested an interruption from the Worker thread.
    // PostQueuedCompletionStatus(mIOCompletionPort, 0, 0, nullptr);
}

/**er
 * This method is used to catch the "xpcom-shutdown-threads" event in order
 * to shutdown this service when closing the application.
 */
NS_IMETHODIMP
NativeFileWatcherService::Observe(nsISupports* aSubject,
                                  const char* aTopic,
                                  const char16_t* aData)
{
    MOZ_ASSERT(NS_IsMainThread());

    if (!strcmp("xpcom-shutdown-threads", aTopic)) {
        DebugOnly<nsresult> rv = Uninit();
        MOZ_ASSERT(NS_SUCCEEDED(rv));
        return NS_OK;
    }

    MOZ_ASSERT(false, "NativeFileWatcherService got an unexpected topic!");

    return NS_ERROR_UNEXPECTED;
}

}
