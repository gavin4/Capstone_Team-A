#ifndef mozilla_nativefilewatcher_h__
#define mozilla_nativefilewatcher_h__

#include "nsINativeFileWatcher.h"
#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsThreadUtils.h"
#include <CoreServices/CoreServices.h>

namespace mozilla {

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
