#include <muduo/net/Channel.h>
#include <muduo/net/TcpClient.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>

#include <boost/bind.hpp>

#include <stdio.h>

using namespace muduo;
using namespace muduo::net;

class TcpTestClient
{
public:
    TcpTestClient(EventLoop *loop, const InetAddress &listenAddr)
        : loop_(loop),
          client_(loop, listenAddr, "TcpTestClient"),
          stdinChannel_(loop, 0)
    {
        client_.setConnectionCallback(
            boost::bind(&TcpTestClient::onConnection, this, _1));
        client_.setMessageCallback(
            boost::bind(&TcpTestClient::onMessage, this, _1, _2, _3));
        //client_.enableRetry();
        // 标准输入缓冲区中有数据的时候，回调TcpTestClient::handleRead
        stdinChannel_.setReadCallback(boost::bind(&TcpTestClient::handleRead, this));
        stdinChannel_.enableReading();      // 关注可读事件
    }

    void connect()
    {
        client_.connect();
    }

private:
    void onConnection(const TcpConnectionPtr &conn)
    {
        if (conn->connected())
        {
            printf("onConnection(): new connection [%s] from %s\n",
                   conn->name().c_str(),
                   conn->peerAddress().toIpPort().c_str());
        }
        else
        {
            printf("onConnection(): connection [%s] is down\n",
                   conn->name().c_str());
        }
    }

    void onMessage(const TcpConnectionPtr &conn, Buffer *buf, Timestamp time)
    {
        string msg(buf->retrieveAllAsString());
        printf("onMessage(): recv a message [%s]\n", msg.c_str());
        LOG_TRACE << conn->name() << " recv " << msg.size() << " bytes at " << time.toFormattedString();
    }

    // 标准输入缓冲区中有数据的时候，回调该函数
    void handleRead()
    {
        char buf[1024] = {0};
        fgets(buf, 1024, stdin);
        buf[strlen(buf) - 1] = '\0';        // 去除\n
        client_.connection()->send(buf);
    }

    EventLoop *loop_;
    TcpClient client_;
    Channel stdinChannel_;      // 标准输入Channel
};

int main(int argc, char *argv[])
{
    LOG_INFO << "pid = " << getpid() << ", tid = " << CurrentThread::tid();
    EventLoop loop;
    InetAddress serverAddr("127.0.0.1", 8888);
    TcpTestClient client(&loop, serverAddr);
    client.connect();
    loop.loop();
}

 	
//int main(int argc, char* argv[])
//{
//  LOG_INFO << "pid = " << getpid();
//  EventLoop loop;
//  g_loop = &loop;
//  // 用两个IO线程来发起多个连接
//  EventLoopThreadPool loopPool(&loop);
//  loopPool.setThreadNum(2);
//  loopPool.start();
//
//  boost::ptr_vector<RecvFileClient> clients(8);
//
//  InetAddress serverAddr("127.0.0.1", 2021);
//
//  for (int i = 0; i < 8; ++i)
//  {
//    char buf[32];
//    snprintf(buf, sizeof buf, "%d", i+1);
//    clients.push_back(new RecvFileClient(loopPool.getNextLoop(), serverAddr, buf));
//    clients[i].connect();
//    usleep(200);
//  }
//
//  loop.loop();
//  usleep(20000);
//}