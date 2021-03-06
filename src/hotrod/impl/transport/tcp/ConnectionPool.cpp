#include "hotrod/impl/transport/tcp/ConnectionPool.h"
#include <hotrod/sys/Log.h>

namespace infinispan {
namespace hotrod {
namespace transport {

void ConnectionPool::addObject(const InetSocketAddress& key) {
    sys::ScopedLock<sys::Mutex> l(lock);

    if (idle.find(key) != idle.end()) {
        return; //key already existed.
    }
    TransportQueuePtr idleQ(new BlockingQueue<TcpTransport *>()); // = TransportQueue(BlockingQueue<TcpTransport *>* idleQ = new BlockingQueue<TcpTransport *>();
    idle.insert(std::pair<InetSocketAddress, TransportQueuePtr>(key, idleQ));
    TransportQueuePtr busyQ(new BlockingQueue<TcpTransport *>()); // = TransportQueue(BlockingQueue<TcpTransport *>* idleQ = new BlockingQueue<TcpTransport *>();
    busy.insert(std::pair<InetSocketAddress, TransportQueuePtr>(key, busyQ));
    ensureMinIdle(key);
}

void ConnectionPool::ensureMinIdle(const InetSocketAddress& key) {
	int grown = calculateMinIdleGrow(key);
	while (grown > 0) {
		idle[key]->push(&factory->makeObject(key));
		grown--;
		totalIdle++;
	}
}

int ConnectionPool::calculateMinIdleGrow(const InetSocketAddress& key) {
	TransportQueuePtr idleQ = idle[key];
	int grown = configuration.getMinIdle() - idleQ->size();
	//Note: if we need to check maxActive, uncomment the code above
	/*if (configuration.getMaxActive() > 0) {
		int growLimit = std::max(0, configuration.getMaxActive() - (int) busy[key]->size() - (int) idleQ->size());
		grown = std::min(grown, growLimit);
	}*/
	if (configuration.getMaxTotal() > 0) {
		int growLimit = std::max(0, configuration.getMaxTotal() - totalIdle - totalActive);
		grown = std::min(grown, growLimit);
	}
	return grown;
}

bool ConnectionPool::hasReachedMaxTotal() {
	int maxTotal = configuration.getMaxTotal();
	return maxTotal > 0 && (maxTotal <= totalActive + totalIdle);
}

bool ConnectionPool::tryRemoveIdle() {
	//first, if we find an idle queue with more the minIdle connection, we destroy one of them.
	//otherwise, we try to destroy and connection from the longer idle queue.
	const int minIdle = configuration.getMinIdle();

	do {
		const InetSocketAddress* keyToRemove = NULL;
		int longerQueueSize = 0;

		for (std::map<InetSocketAddress, TransportQueuePtr>::iterator it = idle.begin(); it != idle.end(); ++it) {
			TransportQueuePtr idleQ = it->second;
			if (minIdle > 0 && (int) idleQ->size() > minIdle) {
				keyToRemove = &it->first;
				break;
			} else if ((int) idleQ->size() > longerQueueSize) {
				keyToRemove = &it->first;
				longerQueueSize = idleQ->size();
			}
		}

		if (keyToRemove != NULL) {
			TcpTransport* t = NULL;
			if (idle[*keyToRemove]->poll(t)) { //in case of concurrent removal, avoid blocking
				factory->destroyObject(*keyToRemove, *t);
				totalIdle--;
				return true;
			}
		} else {
			return false;
		}
	} while (true);
}

TcpTransport& ConnectionPool::borrowObject(const InetSocketAddress& key) {
	sys::ScopedLock<sys::Mutex> l(lock);

    if (closed) {
        throw HotRodClientException("Pool is closed");
    }
    if (!idle.count(key) || !busy.count(key)) {
        throw HotRodClientException("Pool has no idle or no busy transports.");
    }
    TransportQueuePtr idleQ = idle[key];
    TransportQueuePtr busyQ = busy[key];

    // See if an object is readily available
    TcpTransport* obj = NULL;
    bool ok = idleQ->poll(obj);
    if (ok) {
        totalIdle--;
    }

    for (;;) {
        if (ok) {
            // Check if the object is still valid, if not destroy it
            if (configuration.isTestOnBorrow() && !factory->validateObject(key, *obj)) {
                factory->destroyObject(key, *obj);
                ok = false;
            }
            // We have a valid object
            if (ok) {
                busyQ->push(obj);
                totalActive++;
                break;
            }
        }
        // See if we can create a new one
        if (idleQ->size() == 0 && //the idle queue is empty
                (configuration.getMaxActive() < 0 || busyQ->size() < (size_t) configuration.getMaxActive()) && //max active not reached!
				!hasReachedMaxTotal()) {
            obj = &factory->makeObject(key);
		} else if (hasReachedMaxTotal()) {
			//max total reached. try to destroy a existing idle connection. if not possible, wait until some other connection is available.
			if (tryRemoveIdle()) { //removal successful.
				obj = &factory->makeObject(key);
			} else {
				allocationQueue.push(key);
				{
					sys::ScopedUnlock<sys::Mutex> u(lock);
					obj = idleQ->pop();
				}
				totalIdle--;
			}
		} else {
			// Wait for an object to become idle
			{
				sys::ScopedUnlock<sys::Mutex> u(lock);
				obj = idleQ->pop();
			}
			totalIdle--;
		}
		ok = true;
    }
    factory->activateObject(key, *obj);
    return *obj;
}

void ConnectionPool::invalidateObject(const InetSocketAddress& key, TcpTransport* val) {
    sys::ScopedLock<sys::Mutex> l(lock);
    bool maxTotalReached = hasReachedMaxTotal();
    if (val != NULL) {
		// Remove from busy queue
		std::map<InetSocketAddress, TransportQueuePtr>::iterator busyIt = busy.find(key);
		if (busyIt == busy.end()) {
			throw HotRodClientException("No busy queue for address!");
		}

		busyIt->second->remove(val);
		totalActive--;

		if (maxTotalReached && !allocationQueue.empty()) {
			InetSocketAddress keyToAllocate = allocationQueue.front();
			allocationQueue.pop(); //front does not remove it...
			//we need to allocate a new connection for other key.
			idle[keyToAllocate]->push(&factory->makeObject(keyToAllocate));
			totalIdle++;
		}

		// Destroy object
		factory->destroyObject(key, *val);
    }
}

void ConnectionPool::returnObject(const InetSocketAddress& key, TcpTransport& val) {
    sys::ScopedLock<sys::Mutex> l(lock);
    bool ok = true;
    bool maxTotalReached = hasReachedMaxTotal();

    // Remove the object from the busy queue
    if (busy.count(key)) {
        busy[key]->remove(&val);
        totalActive--;
    }

    // If necessary validate the object, then passivate it
    if (closed || (configuration.isTestOnReturn() && !factory->validateObject(key, val))) {
        ok = false;
    } else {
        factory->passivateObject(key, val);
    }

    if (maxTotalReached && !allocationQueue.empty()) {
        InetSocketAddress keyToAllocate = allocationQueue.front();
        allocationQueue.pop(); //front does not remove it...
        //we need to allocate a new connection for other key.
        idle[keyToAllocate]->push(&factory->makeObject(keyToAllocate));
        totalIdle++;
        ok = false; //we need to destroy the object
    }

    if (ok) {
        // The object is still valid, see if the idle queue wants it
        if (idle.count(key)) {
            if ((ok = idle[key]->offer(&val))) {
                totalIdle++;
            }
        } else {
            ok = false;
        }
    }

    if (!ok) {
        // The object is now useless
        factory->destroyObject(key, val);
    }
}

void ConnectionPool::clear() {
    sys::ScopedLock<sys::Mutex> l(lock);
    clear(idle);
    clear(busy);
    totalIdle = 0;
    totalActive = 0;
}

void ConnectionPool::clear(std::map<InetSocketAddress, TransportQueuePtr>& queue) {
    for (std::map<InetSocketAddress, TransportQueuePtr>::iterator it = queue.begin(); it != queue.end(); ++it) {
        clear(it->first, it->second);
    }
    queue.clear();
}

void ConnectionPool::clear(const InetSocketAddress& key) {
    sys::ScopedLock<sys::Mutex> l(lock);
    TransportQueuePtr idleQ = idle[key];
    totalIdle -= idleQ->size();
    clear(key, idleQ);
    idle.erase(key);
}

void ConnectionPool::clear(const InetSocketAddress& key, TransportQueuePtr queue) {
    while (queue->size() > 0) {
        TcpTransport* transport = queue->pop();
        factory->destroyObject(key, *transport);
    }
}

void ConnectionPool::checkIdle() {
    DEBUG("Ensuring idle connection limits");
    // TODO
}

void ConnectionPool::testIdle() {
    DEBUG("Testing idle connections");
    // TODO
}

void ConnectionPool::preparePool(const InetSocketAddress& key) {
    addObject(key);
}

void ConnectionPool::close() {
    closed = true;
    sys::ScopedLock<sys::Mutex> l(lock);
    clear();
}

void PoolWorker::run() {
    long totalTime = pool->getConfiguration().getTimeBetweenEvictionRuns();
    if (totalTime <= 0) {
        // When non-positive, no eviction thread will be launched.
        return;
    }

    while(!pool->closed) {
        pool->checkIdle();
        pool->testIdle();
        // Sleep in 1 second bursts to let us be cancellable
        for(long t = 0; t < totalTime && !pool->closed; t+=1000) {
            sys::Thread::sleep(1000);
        }
    }
}

}}}

