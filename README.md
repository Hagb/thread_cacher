# Thread reuse via `LD_PRELOAD` hook

Keep the "exited" thread in "thread pool" and reuse it when new thread is needed, instead of creating and destory threads again and again. 

At the beginning, it was wriiten as a workaround for [the memory leak of qemu-user when threads are created and destoryed](https://gitlab.com/qemu-project/qemu/-/issues/866).

**DO NOT USE IN PRODUCTION ENVIRNOMENT**

## Build

```bash
git submodule init
git submodule update
mkdir build
cd build
cmake ..
make
```

And `libthread_reuse.so` should be there

Tips: To use in qemu-user, compile for the target architecture is needed.

## Use

```bash
LD_PRELOAD=libthread_reuse.so command
```

or if in qemu-user
``` bash
qemu-ARCH -E LD_PRELOAD=libthread_reuse.so an-executable-of-arch
```

## How can it implemented

It hooks some pthread functions:

- `pthread_create`: (origin) create a new thread; (hooked) if there isn't enough thread in thread pool, then create a new one, else get a thread from pool and submit the task
- `pthread_join`: (origin) join the thread, after that the thread will be destory; (hooked) wait until the thread finish it's task, get the return value of task function, and tell the thread that it can come back to the pool
- `pthread_detach`: (origin) ...; (hooked) tell the thread that it can come back to the pool
- `pthread_exit`: (origin) exit the thread calling this function; (hooked) jump out of the task function via long jump (exactly `siglongjmp`)
- `pthread_kill`: (origin) send a signal to a thread (including kill the signal by `SIGKILL`) or determine whether the thread alive by sig `0`; (hooked) if `SIGKILL` is passed, send the thread a signal whose handler jumps out of the task function, or if sig `0` is passed, return the status of task function

## TODO

- [ ]: handle more signals
- [ ]: more comment and document
- [ ]: code clean-up and better style
- [ ]: more configurable
- [ ]: more tests
- [ ]: test suite
- [ ]: better POSIX compatibility
