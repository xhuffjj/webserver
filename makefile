all:s
s:main.o http_conn.o heap_timer.o
	g++ -o s main.o http_conn.o heap_timer.o
main.o:main.cpp locker.h threadpool.h http_conn.h heap_timer.h
	g++ -c main.cpp
http_conn.o:http_conn.cpp http_conn.h heap_timer.h
	g++ -c http_conn.cpp
heap_timer.o:heap_timer.cpp heap_timer.h
	g++ -c heap_timer.cpp

