/*
 * PS4 Fan Control PayLoad
 * Tested: PS4 / Firmware 12.52
 * SDK:    ps4-payload-dev/sdk
 *
 * Flow:
 *   1. Read the current /dev/icc_fan 10-byte PS4 threshold request.
 *   2. Log the raw bytes so the target console can be inspected.
 *   3. Load the threshold from /data/fan_control/fan_control.ini.
 *   4. Copy the original 10 bytes and change ONLY byte 5.
 *   5. Write the complete 10-byte request back.
 *   6. Read it again, log the raw bytes, and verify byte 5.
 *   7. Append a result to the log and show a notification.
 *   8. Exit.
 *
 * Notes:
 *   - The payload changes the ICC fan temperature threshold; it is not a
 *     direct RPM/PWM controller.
 *   - The ICC ioctl interface is low-level and should be tested on the
 *     target firmware/console before unattended deployment.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

#define PAYLOAD_VERSION        "1.10"

#define CONFIG_DIR             "/data/fan_control"
#define CONFIG_FILE            "/data/fan_control/fan_control.ini"

#define LOG_DIR                "/data/fan_control"
#define LOG_FILE               "/data/fan_control/fan_control.log"

#define DEFAULT_THRESHOLD_C    65
#define MIN_THRESHOLD_C        60
#define MAX_THRESHOLD_C        80

/* -------------------------------------------------------------------------- */
/* ICC fan interface                                                          */
/* -------------------------------------------------------------------------- */

#define ICC_FAN_DEVICE         "/dev/icc_fan"

/* PS4 /dev/icc_fan threshold ioctls. */
#define ICC_IOCTL_SET_THRESHOLD 0xC01C8F07UL
#define ICC_IOCTL_GET_THRESHOLD 0xC01C8F08UL

/* The established PS4 request is 10 bytes; byte 5 is temperature in C. */
#define ICC_PROFILE_SIZE        10
#define ICC_THRESHOLD_OFFSET    5

/* -------------------------------------------------------------------------- */
/* Notification                                                               */
/* -------------------------------------------------------------------------- */

/* SceNotificationRequest layout; total size = 0xC30. */
typedef struct {
    int32_t  type;                 /* 0x00 */
    uint32_t req_id;               /* 0x04 */
    uint32_t priority;             /* 0x08 */
    uint32_t msg_id;               /* 0x0C */
    int32_t  target_id;            /* 0x10 */
    uint32_t user_id;              /* 0x14 */
    uint32_t device_id;            /* 0x18 */
    uint32_t addressing_user_id;   /* 0x1C */
    uint32_t app_id;               /* 0x20 */
    uint32_t error_number;         /* 0x24 */
    uint32_t attribute;            /* 0x28 */
    uint8_t  use_icon_uri;         /* 0x2C */
    char     message[0x400];       /* 0x2D */
    char     icon_uri[0x800];      /* 0x42D */
    uint8_t  reserved[3];          /* 0xC2D */
} SceNotificationRequest;

_Static_assert(sizeof(SceNotificationRequest) == 0xC30,
               "SceNotificationRequest must be 0xC30 bytes");

extern int sceKernelSendNotificationRequest(int api,
                                             SceNotificationRequest *request,
                                             size_t size,
                                             int blocking);
extern int sceKernelUsleep(unsigned int microseconds);

/* -------------------------------------------------------------------------- */
/* Small Helpers                                                              */
/* -------------------------------------------------------------------------- */

static void format_hex(const uint8_t *data, size_t length,
                       char *out, size_t out_size)
{
    size_t used = 0;

    if (out_size == 0)
        return;

    out[0] = '\0';

    for (size_t i = 0; i < length; i++) {
        int written;

        if (used >= out_size)
            break;

        written = snprintf(out + used, out_size - used,
                           "%s%02X", (i == 0) ? "" : " ", data[i]);
        if (written < 0)
            break;

        used += (size_t)written;
    }
}

