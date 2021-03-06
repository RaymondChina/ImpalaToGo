/** @file cache-mgr.h
 *  @brief Definitions of cache entities managed and relevant for Cache layer.
 *
 *  @date   Sep 26, 2014
 *  @author elenav
 */

#ifndef CACHE_MGR_H_
#define CACHE_MGR_H_

#include <string>
#include <deque>
#include <utility>

#include <boost/mem_fn.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/scoped_ptr.hpp>

#include <boost/bind.hpp>

#include "util/hash-util.h"
#include "dfs_cache/cache-definitions.hpp"
#include "dfs_cache/tasks-impl.hpp"
#include "dfs_cache/sync-module.hpp"

/**
 * @namespace impala
 */
namespace impala {

/**
 * Represent Cache Manager.
 * - list of currently managed by Cache files along with their own states (mapped as cache persistence)
 * - list of currently handled by Cache Prepare Requests, therefore no concurrency to access it
 *
 * Cache Manager is the only who works with Cache metadata registry
 */
class CacheManager {
private:
	/** Singleton instance. Instantiated in init(). */
	static boost::scoped_ptr<CacheManager> instance_;

	/*********************************  Shutdown section ******************************************************************/
	bool                                    m_shutdownFlag;                 /**< global shutdown flag */

	boost::condition_variable               m_longthreadIsDoneCondition;    /**< signaled when long thread is done. */
	boost::condition_variable               m_shortthreadIsDoneCondition;   /**< signaled when short thread is done. */

	bool                                    m_longThreadIsDoneFlag;         /**< "long thread is done" flag, shutdown confirmation
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	  should be approved by dispatcher */
	bool                                    m_shortThreadIsDoneFlag;        /**< "short thread is done" flag, shutdown confirmation
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	  should be approved by dispatcher */

	boost::mutex                            m_longThreadIsDoneMux;          /**< protects the "long thread is done" flag */
	boost::mutex                            m_shortThreadIsDoneMux;         /**< protects the "short thread is done" flag */

	/*********************************************************************************************************************/

	CacheLayerRegistry*                     m_registry;            /**< reference to metadata registry instance */

	ClientRequests                          m_activeHighRequests;  /**< Set of highly prioritized client requests currently managed by module.
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     * contains either "Pending" or "In progress" requests.
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     * This set is the subjects pool for scheduling to a thread pool
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     */
	ClientRequests                          m_activeLowRequests;   /**< Set of low-prioritized client requests currently managed by module.
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     * contains either "Pending" or "In progress" requests.
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     * This set is the subjects pool for scheduling to a thread pool
	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	     */
	ClientRequests                          m_syncRequestsQueue;    /**< Set of synhrounous requests */

	HistoryOfRequests                       m_HistoryRequests;      /**< Set of client requests moved to history. */

	boost::shared_ptr<Sync>                 m_syncModule;       /**< sync module reference. */

	boost::mutex                            m_highrequestsMux;  /**< mutex to protect high active client requests */
	boost::mutex                            m_lowrequestsMux;   /**< mutex to protect active client requests */
	boost::mutex                            m_HistoryMux;       /**< mutex to protect history of client requests */

	boost::mutex                            m_SyncRequestsMux;  /**< mutex to protect sync requests queue **/

	boost::condition_variable               m_controlHighRequestsArrival; /**< condition variable to control new arrivals of high requests */
	boost::condition_variable               m_controlLowRequestsArrival;  /**< condition variable to control new arrivals of low requests */

	dfsThreadPool                           m_longpool;         /**< thread pool for long running async operations */
	dfsThreadPool                           m_shortpool;        /**< thread pool for fast running async operations */

	boost::scoped_ptr<Thread>               m_HighPriorityQueueThread;  /**< Thread handling high priority queue */
	boost::scoped_ptr<Thread>               m_LowPriorityQueueThread;   /**< Thread handling low priority queue */

