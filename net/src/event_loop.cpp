
#include "default_event_handler.h"
#include "event_loop.h"
#include <thread>

#ifdef HAVE_EPOLL
    #include "va_epoll.h"
#else
    #ifdef HAVE_KQUEUE
        #include "va_kqueue.h"
    #else
        #include "va_select.h"
    #endif
#endif

namespace valois {
namespace net {

EventLoop::EventLoop()
        : _event_size(0)
        , _stop(true)
        , poll(new Poll())
        , _event_handler_maps(new std::map<int, EventHandler *>)
        , fired(nullptr)
{
    _wakeup_fds[0] = -1;
    _wakeup_fds[1] = -1;
}

bool EventLoop::CreateEventLoop(int setEventSize) {
    _event_size = setEventSize;
    if (pipe(_wakeup_fds) != 0) {
        return false;
    }
    if (poll == nullptr || -1 == poll->PollCreate(this)) {
        return false;
    }
    int listen_fd = DefaultEventHandler::BindSocket();
    AddFdPoll(listen_fd, VA_READABLE | VA_WRITABLE | VA_ET,  new DefaultEventHandler(listen_fd));

    Start();
    return true;
}

EventLoop::~EventLoop() {
    Stop();
    Join();
    if (_wakeup_fds[0] > 0) {
        close(_wakeup_fds[0]);
        close(_wakeup_fds[1]);
    }
}

bool EventLoop::Start() {
    int rc = pthread_create(&_tid, NULL, &Run, this);
    if(rc) {
        return false;
    }
    _stop.store(false, std::memory_order_release);
    return true;
}

bool EventLoop::Join() {
    if(_tid != 0) {
        pthread_join(_tid, NULL);
        _tid = 0;
    }
    return true;
}

bool EventLoop::Stop() {
    if(false == poll->Stop(_wakeup_fds[1])) {
        return false;
    }
    _stop.store(true, std::memory_order_release);
    return true;
}

int EventLoop::AddFdPoll(int fd, int mask, EventHandler *event_handler) {
    RegisterFdEventHandler(fd, event_handler);
    if(-1 == poll->PollAddEvent(this, fd, mask)) {
        return -1;
    }
    return 0;
}

int EventLoop::DelFdPoll(int fd, int mask) {
    RemoveFdEventHandler(fd);
    if(-1 == poll->PollDelEvent(this, fd, mask)) {
        return -1;
    }
    return 0;
}

int EventLoop::RegisterFdEventHandler(int fd, EventHandler *event_handler) {
    _event_handler_maps->insert(std::pair<int, EventHandler *>(fd, event_handler));
    return 0;
}

int EventLoop::RemoveFdEventHandler(int fd) {
    auto it = _event_handler_maps->find(fd);
    if (it != _event_handler_maps->end()) {
        _event_handler_maps->erase(it);
    }
    return 0;
}

void EventLoop::GetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;

    gettimeofday(&tv, nullptr);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

int EventLoop::ProcessEvents(EventLoop *eventLoop, int flags) {
    int processed = 0, num_events = 0;
    if (!(flags & VA_TIME_EVENTS) && !(flags & VA_FILE_EVENTS)) return -1;

    if (((flags & VA_TIME_EVENTS) && !(flags & VA_DONT_WAIT))) {
        int j;
        TimeEvent *shortest = nullptr;
        struct timeval tv, *tvp;

        /*if (flags & VA_TIME_EVENTS && !(flags & VA_DONT_WAIT)) {
            shortest = SearchNearestTimer(eventLoop);
        }*/

        if (shortest) {
            long now_sec, now_ms;

            GetTime(&now_sec, &now_ms);
            tvp = &tv;

            /* How many milliseconds we need to wait for the next
             * time event to fire? */
            long long ms =
                    (shortest->when_sec - now_sec)*1000 +
                    shortest->when_ms - now_ms;

            if (ms > 0) {
                tvp->tv_sec = ms/1000;
                tvp->tv_usec = (ms % 1000)*1000;
            } else {
                tvp->tv_sec = 0;
                tvp->tv_usec = 0;
            }
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to set the timeout
             * to zero */
            if (flags & VA_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = nullptr; /* wait forever */
            }
        }

        /* Call the multiplexing API, will return only on timeout or when
         * some event fires. */
        num_events = poll->PollWaitEvent(eventLoop, tvp);

        /* After sleep callback. */
        /*if (eventLoop->aftersleep != NULL && flags & VA_CALL_AFTER_SLEEP) {
            eventLoop->aftersleep(eventLoop);
        }*/

        for (j = 0; j < num_events; j++) {
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            auto it = _event_handler_maps->find(fd);
            if(it == _event_handler_maps->end()) {
                continue;
            }
            EventHandler *handler = it->second;
            if (mask & VA_READABLE) {
                handler->ReadEvent(fd, (void *)eventLoop, mask);
            }
            if (mask & VA_WRITABLE) {
                handler->WriteEvent(fd, (void *)eventLoop, mask);
            }
            ++processed;
        }
    }
    if(_stop.load(std::memory_order_acquire)) { return false; }
    /* Check time events */
    /*if (flags & VA_TIME_EVENTS) {
        processed += processTimeEvents(eventLoop);
    }*/
    return processed; /* return the number of processed file/time events */
}

void *EventLoop::Run(void *args) {
    EventLoop *eventLoop = static_cast<EventLoop *>(args);
    while(!eventLoop->_stop.load(std::memory_order_acquire)) {
        if(-1 == eventLoop->ProcessEvents(eventLoop, VA_ALL_EVENTS | VA_CALL_AFTER_SLEEP)) {
            break;
        }
    }
    return nullptr;
}

EventLoop* loop;
std::once_flag ONCE_FLAG;

void GlobalInit() {
    loop = new EventLoop();
    if(nullptr == loop) {
        exit(-1);
    }
    loop->CreateEventLoop(128);
}

EventLoop* GetGlobalLoop() {
    std::call_once(ONCE_FLAG, GlobalInit);
    return loop;
}

/*static TimeEvent *SearchNearestTimer(EventLoop *eventLoop) {
    return nullptr;
}*/

/*
long long CreateTimeEvent(EventLoop *eventLoop, long long milliseconds,
                            TimeProc *proc, void *clientData,
                            EventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++;
    TimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

int DeleteTimeEvent(EventLoop *eventLoop, long long id) {
    TimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = VA_DELETED_EVENT_ID;
            return VA_OK;
        }
        te = te->next;
    }
    return 0; // NO event with the specified ID found
}

static TimeEvent *SearchNearestTimer(EventLoop *eventLoop) {
    TimeEvent *te = eventLoop->timeEventHead;
    TimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec &&
             te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}
*/

}
}