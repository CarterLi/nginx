# nginx-io_uring

This repo tries to improve preformance of Nginx using io_uring event model.

## Kernel requirement

Linux 5.13.0+ and liburing HEAD ( for IORING_POLL_ADD_MULTI support ).

## Contribute

This project is highly experimental and unstable currently, any contributions are welcome. The modification is basically 1 to 1 changes, mainly comes from `src/event/modules/ngx_epoll_module.c`

To debug it, compile nginx with `--with-file-aio --with-debug`:

```
# nginx.conf
master_process off;
daemon off;
error_log  stderr debug;

http {
  aio on;
  sendfile off;
}
```
