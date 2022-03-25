# Threads reuse via `LD_PRELOAD` hook

Save the "exited" threads in a "thread pool" and reuse them when new threads is needed, instead of creating and destorying threads again and again.

At the beginning, it was wriiten as a workaround for [the memory leak of qemu-user when threads are created and destoryed](https://gitlab.com/qemu-project/qemu/-/issues/866).

**DO NOT USE IN PRODUCTION ENVIRNOMENT**

## Build

```bash
mkdir build
cd build
cmake ..
make
```

And then there should be a `./libthread_reuse.so`.

## Use

```bash
LD_PRELOAD=libthread_reuse.so command
```

## How can it implemented

It hooks some functions of pthread:

- `pthread_create`: (origin) create a new thread; (hooked) if there isn't enough threads in the thread pool, then create a new one, else get a thread from pool and submit the task
- `pthread_join`: (origin) join the thread, after that the thread will be destoryed; (hooked) wait until the thread finish its task, and get the return value of the task function, and tell the real thread that it can come back to the pool
- `pthread_detach`: (origin) ...; (hooked) tell the thread that it can come back to the pool
- `pthread_exit`: (origin) exit the thread calling this function; (hooked) jump out of the task function via a long jump (exactly `siglongjmp`)
- `pthread_kill`: (origin) send a signal to a thread (including kill the signal by `SIGKILL`) or determine whether the thread alive by sig `0`; (hooked) if `SIGKILL` is passed, send the thread a specific signal whose handler will jump out of the task function, or if sig `0` is passed, return whether the task has been terminated.

## TODO

- [ ] handle more signals
- [ ] more comment and document
- [ ] code clean-up and better style
- [ ] more configurable
- [ ] more tests
- [ ] test suite
- [ ] better POSIX compatibility

