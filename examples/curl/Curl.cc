#include <examples/curl/Curl.h>
#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <boost/bind.hpp>

#include <curl/curl.h>
#include <assert.h>

using namespace curl;
using namespace muduo;
using namespace muduo::net;

static void dummy(const boost::shared_ptr<Channel>&)
{
}

Request::Request(Curl* owner, const char* url)
  : owner_(owner),
    curl_(CHECK_NOTNULL(curl_easy_init()))
{
  setopt(CURLOPT_URL, url);
  setopt(CURLOPT_WRITEFUNCTION, &Request::writeData);
  setopt(CURLOPT_WRITEDATA, this);
  setopt(CURLOPT_HEADERFUNCTION, &Request::headerData);
  setopt(CURLOPT_HEADERDATA, this);
  setopt(CURLOPT_PRIVATE, this);
  setopt(CURLOPT_USERAGENT, "curl");
  // set useragent
  LOG_DEBUG << curl_ << " " << url;
  // add an easy handle to a multi session
  curl_multi_add_handle(owner_->getCurlm(), curl_);
}

Request::~Request()
{
  assert(!channel_ || channel_->isNoneEvent());
  // Removes a given easy_handle from the multi_handle.
  curl_multi_remove_handle(owner_->getCurlm(), curl_);

  // This function must be the last function to call for an easy session.
  // It is the opposite of the curl_easy_init function
  //   and must be called with the same handle as input that the curl_easy_init call returned.
  curl_easy_cleanup(curl_);
}

// NOT implemented yet
//
// void Request::allowRedirect(int redirects)
// {
//   setopt(CURLOPT_FOLLOWLOCATION, 1);
//   setopt(CURLOPT_MAXREDIRS, redirects);
// }

void Request::headerOnly()
{
  setopt(CURLOPT_NOBODY, 1);
}

// should be in the format "X-Y" or "X-Y,N-M", where either X or Y may be left out and X and Y are byte indexes.
void Request::setRange(const StringArg range)
{
  setopt(CURLOPT_RANGE, range.c_str());
}

const char* Request::getEffectiveUrl()
{
  const char* p = NULL;
  curl_easy_getinfo(curl_, CURLINFO_EFFECTIVE_URL, &p);
  return p;
}

const char* Request::getRedirectUrl()
{
  const char* p = NULL;
  curl_easy_getinfo(curl_, CURLINFO_REDIRECT_URL, &p);
  return p;
}

int Request::getResponseCode()
{
  long code = 0;
  curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
  return static_cast<int>(code);
}

Channel* Request::setChannel(int fd)
{
  assert(channel_.get() == NULL);
  channel_.reset(new Channel(owner_->getLoop(), fd));
  channel_->tie(shared_from_this());
  return get_pointer(channel_);
}

void Request::removeChannel()
{
  channel_->disableAll();
  channel_->remove();
  owner_->getLoop()->queueInLoop(boost::bind(dummy, channel_));
  channel_.reset();
}

void Request::done(int code)
{
  if (doneCb_)
  {
    doneCb_(this, code);
  }
}

void Request::dataCallback(const char* buffer, int len)
{
  if (dataCb_)
  {
    dataCb_(buffer, len);
  }
}

void Request::headerCallback(const char* buffer, int len)
{
  if (headerCb_)
  {
    headerCb_(buffer, len);
  }
}

size_t Request::writeData(char* buffer, size_t size, size_t nmemb, void* userp)
{
  assert(size == 1);
  Request* req = static_cast<Request*>(userp);
  req->dataCallback(buffer, static_cast<int>(nmemb));
  return nmemb;
}

size_t Request::headerData(char* buffer, size_t size, size_t nmemb, void* userp)
{
  assert(size == 1);
  Request* req = static_cast<Request*>(userp);
  req->headerCallback(buffer, static_cast<int>(nmemb));
  return nmemb;
}

// ==================================================================

void Curl::initialize(Option opt)
{
  curl_global_init(opt == kCURLnossl ? CURL_GLOBAL_NOTHING : CURL_GLOBAL_SSL);
}


