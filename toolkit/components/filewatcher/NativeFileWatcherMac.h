#ifndef mozilla_nativefilewatcher_h__
#define mozilla_nativefilewatcher_h__

#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsThreadUtils.h"

#include "NativeFileWatcherCommons.h"
#include "NativeFileWatcherIOTask.h"
#include "NativeFileWatcherFSETask.h"

#include <CoreServices/CoreServices.h>
#include <CoreServices/Components.k.h>

namespace mozilla {

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
