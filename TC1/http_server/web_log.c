#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mico.h"


#include"http_server/web_log.h"

static mico_mutex_t log_mutex;

LogRecord log_record = { 1,{ 0 } };
char log_record_str[LOG_NUM * (LOG_LEN + 4) + 64] = { 0 };

void LogMutexInit(void)
{
    mico_rtos_init_mutex(&log_mutex);
}

void SetLogRecord(LogRecord* lr, char* log)
{
    mico_rtos_lock_mutex(&log_mutex);
    if (strlen(log) > LOG_LEN)
    {
        log[LOG_LEN-1] = 0;
    }
    char** p_log = &lr->logs[(++lr->idx)% LOG_NUM];
    if (*p_log)
    {
        free(*p_log);
    }
    *p_log = log;
    mico_rtos_unlock_mutex(&log_mutex);
}

char* GetLogRecord()
{
    mico_rtos_lock_mutex(&log_mutex);
    int i = log_record.idx - LOG_NUM + 1;
    i = i < 0 ? 0 : i;
    size_t remaining = sizeof(log_record_str);
    char* tmp = log_record_str;
    int written = snprintf(tmp, remaining, "%d\n", log_record.idx);
    if (written > 0) { tmp += written; remaining -= (written < (int)remaining ? written : (int)remaining); }
    for (; i <= log_record.idx && remaining > 1; i++)
    {
        if (!log_record.logs[i%LOG_NUM]) continue;
        written = snprintf(tmp, remaining, "%s\n", log_record.logs[i%LOG_NUM]);
        if (written > 0) { tmp += written; remaining -= (written < (int)remaining ? written : (int)remaining); }
        if(i == log_record.idx && remaining > 1){
            written = snprintf(tmp, remaining, "%s\nFreeMem %d bytes\n", log_record.logs[i%LOG_NUM], MicoGetMemoryInfo()->free_memory);
            if (written > 0) { tmp += written; remaining -= (written < (int)remaining ? written : (int)remaining); }
        }
    }
    mico_rtos_unlock_mutex(&log_mutex);
    return log_record_str;
}

void WebLog(const char *M, ...)
{
    char* buff = (char*)malloc(sizeof(char)*LOG_LEN);

    time_t now = time(NULL) + 28800;
    strftime(buff, TIME_LEN, "[%Y-%m-%d %H:%M:%S]", localtime(&now));
    buff[TIME_LEN - 1] = ' ';

    va_list ap;
    va_start(ap, M);
    vsnprintf(buff + TIME_LEN, LOG_LEN - TIME_LEN, M, ap);
    va_end(ap);

    SetLogRecord(&log_record, buff);
}

