#ifndef SPACE_DESCRIPTOR_H_
#define SPACE_DESCRIPTOR_H_

#include <uapi/shapertools/mcu-ioctl.h>

#define SPACE_DESCRIPTOR_MAX_NAME_SIZE 64UL
#define SPACE_DESCRIPTOR_MAGIC 0x1371

extern const char *const field_descriptor_type_str[];

struct field_descriptor
{
	char name[MCU_DESC_MAX_NAME_SIZE];
	enum mcu_desc_field_type type;
	size_t size;
	size_t offset;
};

struct space_descriptor
{
	char name[MCU_DESC_MAX_NAME_SIZE];
	unsigned int id;
	size_t size;
	int num_fields;
	struct field_descriptor fields[];
};

struct space_descriptor **space_descriptor_parse(const void *buffer);
void space_descriptor_release(struct space_descriptor **descs);

#endif
