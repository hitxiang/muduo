#include <muduo/net/inspect/Inspector.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThread.h>

using namespace muduo;
using namespace muduo::net;

int main()
{
  EventLoop loop;  // service loop
  EventLoopThread t; // monitor loop thread
  Inspector ins(t.startLoop(), InetAddress(12345), "test");
  loop.loop();
}

