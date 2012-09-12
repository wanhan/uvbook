#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

uv_loop_t *loop;

struct child_worker {
    uv_process_t req;
    uv_process_options_t options;
    uv_pipe_t pipe;
} *workers;

char worker_path[500];
uv_buf_t dummy_buf;
int round_robin_counter;
int child_worker_count;
int total_count;

void on_exit(uv_process_t *req, int exit_status, int term_signal) {
    fprintf(stderr, "Process exited with status %d, signal %d\n", exit_status, term_signal);
    uv_close((uv_handle_t*) req, NULL);
}

uv_buf_t alloc_buffer(uv_handle_t *handle, size_t suggested_size) {
    return uv_buf_init((char*) calloc(suggested_size, 1), suggested_size);
}

void on_new_connection(uv_stream_t *server, int status) {
    if (status == -1) {
        // error!
        return;
    }

    uv_pipe_t *client = (uv_pipe_t*) malloc(sizeof(uv_pipe_t));
    uv_pipe_init(loop, client, 0);
    if (uv_accept(server, (uv_stream_t*) client) == 0) {
        total_count++;
        uv_write_t *write_req = (uv_write_t*) malloc(sizeof(uv_write_t));
        dummy_buf = uv_buf_init(".", 1);
        fprintf(stderr, "Connection number %d\n", total_count);
        struct child_worker *worker = &workers[round_robin_counter];
        uv_write2(write_req, (uv_stream_t*) &worker->pipe, &dummy_buf, 1, (uv_stream_t*) client, NULL);
        round_robin_counter = (round_robin_counter + 1) % child_worker_count;
    }
    else {
        uv_close((uv_handle_t*) client, NULL);
    }
}

void setup_workers() {
    size_t path_size = 500;
    uv_exepath(worker_path, &path_size);
    strcpy(worker_path + (strlen(worker_path) - strlen("multi-echo-server")), "worker");
    fprintf(stderr, "Worker path: %s\n", worker_path);

    char* args[2];
    args[0] = worker_path;
    args[1] = NULL;

    // launch same number of workers as number of CPUs
    uv_cpu_info_t *info;
    int cpu_count;
    uv_cpu_info(&info, &cpu_count);
    uv_free_cpu_info(info, cpu_count);

    fprintf(stderr, "No. of CPUs: %d\n", cpu_count);
    round_robin_counter = 0;
    total_count = 0;
    child_worker_count = cpu_count;

    workers = calloc(sizeof(struct child_worker), cpu_count);
    while (cpu_count--) {
        struct child_worker *worker = &workers[cpu_count];
        uv_pipe_init(loop, &worker->pipe, 1);

        uv_stdio_container_t child_stdio[3];
        child_stdio[0].flags = UV_CREATE_PIPE | UV_READABLE_PIPE;
        child_stdio[0].data.stream = (uv_stream_t*) &worker->pipe;
        child_stdio[1].flags = UV_IGNORE;
        child_stdio[2].flags = UV_INHERIT_FD;
        child_stdio[2].data.fd = 2;

        worker->options.stdio = child_stdio;
        worker->options.stdio_count = 3;

        worker->options.exit_cb = on_exit;
        worker->options.file = args[0];
        worker->options.args = args;

        uv_spawn(loop, &worker->req, worker->options); 
        fprintf(stderr, "Started worker %d\n", worker->req.pid);
    }
}

int main() {
    loop = uv_default_loop();

    setup_workers();

    uv_tcp_t server;
    uv_tcp_init(loop, &server);

    struct sockaddr_in bind_addr = uv_ip4_addr("0.0.0.0", 7000);
    uv_tcp_bind(&server, bind_addr);
    if (uv_listen((uv_stream_t*) &server, 128, on_new_connection)) {
        fprintf(stderr, "Listen error %s\n", uv_err_name(uv_last_error(loop)));
        return 2;
    }
    return uv_run(loop);
}
