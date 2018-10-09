#ifndef mozilla_nativefilewatcher_h__
#define mozilla_nativefilewatcher_h__

#include "nsINativeFileWatcher.h"
#include "nsIObserver.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsThreadUtils.h"

namespace mozilla {

	class GenericFileWatcherService final : public nsINativeFileWatcherService,
		public nsIObserver
	{
	public:
		NS_DECL_ISUPPORTS
		NS_DECL_NSINATIVEFILEWATCHERSERVICE
		NS_DECL_NSIOBSERVER

		GenericFileWatcherService();

		nsresult Init();

	private:
		//need thing that allows access to file watching here
		nsCOMPtr<nsIThread> mIOThread;

		// The instance of the runnable dealing with the I/O.
		nsCOMPtr<nsIRunnable> mWorkerIORunnable;

		nsresult Uninit();
		void WakeUpWorkerThread();

		// Make the dtor private to make this object only deleted via its ::Release() method.
		~GenericFileWatcherService();
		GenericFileWatcherService(const GenericFileWatcherService& other) = delete;
		void operator=(const GenericFileWatcherService& other) = delete;
	};

} // namespace mozilla

#endif // mozilla_nativefilewatcher_h__
