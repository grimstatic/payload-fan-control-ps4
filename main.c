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

#define PAYLOAD_VERSION     "1.10"

#define WORK_DIR            "/data/fan_control"
#define CONFIG_FILE         WORK_DIR "/fan_control.ini"
#define LOG_FILE            WORK_DIR "/fan_control.log"

#define DEFAULT_THRESHOLD   65
#define MIN_THRESHOLD       60
#define MAX_THRESHOLD       80

#define ICC_DEVICE  "/dev/icc_fan"
#define ICC_GET     0xC01C8F08UL  /* ICC Fan Value */
#define ICC_SET     0xC01C8F07UL

#define PROFILE_BYTES  10
#define TEMP_BYTE       5  

typedef struct {
    int32_t  type;
    uint32_t req_id;
    uint32_t priority;
    uint32_t msg_id;
    int32_t  target_id;
    uint32_t user_id;
    uint32_t device_id;
    uint32_t addressing_user_id;
    uint32_t app_id;
    uint32_t error_number;
    uint32_t attribute;
    uint8_t  use_icon_uri;
    char     message[0x400];
    char     icon_uri[0x800];
    uint8_t  reserved[3];
} SceNotificationRequest;

_Static_assert(sizeof(SceNotificationRequest) == 0xC30,
               "SceNotificationRequest size wrong — kernel will ignore it");

extern int sceKernelSendNotificationRequest(int, SceNotificationRequest *, size_t, int);
extern int sceKernelUsleep(unsigned int);

/* ShellUI */

static void to_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    size_t pos = 0;
    if (!out_size) return;
    out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        int n = snprintf(out + pos, out_size - pos, "%s%02X", i ? " " : "", data[i]);
        if (n < 0 || (size_t)n >= out_size - pos) break;
        pos += (size_t)n;
    }
}

static int ensure_workdir(void)
{
    static int ready = 0;
    if (ready) return 0;
    if (mkdir(WORK_DIR, 0777) < 0 && errno != EEXIST)
        return -errno;
    ready = 1;
    return 0;
}

static void log_write(const char *level, const char *msg)
{
    int rc = ensure_workdir();
    if (rc < 0)
        fprintf(stderr, "[FanCtrl] mkdir %s: %d\n", WORK_DIR, -rc);

    FILE *f = fopen(LOG_FILE, "a");
    if (!f) { fprintf(stderr, "[FanCtrl] can't open log: %d\n", errno); return; }

    time_t     now = time(NULL);
    struct tm *t   = gmtime(&now);

    if (t)
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%-6s] %s\n",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec, level, msg);
    else
        fprintf(f, "[time unknown] [%-6s] %s\n", level, msg);

    fflush(f);
    fclose(f);
}

