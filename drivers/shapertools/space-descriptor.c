/*#define DEBUG*/

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/sort.h>

#include "space-descriptor.h"
#include "capnp/mcu-descriptor-capnp.h"
#include "capnp/capnp_c.h"

const char *const field_descriptor_type_str[] =
{
	/* Base types */
	"float",
	"double",
	"char",
	"int8_t",
	"uint8_t",
	"int16_t",
	"uint16_t",
	"int32_t",
	"uint32_t",
	"int64_t",
	"uint64_t",

	/* Array Types */
	"float",
	"double",
	"char",
	"int8_t",
	"uint8_t",
	"int16_t",
	"uint16_t",
	"int32_t",
	"uint32_t",
	"int64_t",
	"uint64_t",

	/* Triple types */
	"float_triple_t",
	"double_triple_t",
	"int8_triple_t",
	"uint8_triple_t",
	"int16_triple_t",
	"uint16_triple_t",
	"int32_triple_t",
	"uint32_triple_t",
	"int64_triple_t",
	"uint64_triple_t",
};

static int compare_space_id(const void *first, const void *second)
{
	const struct space_descriptor *first_desc = *(const struct space_descriptor **)first;
	const struct space_descriptor *second_desc = *(const struct space_descriptor **)second;

	if (first_desc->id < second_desc->id)
		return -1;
	else if (first_desc->id > second_desc->id)
		return 1;
	return 0;
}

struct space_descriptor **space_descriptor_parse(const void *buffer)
{
	struct capn arena;
	McuDescriptors_ptr mcu_descs;
	SpaceDescriptor_ptr space;
	SpaceDescriptor_list spaces;
	SpaceField_ptr field;
	SpaceField_list fields;
	int s;
	int f;
	struct space_descriptor **descs;
	size_t len;

	/* Extract the descriptor length */
	const uint16_t *desc_magic = buffer;
	if ((desc_magic[0] ^ desc_magic[1]) != SPACE_DESCRIPTOR_MAGIC)
		return (void *)(-EINVAL);
	len = desc_magic[0];
	buffer += 4;

	/* Initialize the arena for the descriptor */
	if (capn_init_mem(&arena, buffer, len, 1) < 0)
		return 0;

	/* Extract the root descriptor */
	mcu_descs.p = capn_getp(capn_root(&arena), 0, 0);
	if (mcu_descs.p.type == CAPN_NULL)
		goto error_free_capn;

	/* Extract the space pointer */
	spaces = McuDescriptors_get_spaces(mcu_descs);
	if (spaces.p.type == CAPN_NULL)
		goto error_free_capn;

	/* Allocate the descriptor with enough room for pointer to each space descriptor */
	descs = kzalloc(capn_len(spaces) * sizeof(struct space_descriptor *) + 1, GFP_KERNEL);
	if (!descs)
		return (void *)(-ENOMEM);

	/* Loop through each space */
	for (s = 0; s < capn_len(spaces); ++s) {

		/* Extract the current space */
		space.p = capn_getp(spaces.p, s, 1);
		if (space.p.type == CAPN_NULL)
			goto error_desc_destroy;

		/* Extract the fields */
		fields = SpaceDescriptor_get_fields(space);

		/* Allocate the space descriptor */
		descs[s] = kzalloc(sizeof(struct space_descriptor) + (capn_len(fields) * sizeof(struct field_descriptor)), GFP_KERNEL);
		if (!descs[s])
			goto error_desc_destroy;

		/* Initialize the space */
		descs[s]->id = SpaceDescriptor_get_id(space);
		descs[s]->size = SpaceDescriptor_get_size(space);
		descs[s]->num_fields = capn_len(fields);
		scnprintf(descs[s]->name, sizeof(descs[s]->name), "%s", SpaceDescriptor_get_name(space).str);

		/* Loop through each field creating */
		for (f = 0; f < descs[s]->num_fields; ++f) {

			/* Extract the current field */
			field.p = capn_getp(fields.p, f, 1);
			if (field.p.type == CAPN_NULL)
				goto error_desc_destroy;

			/* Initialize the field */
			descs[s]->fields[f].type = SpaceField_get_type(field);
			descs[s]->fields[f].size = SpaceField_get_size(field);
			descs[s]->fields[f].offset = SpaceField_get_offset(field);
			scnprintf(descs[s]->fields[f].name, sizeof(descs[s]->fields[f].name), "%s", SpaceField_get_name(field).str);
		}
	}

	/* Release the arena */
	capn_free(&arena);

	/* Sort the space descriptors */
	sort(descs, capn_len(spaces), sizeof(struct space_descriptor *), compare_space_id, 0);

	/* Return the new descriptor */
	return descs;

error_desc_destroy:
	space_descriptor_release(descs);

error_free_capn:
	/* Release the arena */
	capn_free(&arena);

	return 0;
}

void space_descriptor_release(struct space_descriptor **descs)
{
	struct space_descriptor **cur = descs;
	while (*cur)
		kfree(*cur++);
	kfree(descs);
}
