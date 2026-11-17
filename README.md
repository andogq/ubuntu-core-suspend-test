> [!warning]
> Made with an agent as a PoC, do not trust it.

# Ubuntu Core suspend test

Proof-of-concept strict snap for exercising
`org.freedesktop.login1.Manager.Suspend(false)` on Ubuntu Core 26. The program
uses the host system D-Bus to:

1. call `ListInhibitors()` and report every active inhibitor;
2. identify blocking and delaying inhibitors whose `what` field contains
   `sleep`;
3. call `CanSuspend()` and require the result to be `yes`;
4. refuse the suspend if a blocking sleep inhibitor exists;
5. subscribe to the login1 `PrepareForSleep` signal before requesting sleep;
6. call `Suspend(false)` asynchronously and wait for both
   `PrepareForSleep(true)` and `PrepareForSleep(false)`; and
7. compare `CLOCK_BOOTTIME` with `CLOCK_MONOTONIC` after the resume signal,
   proving that the same process continued across a real suspend interval.

The super-privileged `shutdown` interface permits the suspend and inhibitor
method calls. `login-session-observe` permits receiving `PrepareForSleep`.
Neither interface auto-connects. The snap must run as root so logind's
non-interactive authorization returns `CanSuspend=yes`.

## Build

```shell
snapcraft
```

## Deploy to the Multipass Ubuntu Core VM

```shell
multipass transfer ubuntu-core-suspend-test_0.1_amd64.snap suspend-test:
multipass exec suspend-test -- \
  sudo snap install --dangerous ubuntu-core-suspend-test_0.1_amd64.snap
multipass exec suspend-test -- \
  sudo snap connect ubuntu-core-suspend-test:shutdown
multipass exec suspend-test -- \
  sudo snap connect ubuntu-core-suspend-test:login-session-observe
```

Run the non-destructive D-Bus and inhibitor check first:

```shell
multipass exec suspend-test -- \
  sudo ubuntu-core-suspend-test.suspend-test check
```

Multipass continues to report a guest-triggered S3 state as `Running`, so
`multipass start` does not wake this VM. Arm its virtual RTC before invoking
the snap; the alarm supplies the external wake event after eight seconds:

```shell
multipass exec suspend-test -- sh -c \
  'sudo rtcwake -m no -s 8 && \
   sudo ubuntu-core-suspend-test.suspend-test suspend'
```

On physical hardware, invoke the `suspend` command without `rtcwake` and resume
the machine with its normal wake mechanism.

The original command succeeds only after receiving the complete
`PrepareForSleep(true)` → `PrepareForSleep(false)` cycle and observing at least
one second during which `CLOCK_BOOTTIME` advanced but `CLOCK_MONOTONIC` did not.
Its final output must contain the original PID and:

```text
PASS: observed the complete PrepareForSleep cycle and the same process continued after suspend
```