	/**
	 * Ctor. Subscribe to Sync's completion routines and pass the credentials mapping to Sync module.
	 */
	CacheManager() : m_syncModule(new Sync()),
			m_longpool("CacheManagementLong", "LongRunningClientRequestsPool", 4, 4,
			          boost::bind<void>(boost::mem_fn(&CacheManager::dispatcherLowProc), this, _1, _2)),
		    m_shortpool("CacheManagementShort", "FastRunningClientRequestsPool", 4, 4,
		              boost::bind<void>(boost::mem_fn(&CacheManager::dispatcherHighProc), this, _1, _2)){

		  // Run 2 requests dispatch threads.
		  // For high prioritized tasks such as "Estimate dataset" task
		  m_HighPriorityQueueThread.reset(new Thread("cache-layer",
		        "cache-layer-high-priority-queue-thread",
		        &CacheManager::dispatchRequest, this, requestPriority::HIGH));

		  // For low prioritized tasks such as "Download dataset" task
		  m_LowPriorityQueueThread.reset(new Thread("cache-layer",
		          "cache-layer-low-priority-queue-thread",
		          &CacheManager::dispatchRequest, this, requestPriority::LOW));
	}

	CacheManager(CacheManager const& l);            // disable copy constructor
	CacheManager& operator=(CacheManager const& l); // disable assignment operator


	/** generic request dispatcher's pool's thread function */
	void dispatcherProc(dfsThreadPool* pool, int threadnum, const boost::shared_ptr<request::Task>& task );

	/** high request dispatcher's pool's thread function */
	void dispatcherHighProc(int threadnum, const boost::shared_ptr<request::Task>& task) { dispatcherProc(&m_shortpool, threadnum, task); }

	/* low request dispatcher's pool's thread function */
	void dispatcherLowProc(int threadnum, const boost::shared_ptr<request::Task>& task) { dispatcherProc(&m_longpool, threadnum, task); }

	/**
	 * This routine will be invoked after the task created basing on user's request finished its work.
	 * Supposed to manage internal data structures representing history and pending/progress collections.
	 *
	 * @param requestIdentity - request identity
	 * @param fsDescriptor    - affected fs focal point
	 * @param canceled        - flag, indicates whether request was canceled
	 * @param async           - flag, indicates that the request is async.
	 * @param priority        - request priority to locate it on correct queue
	 *
	 * @return overall status
	 */
	void finalizeUserRequest(const requestIdentity& requestIdentity,
			const FileSystemDescriptor & fsDescriptor, requestPriority priority, bool canceled = false, bool async = true );

	/** Run finalization on the specified requests queue
	 *
	 * @param requests - queue of requests to finalize
	 * @param mux      - mutex to protect the queue access
	 */
	template<typename IndexType_, typename ItemType_>
	void finalize(IndexType_* requests, boost::mutex* mux);

	/** dispatch user request to the further processing basing on @a priority
	 * @param - priority - request priority
	 */
	void dispatchRequest(requestPriority priority);

public:

       ~CacheManager() {
    	   // run all finalizations here
    	   shutdown();
       }

       static CacheManager* instance() { return CacheManager::instance_.get(); }

       /** Initialize Cache Manager. Call this before any Cache Manager usage */
       static void init();

       /**
        * Subscribe to cache registry as one of owners.
        *
        * @param registry - cache registry
        */
       status::StatusInternal configure() {
    	   // become one of owners of the arrived registry:
    	   m_registry = CacheLayerRegistry::instance();
    	   // pass the registry reference to sync module:
    	   return m_syncModule->init();
       }

       /**
        * Shutdown cache manager.
        *
        * @param force - flag, indicates whether all work in progress should be forcelly interrupted.
        * If false, all work in progress will be completed.
        * If true, all work will be cancelled.
        *
        * @param updateClients - flag, indicates whether completion callbacks should be invoked on
        * pending clients
        */
       status::StatusInternal shutdown(bool force = true, bool updateClients = true);