// Function matching the curl_socket_callback prototype
// @userp is set by CURLMOPT_SOCKETDATA
// @socketp is set by curl_multi_assign
int Curl::socketCallback(CURL* c, int fd, int what, void* userp, void* socketp)
{
  Curl* curl = static_cast<Curl*>(userp);
  const char *whatstr[]={ "none", "IN", "OUT", "INOUT", "REMOVE" };
  LOG_DEBUG << "Curl::socketCallback [" << curl << "] - fd = " << fd
            << " what = " << whatstr[what];
  Request* req = NULL;
  // setted by setopt(CURLOPT_PRIVATE, this)
  curl_easy_getinfo(c, CURLINFO_PRIVATE, &req);
  assert(req->getCurl() == c);
  if (what == CURL_POLL_REMOVE)
  {
    muduo::net::Channel* ch = static_cast<Channel*>(socketp);
    assert(req->getChannel() == ch);
    req->removeChannel();
    ch = NULL;
    curl_multi_assign(curl->curlm_, fd, ch);
  }
  else
  {
    muduo::net::Channel* ch = static_cast<Channel*>(socketp);
    if (!ch)
    {
      ch = req->setChannel(fd);
      ch->setReadCallback(boost::bind(&Curl::onRead, curl, fd));
      ch->setWriteCallback(boost::bind(&Curl::onWrite, curl, fd));
      ch->enableReading();
      // set data to association with an internal socket
      curl_multi_assign(curl->curlm_, fd, ch);
      LOG_TRACE << "new channel for fd=" << fd;
    }
    assert(req->getChannel() == ch);
    // update
    if (what & CURL_POLL_OUT)
    {
      ch->enableWriting();
    }
    else
    {
      ch->disableWriting();
    }
  }
  return 0;
}

// Function matching the curl_multi_timer_callback prototype
// The userp argument is set by CURLMOPT_TIMERDATA
int Curl::timerCallback(CURLM* curlm, long ms, void* userp)
{
  Curl* curl = static_cast<Curl*>(userp);
  LOG_DEBUG << curl << " " << ms << " ms";
  curl->loop_->runAfter(static_cast<int>(ms)/1000.0, boost::bind(&Curl::onTimer, curl));
  return 0;
}

// curl_multi_init() returns CURLM handle to be used as input to all the other multi-functions
Curl::Curl(EventLoop* loop)
  : loop_(loop),
    curlm_(CHECK_NOTNULL(curl_multi_init())),
    runningHandles_(0),
    prevRunningHandles_(0)
{
  curl_multi_setopt(curlm_, CURLMOPT_SOCKETFUNCTION, &Curl::socketCallback);
  curl_multi_setopt(curlm_, CURLMOPT_SOCKETDATA, this);
  curl_multi_setopt(curlm_, CURLMOPT_TIMERFUNCTION, &Curl::timerCallback);
  curl_multi_setopt(curlm_, CURLMOPT_TIMERDATA, this);
}

Curl::~Curl()
{
  // Cleans up and removes a whole multi stack. It does not free or touch any individual [easy handles] in any way
  curl_multi_cleanup(curlm_);
}

RequestPtr Curl::getUrl(StringArg url)
{
  RequestPtr req(new Request(this, url.c_str()));
  return req;
}

void Curl::onTimer()
{
  CURLMcode rc = CURLM_OK;
  do {
    LOG_TRACE;
    // Get the timeout time by setting the CURLMOPT_TIMERFUNCTION option with curl_multi_setopt.
    // Your application will then get called with information on how long to wait for socket actions at most before doing the timeout action:
    //   call the curl_multi_socket_action function with the sockfd argument set to CURL_SOCKET_TIMEOUT.
    rc = curl_multi_socket_action(curlm_, CURL_SOCKET_TIMEOUT, 0, &runningHandles_);
    LOG_TRACE << rc << " " << runningHandles_;
  } while (rc == CURLM_CALL_MULTI_PERFORM);
  checkFinish();
}

void Curl::onRead(int fd)
{
  CURLMcode rc = CURLM_OK;
  do {
    LOG_TRACE << fd;
    // The curl_multi_socket_action functions inform the application about updates in the socket (file descriptor) status
    // by doing none, one, or multiple calls to the socket callback function set with the CURLMOPT_SOCKETFUNCTION option to curl_multi_setopt.
    rc = curl_multi_socket_action(curlm_, fd, CURL_POLL_IN, &runningHandles_);
    LOG_TRACE << fd << " " << rc << " " << runningHandles_;
  } while (rc == CURLM_CALL_MULTI_PERFORM);
  checkFinish();
}

void Curl::onWrite(int fd)
{
  CURLMcode rc = CURLM_OK;
  do {
    LOG_TRACE << fd;
    rc = curl_multi_socket_action(curlm_, fd, CURL_POLL_OUT, &runningHandles_);
    LOG_TRACE << fd << " " << rc << " " << runningHandles_;
  } while (rc == CURLM_CALL_MULTI_PERFORM);
  checkFinish();
}

void Curl::checkFinish()
{
  if (prevRunningHandles_ > runningHandles_ || runningHandles_ == 0)
  {
    CURLMsg* msg = NULL;
    int left = 0;
    while ( (msg = curl_multi_info_read(curlm_, &left)) != NULL)
    {
      if (msg->msg == CURLMSG_DONE)
      {
        CURL* c = msg->easy_handle;
        CURLcode res = msg->data.result;
        Request* req = NULL;
        curl_easy_getinfo(c, CURLINFO_PRIVATE, &req);
        assert(req->getCurl() == c);
        LOG_TRACE << req << " done";
        req->done(res);
      }
    }
  }
  prevRunningHandles_ = runningHandles_;
}
