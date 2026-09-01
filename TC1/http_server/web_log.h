#include <time.h>

#ifndef WEB_LOG_H
#define WEB_LOG_H

#define LOG_NUM 50
#define LOG_LEN 256
#define TIME_LEN 22

typedef struct
{
    int idx;
    char* logs[LOG_NUM];
} LogRecord;

void SetLogRecord(LogRecord* lr, char* log);
char* GetLogRecord(int since);
void WebLog(const char *M, ...);
void LogMutexInit(void);

extern LogRecord log_record;
#define web_log(N, M, ...) \
    do { \
        char *_log_buf = (char*)malloc(sizeof(char)*LOG_LEN); \
        if (_log_buf) { \
            time_t _log_now = time(NULL) + 28800; \
            strftime(_log_buf, TIME_LEN, "[%Y-%m-%d %H:%M:%S]", localtime(&_log_now)); \
            _log_buf[TIME_LEN - 1] = ' '; \
            snprintf(_log_buf + TIME_LEN, LOG_LEN - TIME_LEN, "["N" %s:%d] "M, SHORT_FILE, __LINE__, ##__VA_ARGS__); \
            SetLogRecord(&log_record, _log_buf); \
        } \
    } while(0)

#define web_log0(N, M, ...) WebLog("["N" %s:%d] "M, SHORT_FILE, __LINE__, ##__VA_ARGS__)

#endif // !WEB_LOG_H