static void notify(const char *text)
{
    sceKernelUsleep(2000000);  /* ShellUI 2s Delay */

    SceNotificationRequest req = {0};
    req.target_id    = -1;
    req.use_icon_uri = 1;
    strncpy(req.message,  text,                                    sizeof(req.message)  - 1);
    strncpy(req.icon_uri, "cxml://psnotification/tex_icon_system", sizeof(req.icon_uri) - 1);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void fail(const char *detail, const char *end_msg, const char *notif)
{
    printf("[FanCtrl] %s\n", detail);
    log_write("FAIL", detail);
    log_write("END",  end_msg);
    notify(notif);
}

/* Logging */
static void log_icc(const char *log_level, const char *action, const char *hex, int temp)
{
    char detail[96];
    snprintf(detail, sizeof(detail), "ICC %s (%d bytes): %s  Threshold=%dC",
             action, PROFILE_BYTES, hex, temp);
    log_write(log_level, detail);
}

/* Config Creation */

typedef enum {
    FROM_INI,
    FILE_CREATED,
    KEY_MISSING,
    VALUE_CLAMPED,
    VALUE_INVALID,
    FILE_ERROR
} ConfigResult;

static const char *result_label(ConfigResult r)
{
    switch (r) {
        case FROM_INI:      return "ini";
        case FILE_CREATED:  return "created default";
        case KEY_MISSING:   return "missing key, used default";
        case VALUE_CLAMPED: return "out of range, used default";
        case VALUE_INVALID: return "bad value, used default";
        case FILE_ERROR:    return "file error";
        default:            return "??";
    }
}

static int write_default_config(void)
{
    int rc = ensure_workdir();
    if (rc < 0) return rc;

    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return -errno;

    fprintf(f,
            "# PS4 Fan Control\n"
            "# valid range: %d-%d C  (anything outside gets clamped to default)\n"
            "# lower = quieter, higher = hotter before ramp\n"
            "threshold=%d\n",
            MIN_THRESHOLD, MAX_THRESHOLD, DEFAULT_THRESHOLD);

    return fclose(f) ? -errno : 0;
}

/*
 * always returns a value safe to write to the ICC.
 * *result tells you why. *raw is what was literally in the file (-1 if never parsed).
 * returns negative only on ferror — caller's range check catches it as invalid.
 */
static int load_threshold(ConfigResult *result, int *raw)
{
    *result = FILE_CREATED;
    *raw    = -1;

    FILE *f = fopen(CONFIG_FILE, "r");
    if (!f) {
        if (write_default_config() < 0)
            *result = FILE_ERROR;
        return DEFAULT_THRESHOLD;
    }

    char line[256];
    int  value = -1, found = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\r' || *p == '\n' || *p == '#' || *p == ';') continue;
        if (strncmp(p, "threshold=", 10) != 0) continue;

        char *end;
        errno = 0;
        long parsed = strtol(p + 10, &end, 10);
        char *num_end = end;  /* where strtol actually stopped, before we chew trailing whitespace */
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;

        /* allow a trailing comment after the value, e.g. "threshold=65 # quiet mode" */
        found = 1;
        if (num_end != p + 10 && errno != ERANGE
                && (*end == '\0' || *end == '#' || *end == ';')
                && parsed >= INT_MIN && parsed <= INT_MAX)
            value = (int)parsed;
        break;
    }

    if (ferror(f)) {
        int err = errno ? errno : EIO;
        fclose(f);
        *result = FILE_ERROR;
        return -err;
    }
    fclose(f);

    if (!found)    { *result = KEY_MISSING;   return DEFAULT_THRESHOLD; }
    if (value < 0) { *result = VALUE_INVALID; return DEFAULT_THRESHOLD; }

    if (value < MIN_THRESHOLD || value > MAX_THRESHOLD) {
        *result = VALUE_CLAMPED;
        *raw    = value;
        return DEFAULT_THRESHOLD;
    }

    *result = FROM_INI;
    *raw    = value;
    return value;
}

/* ICC Helper */

static int icc_read(uint8_t profile[PROFILE_BYTES])
{
    memset(profile, 0, PROFILE_BYTES);
    int fd = open(ICC_DEVICE, O_RDONLY, 0);
    if (fd < 0) return -errno;
    int rc  = ioctl(fd, ICC_GET, profile);
    int err = errno;
    close(fd);
    return rc < 0 ? -err : 0;
}

static int icc_write(const uint8_t profile[PROFILE_BYTES])
{
    int fd = open(ICC_DEVICE, O_RDONLY, 0);
    if (fd < 0) return -errno;
    int rc  = ioctl(fd, ICC_SET, profile);
    int err = errno;
    close(fd);
    return rc < 0 ? -err : 0;
}

/* Main */

