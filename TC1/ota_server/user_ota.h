#ifndef __USER_OTA_H_
#define __USER_OTA_H_

extern volatile int ota_progress;

void UserOtaStart(char *url, char *md5);

/* 校验 OTA_TEMP(被动分区) 固件镜像头 (返回 1 合法, 0 非法) */
int OtaImageHeaderValid(void);

#endif
