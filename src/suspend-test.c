#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define LOGIN_DEST "org.freedesktop.login1"
#define LOGIN_PATH "/org/freedesktop/login1"
#define LOGIN_IFACE "org.freedesktop.login1.Manager"
#define MIN_SUSPEND_SECONDS 1.0

struct inhibitor_summary {
    unsigned int total;
    unsigned int sleep_delays;
    unsigned int sleep_blockers;
};

struct suspend_state {
    bool method_replied;
    bool prepare_seen;
    bool resume_seen;
    int error;
};

static void print_bus_error(const char *operation, int result,
                            const sd_bus_error *error)
{
    const char *detail = error != NULL && error->message != NULL
                             ? error->message
                             : strerror(-result);

    fprintf(stderr, "%s failed: %s\n", operation, detail);
}

static bool inhibitor_covers_sleep(const char *what)
{
    const char *token = what;

    while (*token != '\0') {
        const char *end = strchr(token, ':');
        size_t length = end == NULL ? strlen(token) : (size_t)(end - token);

        if (length == strlen("sleep") &&
            strncmp(token, "sleep", length) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        token = end + 1;
    }

    return false;
}

static bool inhibitor_is_delay(const char *mode)
{
    return strncmp(mode, "delay", strlen("delay")) == 0;
}

static int list_inhibitors(sd_bus *bus, struct inhibitor_summary *summary)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int result = sd_bus_call_method(bus, LOGIN_DEST, LOGIN_PATH, LOGIN_IFACE,
                                    "ListInhibitors", &error, &reply, NULL);
    if (result < 0) {
        print_bus_error("ListInhibitors", result, &error);
        sd_bus_error_free(&error);
        return result;
    }

    result = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY,
                                             "(ssssuu)");
    if (result < 0) {
        print_bus_error("decoding ListInhibitors", result, NULL);
        goto out;
    }

    puts("Inhibitors:");
    while ((result = sd_bus_message_enter_container(
                reply, SD_BUS_TYPE_STRUCT, "ssssuu")) > 0) {
        const char *what;
        const char *who;
        const char *why;
        const char *mode;
        uint32_t uid;
        uint32_t pid;

        result = sd_bus_message_read(reply, "ssssuu", &what, &who, &why,
                                     &mode, &uid, &pid);
        if (result < 0) {
            print_bus_error("decoding an inhibitor", result, NULL);
            goto out;
        }

        ++summary->total;
        printf("  what=%s mode=%s who=%s why=%s uid=%u pid=%u\n", what,
               mode, who, why, uid, pid);

        if (inhibitor_covers_sleep(what)) {
            if (inhibitor_is_delay(mode)) {
                ++summary->sleep_delays;
            } else {
                ++summary->sleep_blockers;
            }
        }

        result = sd_bus_message_exit_container(reply);
        if (result < 0) {
            print_bus_error("decoding an inhibitor", result, NULL);
            goto out;
        }
    }
    if (result < 0) {
        print_bus_error("decoding ListInhibitors", result, NULL);
        goto out;
    }

    result = sd_bus_message_exit_container(reply);
    if (result < 0) {
        print_bus_error("decoding ListInhibitors", result, NULL);
        goto out;
    }

    if (summary->total == 0) {
        puts("  none");
    }
    printf("Sleep inhibitors: %u blocking, %u delaying\n",
           summary->sleep_blockers, summary->sleep_delays);
    result = 0;

out:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int get_suspend_capability(sd_bus *bus, const char **capability,
                                  sd_bus_message **reply_out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int result = sd_bus_call_method(bus, LOGIN_DEST, LOGIN_PATH, LOGIN_IFACE,
                                    "CanSuspend", &error, &reply, NULL);
    if (result < 0) {
        print_bus_error("CanSuspend", result, &error);
        sd_bus_error_free(&error);
        return result;
    }

    result = sd_bus_message_read(reply, "s", capability);
    if (result < 0) {
        print_bus_error("decoding CanSuspend", result, NULL);
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return result;
    }

    *reply_out = reply;
    sd_bus_error_free(&error);
    return 0;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static int handle_prepare_for_sleep(sd_bus_message *message, void *userdata,
                                    sd_bus_error *ret_error)
{
    struct suspend_state *state = userdata;
    int preparing;
    int result;

    (void)ret_error;
    result = sd_bus_message_read(message, "b", &preparing);
    if (result < 0) {
        print_bus_error("decoding PrepareForSleep", result, NULL);
        state->error = result;
        return 0;
    }

    if (preparing) {
        if (!state->prepare_seen) {
            puts("PrepareForSleep(true): suspend is starting");
            state->prepare_seen = true;
        }
    } else if (!state->prepare_seen) {
        fputs("PrepareForSleep(false) arrived before PrepareForSleep(true)\n",
              stderr);
        state->error = -EPROTO;
    } else if (!state->resume_seen) {
        puts("PrepareForSleep(false): resume is complete");
        state->resume_seen = true;
    }

    return 0;
}

static int handle_suspend_reply(sd_bus_message *message, void *userdata,
                                sd_bus_error *ret_error)
{
    struct suspend_state *state = userdata;

    (void)ret_error;
    state->method_replied = true;
    if (sd_bus_message_is_method_error(message, NULL) > 0) {
        const sd_bus_error *error = sd_bus_message_get_error(message);
        int error_number = sd_bus_message_get_errno(message);

        if (error_number <= 0) {
            error_number = EIO;
        }
        print_bus_error("Suspend", -error_number, error);
        state->error = -error_number;
    } else {
        puts("Suspend request accepted; waiting for PrepareForSleep(false)");
    }

    return 0;
}

static int request_suspend(sd_bus *bus)
{
    sd_bus_message *request = NULL;
    sd_bus_slot *match_slot = NULL;
    sd_bus_slot *call_slot = NULL;
    struct suspend_state state = {0};
    struct timespec boot_before;
    struct timespec mono_before;
    struct timespec boot_after;
    struct timespec mono_after;
    pid_t pid = getpid();
    int result;

    result = sd_bus_add_match(
        bus, &match_slot,
        "type='signal',sender='org.freedesktop.login1',"
        "path='/org/freedesktop/login1',"
        "interface='org.freedesktop.login1.Manager',"
        "member='PrepareForSleep'",
        handle_prepare_for_sleep, &state);
    if (result < 0) {
        print_bus_error("subscribing to PrepareForSleep", result, NULL);
        goto out;
    }

    if (clock_gettime(CLOCK_BOOTTIME, &boot_before) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &mono_before) < 0) {
        perror("clock_gettime");
        result = -errno;
        goto out;
    }

    printf("Requesting Suspend(false): pid=%ld\n", (long)pid);

    result = sd_bus_message_new_method_call(bus, &request, LOGIN_DEST,
                                            LOGIN_PATH, LOGIN_IFACE, "Suspend");
    if (result < 0) {
        print_bus_error("creating Suspend request", result, NULL);
        goto out;
    }
    result = sd_bus_message_append(request, "b", 0);
    if (result < 0) {
        print_bus_error("creating Suspend request", result, NULL);
        goto out;
    }

    result = sd_bus_call_async(bus, &call_slot, request, handle_suspend_reply,
                               &state, UINT64_MAX);
    if (result < 0) {
        print_bus_error("sending Suspend request", result, NULL);
        goto out;
    }

    while (state.error >= 0 &&
           (!state.method_replied || !state.resume_seen)) {
        result = sd_bus_process(bus, NULL);
        if (result < 0) {
            print_bus_error("processing system bus messages", result, NULL);
            goto out;
        }
        if (result > 0) {
            continue;
        }

        result = sd_bus_wait(bus, UINT64_MAX);
        if (result < 0) {
            print_bus_error("waiting for PrepareForSleep", result, NULL);
            goto out;
        }
    }
    if (state.error < 0) {
        result = state.error;
        goto out;
    }

    if (clock_gettime(CLOCK_BOOTTIME, &boot_after) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &mono_after) < 0) {
        perror("clock_gettime");
        result = -errno;
        goto out;
    }

    double boot_elapsed = elapsed_seconds(&boot_before, &boot_after);
    double active_elapsed = elapsed_seconds(&mono_before, &mono_after);
    double suspended_elapsed = boot_elapsed - active_elapsed;

    printf("Continued after Suspend: pid=%ld boot-time=%.3fs active-time=%.3fs "
           "suspended-time=%.3fs\n",
           (long)getpid(), boot_elapsed, active_elapsed, suspended_elapsed);

    if (getpid() != pid) {
        fputs("Process identity changed across suspend\n", stderr);
        result = -ECHILD;
    } else if (suspended_elapsed < MIN_SUSPEND_SECONDS) {
        fprintf(stderr,
                "Suspend was not verified: observed only %.3fs with CPUs "
                "suspended (minimum %.1fs)\n",
                suspended_elapsed, MIN_SUSPEND_SECONDS);
        result = -ETIME;
    } else {
        puts("PASS: observed the complete PrepareForSleep cycle and the same "
             "process continued after suspend");
        result = 0;
    }

out:
    sd_bus_slot_unref(call_slot);
    sd_bus_slot_unref(match_slot);
    sd_bus_message_unref(request);
    return result;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s check|suspend\n", program);
}

int main(int argc, char **argv)
{
    sd_bus *bus = NULL;
    sd_bus_message *capability_reply = NULL;
    struct inhibitor_summary inhibitors = {0};
    const char *capability = NULL;
    bool should_suspend;
    int result;
    (void)setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc != 2 ||
        (strcmp(argv[1], "check") != 0 && strcmp(argv[1], "suspend") != 0)) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    should_suspend = strcmp(argv[1], "suspend") == 0;

    result = sd_bus_default_system(&bus);
    if (result < 0) {
        print_bus_error("connecting to the host system bus", result, NULL);
        goto out;
    }

    result = list_inhibitors(bus, &inhibitors);
    if (result < 0) {
        goto out;
    }

    result = get_suspend_capability(bus, &capability, &capability_reply);
    if (result < 0) {
        goto out;
    }
    printf("CanSuspend: %s\n", capability);

    if (inhibitors.sleep_blockers != 0) {
        fprintf(stderr, "Refusing to suspend: %u blocking sleep inhibitor(s)\n",
                inhibitors.sleep_blockers);
        result = -EBUSY;
        goto out;
    }
    if (strcmp(capability, "yes") != 0) {
        fprintf(stderr,
                "Refusing to suspend: CanSuspend must be yes (got %s)\n",
                capability);
        result = -EACCES;
        goto out;
    }

    if (!should_suspend) {
        puts("PASS: suspend is available and has no blocking inhibitor");
        result = 0;
        goto out;
    }

    result = request_suspend(bus);

out:
    sd_bus_message_unref(capability_reply);
    sd_bus_unref(bus);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
