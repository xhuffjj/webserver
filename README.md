# webserver
一个基于 Reactor 模式的轻量级高并发 Web 服务器。采用 Epoll + 线程池作为核心并发模型，通过有限状态机解析 HTTP/1.1 协议，实现了对静态资源（GET）和动态 CGI 脚本（POST）的并发处理。

技术栈： C++, Linux, Epoll, Reactor 模式, 线程池, HTTP, Socket, mmap, CGI

运行方法：
1.在val/www/html下运行
chmod +x post.cgi

chmod +666 post.html

需要注意的是post.cgi是Python3语法，/usr/bin/python3目录下查看Python版本

http_conn.cpp中的 doc_root = "/home/a/webserver最新版/val/www/html";改成   "你的项目目录/val/www/html"

2.make

./s 127.0.0.1 8888
(./picture/运行.png)



