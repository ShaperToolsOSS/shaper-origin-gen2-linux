#ifndef _SHAPER_MCU_LOG_H_
#define _SHAPER_MCU_LOG_H_

#define MCU_LOG_NONE 0U
#define MCU_LOG_FATAL 1U
#define MCU_LOG_ERROR 2U
#define MCU_LOG_WARN 3U
#define MCU_LOG_INFO 4U
#define MCU_LOG_DEBUG 5U
#define MCU_LOG_TRACE 6U

#define MCU_LOG_CALLER_SIZE 32UL
#define MCU_LOG_MESSAGE_SIZE 128UL

struct log_msg
{
	uint32_t sec;
	uint32_t nsec;
	int32_t level;
	char caller[MCU_LOG_CALLER_SIZE];
	char msg_str[MCU_LOG_MESSAGE_SIZE];
} __attribute__((packed));

#endif