static int write_log(const char *status, const char *detail)
{
    FILE *file;
    time_t now;
    struct tm *utc;
    int saved_errno;

    /* Create the project log directory if needed. */
    if (mkdir(LOG_DIR, 0777) < 0 && errno != EEXIST) {
        saved_errno = errno;
        fprintf(stderr,
                "[FanCtrl] mkdir(%s) failed: errno=%d\n",
                LOG_DIR, saved_errno);
    }

    /* Keep the log in the fan_control folder only. */
    file = fopen(LOG_FILE, "a");
    if (!file) {
        saved_errno = errno;
        fprintf(stderr,
                "[FanCtrl] fopen(%s) failed: errno=%d\n",
                LOG_FILE, saved_errno);
        return -saved_errno;
    }

    now = time(NULL);
    utc = gmtime(&now);

    if (utc) {
        fprintf(file,
                "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
                utc->tm_year + 1900,
                utc->tm_mon + 1,
                utc->tm_mday,
                utc->tm_hour,
                utc->tm_min,
                utc->tm_sec,
                status,
                detail);
    } else {
        fprintf(file, "[time unavailable] [%s] %s\n", status, detail);
    }

    fflush(file);
    fclose(file);
    return 0;
}

static void notify_user(const char *text)
{
    SceNotificationRequest request;

    /* Give ShellUI/notification services a chance to finish startup. */
    (void)sceKernelUsleep(2000000);

    memset(&request, 0, sizeof(request));
    request.type = 0;
    request.target_id = -1;
    request.use_icon_uri = 1;

    strncpy(request.message, text, sizeof(request.message) - 1);
    strncpy(request.icon_uri,
            "cxml://psnotification/tex_icon_system",
            sizeof(request.icon_uri) - 1);

    (void)sceKernelSendNotificationRequest(0,
                                           &request,
                                           sizeof(request),
                                           0);
}

/* -------------------------------------------------------------------------- */
/* Config                                                                    */
/* -------------------------------------------------------------------------- */

typedef enum {
    CONFIG_FROM_INI,
    CONFIG_CREATED,
    CONFIG_MISSING_KEY,
    CONFIG_CLAMPED,
    CONFIG_INVALID,
    CONFIG_FILE_ERROR
} ConfigSource;

static int create_default_config(void)
{
    FILE *file;

    if (mkdir(CONFIG_DIR, 0777) < 0 && errno != EEXIST)
        return -errno;

    file = fopen(CONFIG_FILE, "w");
    if (!file)
        return -errno;

    fprintf(file,
            "# PS4 Fan Control PayLoad\n"
            "# Temperature threshold in degrees Celsius.\n"
            "# Safe configuration range used by this payload: %d-%d C.\n"
            "# Default: %d C.\n"
            "threshold=%d\n",
            MIN_THRESHOLD_C,
            MAX_THRESHOLD_C,
            DEFAULT_THRESHOLD_C,
            DEFAULT_THRESHOLD_C);

    if (fclose(file) != 0)
        return -errno;

    return 0;
}

static int read_threshold(ConfigSource *source, int *raw_value)
{
    FILE *file;
    char line[128];
    int value = -1;
    int found = 0;

    *source = CONFIG_CREATED;
    *raw_value = -1;

    file = fopen(CONFIG_FILE, "r");
    if (!file) {
        int create_rc = create_default_config();
        if (create_rc < 0) {
            *source = CONFIG_FILE_ERROR;
            return -1;
        }
        return DEFAULT_THRESHOLD_C;
    }

    while (fgets(line, sizeof(line), file)) {
        char *cursor = line;

        while (*cursor == ' ' || *cursor == '\t')
            cursor++;

        if (*cursor == '\0' || *cursor == '\r' || *cursor == '\n')
            continue;
        if (*cursor == '#' || *cursor == ';')
            continue;

        if (strncmp(cursor, "threshold=", 10) == 0) {
            char *end;
            long parsed;

            errno = 0;
            parsed = strtol(cursor + 10, &end, 10);

            if (end == cursor + 10 || errno == ERANGE) {
                value = -1;
                found = 1;
            } else {
                while (*end == ' ' || *end == '\t' ||
                       *end == '\r' || *end == '\n')
                    end++;

                if (*end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
                    value = -1;
                    found = 1;
                } else {
                    value = (int)parsed;
                    found = 1;
                }
            }
            break;
        }
    }

    if (ferror(file)) {
        int saved_errno = errno ? errno : EIO;
        fclose(file);
        *source = CONFIG_FILE_ERROR;
        *raw_value = -1;
        return -saved_errno;
    }

    fclose(file);

    if (!found) {
        *source = CONFIG_MISSING_KEY;
        return DEFAULT_THRESHOLD_C;
    }

    *raw_value = value;

    if (value < 0) {
        *source = CONFIG_INVALID;
        return DEFAULT_THRESHOLD_C;
    }

    if (value < MIN_THRESHOLD_C || value > MAX_THRESHOLD_C) {
        *source = CONFIG_CLAMPED;
        return DEFAULT_THRESHOLD_C;
    }

    *source = CONFIG_FROM_INI;
    return value;
}