int main(void)
{
    char    detail[512];
    char    hex[PROFILE_BYTES * 3 + 1];  /* +1 slack, exact fit today but no reason to cut it close */
    uint8_t before[PROFILE_BYTES];
    uint8_t after[PROFILE_BYTES];

    printf("[FanCtrl] v%s\n", PAYLOAD_VERSION);
    log_write("START", "----------------------------------------");
    log_write("START", "Fan Control Payload Loaded");

    int rc = icc_read(before);
    if (rc < 0) {
        snprintf(detail, sizeof(detail), "icc_read failed: %d (%s)", -rc, strerror(-rc));
        fail(detail, "Stopped before touching anything", "Fan Control: ERROR — couldn't read ICC");
        return 1;
    }

    int old_temp = before[TEMP_BYTE];
    to_hex(before, PROFILE_BYTES, hex, sizeof(hex));
    printf("[FanCtrl] current: %dC  raw: %s\n", old_temp, hex);
    log_icc("READ", "GET", hex, old_temp);

    ConfigResult cfg;
    int raw;
    int new_temp = load_threshold(&cfg, &raw);

    if (cfg == FILE_ERROR) {
        snprintf(detail, sizeof(detail), "I/O error reading %s — Stopping", CONFIG_FILE);
        fail(detail, "Stopped — Config Read Failed", "Fan Control: ERROR — config I/O error");
        return 1;
    }
    if (new_temp < MIN_THRESHOLD || new_temp > MAX_THRESHOLD) {
        snprintf(detail, sizeof(detail), "Invalid threshold from %s — Stopping", CONFIG_FILE);
        fail(detail, "Stopped — Invalid Config ", "Fan Control: ERROR — Invalid config");
        return 1;
    }

    printf("[FanCtrl] target: %dC (%s)\n", new_temp, result_label(cfg));

    switch (cfg) {
        case FILE_CREATED:
            log_write("INFO", "No config — wrote default, edit it and re-run");
            break;
        case KEY_MISSING:
            log_write("WARN", "No threshold= in config — used default");
            break;
        case VALUE_INVALID:
            log_write("WARN", "Threshold= not a valid number — used default");
            break;
        case VALUE_CLAMPED:
            snprintf(detail, sizeof(detail),
                     "threshold=%d outside %d-%dC — used default (%dC)",
                     raw, MIN_THRESHOLD, MAX_THRESHOLD, DEFAULT_THRESHOLD);
            log_write("WARN", detail);
            break;
        default:
            if (raw >= 0) {
                snprintf(detail, sizeof(detail), "loaded threshold=%dC from ini", raw);
                log_write("INFO", detail);
            }
    }

    if (old_temp != new_temp) {
        /* 5 Byte Patch */
        uint8_t profile[PROFILE_BYTES];
        memcpy(profile, before, PROFILE_BYTES);
        profile[TEMP_BYTE] = (uint8_t)new_temp;

        to_hex(profile, PROFILE_BYTES, hex, sizeof(hex));
        log_icc("WRITE", "SET", hex, new_temp);

        rc = icc_write(profile);
        if (rc < 0) {
            snprintf(detail, sizeof(detail),
                     "icc_write failed: %d (%s) — still at %dC", -rc, strerror(-rc), old_temp);
            fail(detail, "stopped — write failed, nothing changed", "Fan Control: ERROR — write failed");
            return 1;
        }
    } else {
        log_write("INFO", "already at target — skipped write");
    }

    rc = icc_read(after);
    if (rc < 0) {
        snprintf(detail, sizeof(detail),
                 "verify read failed: %d (%s) — write result unknown", -rc, strerror(-rc));
        fail(detail, "stopped — couldn't verify", "Fan Control: ERROR — verify failed, check log");
        return 1;
    }

    int verify_temp = after[TEMP_BYTE];
    to_hex(after, PROFILE_BYTES, hex, sizeof(hex));
    printf("[FanCtrl] verify: %s\n", hex);
    log_icc("VERIFY", "VERIFY", hex, verify_temp);

    if (verify_temp != new_temp) {
        snprintf(detail, sizeof(detail),
                 "mismatch: wrote %dC, ICC returned %dC", new_temp, verify_temp);
        fail(detail, "verify failed — value may not have stuck",
             "Fan Control: ERROR — value didn't stick");
        return 1;
    }

    if (raw >= 0)
        snprintf(detail, sizeof(detail), "ok: %dC -> %dC  source=%s  ini=%dC",
                 old_temp, new_temp, result_label(cfg), raw);
    else
        snprintf(detail, sizeof(detail), "ok: %dC -> %dC  source=%s",
                 old_temp, new_temp, result_label(cfg));

    printf("[FanCtrl] %s\n", detail);
    log_write("OK", detail);

    char message[192];
    snprintf(message, sizeof(message), "Fan threshold: %dC  (was %dC)", new_temp, old_temp);
    notify(message);

    log_write("END", "Fan Control Payload Completed Successfully");
    log_write("END", "----------------------------------------");

    return 0;
}