        /**
        * @fn Status cacheEstimate(clusterId cluster, std::list<const char*> files, time_t* time)
        * @brief For files from the list @a files, check whether all files can be accessed locally,
        *        estimate the time required to get locally all files from the list if any specified
        *        file is not available locally yet.
        *
        *        Internally, this call is divided to phases:
        *        - check the cache persistence which files we already have.
        *        - for files that are not locally, invoke sync to estimate the time to get them -
        *        per file
        *        - aggregate estimations reported by Sync for each file and reply to client.
        *
        *        TODO: whether this operation async? If so - need to add the SessionContext here and the callback.
        *
        * @param[In]  fsDescriptor - fs connection details
        * @param[In]  files        - List of files required to be locally.
        * @param[Out] time         - time required to get all requested files locally (if any).
        * 							 Zero time means all data is in place
        *
        * @param[In]  callback        - callback that should be invoked on completion in case if async mode is selected
        * @param[Out] requestIdentity - request identity assigned to this request, should be used to poll it for progress later.
        * @param[In]  async       - if true, the callback should be passed as well in order to be called on the operation completion.
        *
        * @return Operation status. If either file is not available in specified @a cluster
        * the status will be "Canceled"
        */
       status::StatusInternal cacheEstimate(SessionContext session, const FileSystemDescriptor & fsDescriptor, const DataSet& files, time_t& time,
    		   CacheEstimationCompletedCallback callback, requestIdentity & requestIdentity, bool async = true);

       /**
        * @fn Status cachePrepareData(SessionContext session, clusterId cluster, std::list<const char*> files)
        * @brief Run load scenario from specified files list @a files from the @a cluster.
        *
        *       Internally, this call is divided to phases:
        *       - create Prepare Request. Filter out files that are already locally.
        *       - for each file which is not marked as "local"  or "in progress" in persistence,
        *       mark file with "in progress" and run Sync to download it.
        *       - in callback from Sync, decrement number of remained files to prepare - only in case if
        *       consequential download finished successfully. Update persistence.
        *       - if any file download operation got failure report from Sync, Prepare Request should be marked as failed
        *       and failure immediately reported to caller (coordinator) with detail per file.
        *       - Once number of remained files in Prepare Request becomes 0, report the final callback
        *       to the caller (coordinator) with the overall status.
        * @param[In]  session     - Request session id.
        * @param[In]  namenode    - namenode connection details
        * @param[In]  files       - List of files required to be locally.
        * @param[Out] callback    - callback to invoke when prepare is finished (whatever the status).
        *
        * @param[Out] requestIdentity - request identity assigned to this request, should be used to poll it for progress later.
        *
        * @return Operation status
        */
       status::StatusInternal cachePrepareData(SessionContext session, const FileSystemDescriptor & fsDescriptor,
    		   const DataSet& files,
    		   PrepareCompletedCallback callback, requestIdentity & requestIdentity);

       /**
        * @fn Status cacheCancelPrepareData(SessionContext session)
        * @brief cancel prepare data request
        *
        * @param[In] requestIdentity - request identity assigned to this request,
        *
        * @return Operation status
        */
       status::StatusInternal cacheCancelPrepareData(const requestIdentity & requestIdentity);

       /**
        * @fn Status cachePrepareData(SessionContext session, clusterId cluster, std::list<const char*> files)
        * @brief Run load scenario from specified files list @a files from the @a cluster.
        *
        * @param[In]   requestIdentity - request identity assigned to this request,
        * @param[Out]  progress        - Detailed prepare progress. Can be used to present it to the user.
        * @param[Out]  performance     - to hold request current performance statistic
        *
        * @return Operation status
        */
       status::StatusInternal cacheCheckPrepareStatus(const requestIdentity & requestIdentity,
    		   std::list<boost::shared_ptr<FileProgress> >& progress, request_performance& performance );

};
} /** namespace impala */


#endif /* CACHE_MGR_H_ */
