#ifndef __USER_OTA_H_
#define __USER_OTA_H_

extern volatile int ota_progress;

void UserOtaStart(char *url, char *md5);

#endif
