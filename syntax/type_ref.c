#include "type_ref.h"
#include <stdlib.h>
#include <string.h>

/* =========================
   Types (TypeRef)
   ========================= */

TypeRef *type_name_create(char *name) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_NAME;
	type->data.name = name;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

TypeRef *type_array_create(TypeRef *element_type) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_ARRAY;
	type->data.array.element_type = element_type;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

TypeRef *type_shaped_array_create(TypeRef *element_type, int rank) {
	TypeRef *type = malloc(sizeof(TypeRef));
	type->kind = TYPE_SHAPED_ARRAY;
	type->data.shaped_array.element_type = element_type;
	type->data.shaped_array.rank = rank;
	type->loc.line = 1;
	type->loc.column = 1;
	return type;
}

void type_ref_free(TypeRef *type) {
	if (!type)
		return;
	switch (type->kind) {
	case TYPE_NAME:
		free(type->data.name);
		break;
	case TYPE_ARRAY:
		type_ref_free(type->data.array.element_type);
		break;
	case TYPE_SHAPED_ARRAY:
		type_ref_free(type->data.shaped_array.element_type);
		break;
	case TYPE_TUPLE:
		for (int i = 0; i < type->data.tuple.field_count; i++) {
			free(type->data.tuple.field_names[i]);
			type_ref_free(type->data.tuple.field_types[i]);
		}
		free(type->data.tuple.field_names);
		free(type->data.tuple.field_types);
		break;
	case TYPE_HANDLE:
		free(type->data.handle.archetype_name);
		break;
	case TYPE_ARCHETYPE:
		break;
	case TYPE_OPAQUE:
		break;
	case TYPE_TYPE:
		break;
	case TYPE_PROC:
	case TYPE_FUNC:
		/* A callable signature owns its param/result TypeRefs and the arrays holding them. */
		for (int i = 0; i < type->data.callable.param_count; i++)
			type_ref_free(type->data.callable.param_types[i]);
		free(type->data.callable.param_types);
		for (int i = 0; i < type->data.callable.result_count; i++)
			type_ref_free(type->data.callable.result_types[i]);
		free(type->data.callable.result_types);
		break;
	}
	free(type);
}