static const char *config_source_name(ConfigSource source)
{
    switch (source) {
        case CONFIG_FROM_INI:   return "ini";
        case CONFIG_CREATED:    return "ini created";
        case CONFIG_MISSING_KEY:return "default";
        case CONFIG_CLAMPED:    return "ini clamped";
        case CONFIG_INVALID:   return "ini invalid";
        case CONFIG_FILE_ERROR:return "config error";
        default:                return "unknown";
    }
}

/* -------------------------------------------------------------------------- */
/* ICC Helpers                                                                */
/* -------------------------------------------------------------------------- */

static int icc_open(void)
{
    /* The PS4 fan device is commonly opened read-only. */
    return open(ICC_FAN_DEVICE, O_RDONLY, 0);
}

static int icc_get_profile(uint8_t profile[ICC_PROFILE_SIZE])
{
    int fd;
    int rc;

    memset(profile, 0, ICC_PROFILE_SIZE);

    fd = icc_open();
    if (fd < 0)
        return -errno;

    rc = ioctl(fd, ICC_IOCTL_GET_THRESHOLD, profile);
    {
        int saved_errno = errno;
        close(fd);
        if (rc < 0)
            return -saved_errno;
    }

    return 0;
}

static int icc_set_profile(const uint8_t profile[ICC_PROFILE_SIZE])
{
    int fd;
    int rc;

    fd = icc_open();
    if (fd < 0)
        return -errno;

    rc = ioctl(fd, ICC_IOCTL_SET_THRESHOLD, profile);
    {
        int saved_errno = errno;
        close(fd);
        if (rc < 0)
            return -saved_errno;
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                      */
/* -------------------------------------------------------------------------- */

int main(void)
{
    uint8_t initial_profile[ICC_PROFILE_SIZE];
    uint8_t verify_profile[ICC_PROFILE_SIZE];
    ConfigSource source;
    int raw_value;
    int old_threshold;
    int new_threshold;
    int verify_threshold;
    int rc;
    char detail[512];
    char message[192];
    char initial_hex[ICC_PROFILE_SIZE * 3];
    char verify_hex[ICC_PROFILE_SIZE * 3];

    printf("[FanCtrl] Starting v%s\n", PAYLOAD_VERSION);

    /* Always record that the payload started, even if a later step fails. */
    write_log("START", "----------------------------------------");
    write_log("START", "Fan Control Payload Loaded");

    /* 1. Read the existing ICC profile. */
    rc = icc_get_profile(initial_profile);
    if (rc < 0) {
        snprintf(detail, sizeof(detail),
                 "Failed to read /dev/icc_fan: rc=%d errno=%d (%s)",
                 rc, -rc, strerror(-rc));
        printf("[FanCtrl] %s\n", detail);
        write_log("FAIL", detail);
        write_log("END", "Payload stopped before changing the fan threshold");
        notify_user("Fan Control Payload: ERROR - ICC read failed");
        return 1;
    }

    old_threshold = initial_profile[ICC_THRESHOLD_OFFSET];
    format_hex(initial_profile, sizeof(initial_profile), initial_hex, sizeof(initial_hex));

    printf("[FanCtrl] Current threshold: %dC\n", old_threshold);
    printf("[FanCtrl] ICC GET raw: %s\n", initial_hex);

    snprintf(detail, sizeof(detail),
             "ICC GET raw (%zu bytes): %s | threshold=%dC",
             sizeof(initial_profile), initial_hex, old_threshold);
    write_log("READ", detail);

    /* 2. Load the target from the INI file. */
    new_threshold = read_threshold(&source, &raw_value);
    if (new_threshold < MIN_THRESHOLD_C || new_threshold > MAX_THRESHOLD_C) {
        snprintf(detail, sizeof(detail),
                 "Failed to load a valid threshold from %s; using no fan write",
                 CONFIG_FILE);
        printf("[FanCtrl] %s\n", detail);
        write_log("FAIL", detail);
        write_log("END", "Payload stopped because configuration could not be loaded");
        notify_user("Fan Control Payload: ERROR - config load failed");
        return 1;
    }

    printf("[FanCtrl] Target threshold: %dC (%s)\n",
           new_threshold,
           config_source_name(source));

    if (source == CONFIG_CREATED)
        write_log("INFO", "Configuration file was missing; created with the default threshold");
    else if (source == CONFIG_MISSING_KEY)
        write_log("WARN", "Configuration file has no threshold= entry; using the default threshold");
    else if (source == CONFIG_INVALID)
        write_log("FAIL", "Configuration threshold is not a valid integer; using the default threshold");
    else if (source == CONFIG_CLAMPED)
        write_log("WARN", "Configuration threshold was outside the allowed 60-80C range; using the default threshold");
    else if (raw_value >= 0) {
        snprintf(detail, sizeof(detail),
                 "Configuration loaded: threshold=%dC",
                 raw_value);
        write_log("INFO", detail);
    }

    /* 3. Avoid an unnecessary write when the value is already correct. */
    if (old_threshold != new_threshold) {
        uint8_t profile[ICC_PROFILE_SIZE];

        memcpy(profile, initial_profile, sizeof(profile));
        profile[ICC_THRESHOLD_OFFSET] = (uint8_t)new_threshold;

        format_hex(profile, sizeof(profile), verify_hex, sizeof(verify_hex));
        snprintf(detail, sizeof(detail),
                 "ICC SET raw (%zu bytes): %s | threshold=%dC",
                 sizeof(profile), verify_hex, new_threshold);
        write_log("WRITE", detail);

        rc = icc_set_profile(profile);
        if (rc < 0) {
            snprintf(detail, sizeof(detail),
                     "Failed to write /dev/icc_fan: rc=%d errno=%d (%s); threshold remains %dC",
                     rc, -rc, strerror(-rc), old_threshold);
            printf("[FanCtrl] %s\n", detail);
            write_log("FAIL", detail);
            write_log("END", "Payload stopped after ICC write failure");
            notify_user("Fan Control Payload: ERROR - write failed");
            return 1;
        }
    } else {
        write_log("INFO", "Requested threshold matches the current threshold; skipped SET ioctl");
    }

    /* 4. Read back the profile and verify the threshold. */
    rc = icc_get_profile(verify_profile);
    if (rc < 0) {
        snprintf(detail, sizeof(detail),
                 "Verification read failed: rc=%d errno=%d (%s); a write may have occurred",
                 rc, -rc, strerror(-rc));
        printf("[FanCtrl] %s\n", detail);
        write_log("FAIL", detail);
        write_log("END", "Payload stopped because verification could not complete");
        notify_user("Fan Control Payload: ERROR - verify read failed");
        return 1;
    }

    verify_threshold = verify_profile[ICC_THRESHOLD_OFFSET];
    format_hex(verify_profile, sizeof(verify_profile), verify_hex, sizeof(verify_hex));
    printf("[FanCtrl] ICC GET verify raw: %s\n", verify_hex);
    snprintf(detail, sizeof(detail),
             "ICC VERIFY raw (%zu bytes): %s | threshold=%dC",
             sizeof(verify_profile), verify_hex, verify_threshold);
    write_log("VERIFY", detail);

    if (verify_threshold != new_threshold) {
        snprintf(detail, sizeof(detail),
                 "Verification failed: requested %dC but ICC reports %dC",
                 new_threshold, verify_threshold);
        printf("[FanCtrl] %s\n", detail);
        write_log("FAIL", detail);
        write_log("END", "Payload finished with verification failure");
        notify_user("Fan Control Payload: ERROR - value did not verify");
        return 1;
    }

    /* 5. Successful result. */
    if (raw_value >= 0) {
        snprintf(detail, sizeof(detail),
                 "SUCCESS: fan threshold is %dC (was %dC) | source=%s | ini=%dC",
                 new_threshold,
                 old_threshold,
                 config_source_name(source),
                 raw_value);
    } else {
        snprintf(detail, sizeof(detail),
                 "SUCCESS: fan threshold is %dC (was %dC) | source=%s",
                 new_threshold,
                 old_threshold,
                 config_source_name(source));
    }

    printf("[FanCtrl] %s\n", detail);
    write_log("OK", detail);

    snprintf(message, sizeof(message),
             "Fan Threshold: %dC set | was %dC",
             new_threshold,
             old_threshold);
    notify_user(message);

    write_log("END", "Fan Control Payload Completed Successfully");
    write_log("END", "----------------------------------------");

    return 0;
}
