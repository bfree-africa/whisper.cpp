// Implementation of required uSockets symbols
#include <cstddef>
#include <cstdlib>

extern "C" {
    // Socket context functions
    void *us_create_socket_context(int ssl, void *loop, int ext_size, void *options) {
        return malloc(ext_size);
    }
    
    void us_socket_context_free(int ssl, void *context) {
        free(context);
    }
    
    void *us_socket_context_ext(int ssl, void *context) {
        return context;
    }
    
    // Socket functions
    int us_socket_is_closed(int ssl, void *s) { return 0; }
    int us_socket_is_shut_down(int ssl, void *s) { return 0; }
    void us_socket_timeout(int ssl, void *s, unsigned int seconds) {}
    void us_socket_close(int ssl, void *s, int code, void *reason) {}
    
    // Loop functions
    void *us_create_loop(void *hint, void (*wakeup_cb)(void *loop), void (*pre_cb)(void *loop), 
                         void (*post_cb)(void *loop), unsigned int ext_size) {
        return malloc(ext_size);
    }
    
    void us_loop_free(void *loop) {
        free(loop);
    }
    
    void *us_loop_ext(void *loop) {
        return loop;
    }
    
    void us_loop_run(void *loop) {}
    void us_loop_integrate(void *loop) {}
    
    // Listen socket functions
    void *us_socket_context_listen(int ssl, void *context, const char *host, int port, int options, int ext_size) {
        return NULL;
    }
}
