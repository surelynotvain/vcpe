/* VCPE log file (see vcpe_log.h). */
#include <stdarg.h>
#include <stdio.h>
#include "vcpe_sys.h"
#include "vcpe_log.h"

static FILE *logf_;
static int buffered;

int vcpe_log_open(const char *file)
{
    char p[300];
    vcpe_path(p, sizeof p, file);
    if (logf_)
        fclose(logf_);
    logf_ = fopen(p, "w");
    return logf_ ? 0 : -1;
}

void vcpe_log(const char *fmt, ...)
{
    if (!logf_)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf_, fmt, ap);
    va_end(ap);
    fputc('\n', logf_);
    if (!buffered)
        fflush(logf_);
}

/* vcpe_fatal writes its message here before it takes the screen (weak link in vcpe_sys) */
void vcpe_log_line(const char *s)
{
    if (!logf_)
        return;
    fputs(s, logf_);
    fputc('\n', logf_);
    fflush(logf_);
}

void vcpe_log_buffered(int on) { buffered = on; if (!on) vcpe_log_flush(); }
void vcpe_log_flush(void) { if (logf_) fflush(logf_); }
int vcpe_log_is_open(void) { return logf_ != 0; }

void vcpe_log_close(void)
{
    if (logf_)
        fclose(logf_);
    logf_ = 0;
}
