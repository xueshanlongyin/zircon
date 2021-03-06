# zx_thread_write_state

## NAME

thread_write_state - Write one aspect of thread state.

## SYNOPSIS

```
#include <zircon/syscalls.h>
#include <zircon/syscalls/debug.h>

zx_status_t zx_thread_write_state(
    zx_handle_t handle,
    uint32_t kind,
    const void* buffer,
    size_t len);
```

## DESCRIPTION

**thread_write_state**() writes one aspect of state of the thread. The thread
state may only be written when the thread is halted for an exception or the
thread is suspended.

The thread state is highly processor specific. See the structures in
zircon/syscalls/debug.h for the contents of the structures on each platform.

## STATES

See [thread_read_state](thread_read_state.md) for the list of available states
and their corresponding values.

### ZX_THREAD_STATE_DEBUG_REGS

#### ARM

ARM has a variable amount of debug breakpoints and watchpoints. For this
architecture, **zx_thread_state_debug_regs_t** is big enough to hold the maximum
amount of breakpoints possible. But in most cases a given CPU implementation
holds a lesser amount, meaning that the upper values beyond the limit are not
used.

The kernel will write all the available registers in the hardware independent of
the given breakpoint/watchpoint count value. This means that all the correct
state must be set for the call.

You can get the current state of the registers by calling
[thread_read_state](thread_read_state.md#zx_thread_state_debug_regs).

## RIGHTS

TODO(ZX-2399)

## RETURN VALUE

**thread_write_state**() returns **ZX_OK** on success.
In the event of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle.

**ZX_ERR_WRONG_TYPE**  *handle* is not that of a thread.

**ZX_ERR_ACCESS_DENIED**  *handle* lacks *ZX_RIGHT_WRITE*.

**ZX_ERR_INVALID_ARGS**  *kind* is not valid, *buffer* is an invalid pointer,
*len* doesn't match the size of the structure expected for *kind* or the given
values to set are not valid.

**ZX_ERR_NO_MEMORY**  Failure due to lack of memory.
There is no good way for userspace to handle this (unlikely) error.
In a future build this error will no longer occur.

**ZX_ERR_BAD_STATE**  The thread is not stopped at a point where state
is available. The thread state may only be read when the thread is stopped due
to an exception.

**ZX_ERR_NOT_SUPPORTED**  *kind* is not supported.
This can happen, for example, when trying to read a register set that
is not supported by the hardware the program is currently running on.

## SEE ALSO

[thread_read_state](thread_read_state.md).
