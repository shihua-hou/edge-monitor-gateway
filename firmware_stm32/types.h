#ifndef __TYPES_H
#define __TYPES_H

/* 沿用项目里原来的短类型名（原本来自正点原子标准库版本的 sys.h），
 * 换成 HAL 工程后这些类型不再随库自带，这里用 stdint.h 重新定义一份，
 * 这样 can.c / app_task.c / sensor 驱动里已有的代码不用逐处改类型名 */
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

#endif
