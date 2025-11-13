#include "heap_timer.h"
#include "http_conn.h"
heap_timer::heap_timer(int delay) {
        expire = time(NULL) + delay;
}
