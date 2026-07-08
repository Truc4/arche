// wasm_encode.c — see wasm_encode.h. A small, correct WebAssembly binary writer.
#include "wasm_encode.h"
#include <stdlib.h>
#include <string.h>

// ---- buffer ----
void wb_init(WasmBuf *b) {
	b->data = NULL;
	b->len = b->cap = 0;
}
void wb_free(WasmBuf *b) {
	free(b->data);
	wb_init(b);
}
static void wb_reserve(WasmBuf *b, size_t extra) {
	if (b->len + extra <= b->cap)
		return;
	size_t cap = b->cap ? b->cap * 2 : 64;
	while (cap < b->len + extra)
		cap *= 2;
	b->data = realloc(b->data, cap);
	b->cap = cap;
}
void wb_byte(WasmBuf *b, uint8_t v) {
	wb_reserve(b, 1);
	b->data[b->len++] = v;
}
void wb_bytes(WasmBuf *b, const void *p, size_t n) {
	if (!n)
		return;
	wb_reserve(b, n);
	memcpy(b->data + b->len, p, n);
	b->len += n;
}
void wb_u(WasmBuf *b, uint64_t v) {
	do {
		uint8_t byte = v & 0x7f;
		v >>= 7;
		if (v)
			byte |= 0x80;
		wb_byte(b, byte);
	} while (v);
}
void wb_i(WasmBuf *b, int64_t v) {
	int more = 1;
	while (more) {
		uint8_t byte = v & 0x7f;
		v >>= 7; // arithmetic shift
		if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
			more = 0;
		else
			byte |= 0x80;
		wb_byte(b, byte);
	}
}
void wb_name(WasmBuf *b, const char *s) {
	size_t n = strlen(s);
	wb_u(b, n);
	wb_bytes(b, s, n);
}

// ---- module ----
void wm_init(WasmModule *m) {
	memset(m, 0, sizeof(*m));
	wb_init(&m->types);
	wb_init(&m->imports);
	wb_init(&m->funcs);
	wb_init(&m->globals);
	wb_init(&m->exports);
	wb_init(&m->code);
	wb_init(&m->data);
}
void wm_free(WasmModule *m) {
	wb_free(&m->types);
	wb_free(&m->imports);
	wb_free(&m->funcs);
	wb_free(&m->globals);
	wb_free(&m->exports);
	wb_free(&m->code);
	wb_free(&m->data);
}

int wm_type(WasmModule *m, const uint8_t *params, int np, const uint8_t *results, int nr) {
	wb_byte(&m->types, 0x60);
	wb_u(&m->types, np);
	for (int i = 0; i < np; i++)
		wb_byte(&m->types, params[i]);
	wb_u(&m->types, nr);
	for (int i = 0; i < nr; i++)
		wb_byte(&m->types, results[i]);
	return m->type_count++;
}

int wm_import_func(WasmModule *m, const char *field, int type_idx) {
	wb_name(&m->imports, "env");
	wb_name(&m->imports, field);
	wb_byte(&m->imports, 0x00 /*func*/);
	wb_u(&m->imports, type_idx);
	return m->import_count++; // func index == import order (all imports are funcs)
}

void wm_memory(WasmModule *m, int min_pages) {
	m->has_memory = 1;
	m->mem_min = min_pages;
}

int wm_global_i32(WasmModule *m, int is_mutable, int32_t init) {
	wb_byte(&m->globals, WT_I32);
	wb_byte(&m->globals, is_mutable ? 1 : 0);
	wb_byte(&m->globals, 0x41);
	wb_i(&m->globals, init);
	wb_byte(&m->globals, 0x0b); // i32.const init; end
	return m->global_count++;
}

int wm_func(WasmModule *m, int type_idx) {
	int idx = m->import_count + m->func_count;
	wb_u(&m->funcs, type_idx);
	m->func_count++;
	return idx;
}

void wm_code(WasmModule *m, const uint8_t *lt, const uint32_t *lc, int nlg, const WasmBuf *body) {
	WasmBuf fb;
	wb_init(&fb);
	wb_u(&fb, nlg);
	for (int i = 0; i < nlg; i++) {
		wb_u(&fb, lc[i]);
		wb_byte(&fb, lt[i]);
	}
	wb_bytes(&fb, body->data, body->len); // body already ends in 0x0b (end)
	wb_u(&m->code, fb.len);
	wb_bytes(&m->code, fb.data, fb.len);
	wb_free(&fb);
	m->code_count++;
}

void wm_export_func(WasmModule *m, const char *name, int func_idx) {
	wb_name(&m->exports, name);
	wb_byte(&m->exports, WK_FUNC);
	wb_u(&m->exports, func_idx);
	m->export_count++;
}
void wm_export_mem(WasmModule *m, const char *name) {
	wb_name(&m->exports, name);
	wb_byte(&m->exports, WK_MEM);
	wb_u(&m->exports, 0);
	m->export_count++;
}
void wm_export_global(WasmModule *m, const char *name, int gi) {
	wb_name(&m->exports, name);
	wb_byte(&m->exports, WK_GLOBAL);
	wb_u(&m->exports, gi);
	m->export_count++;
}
void wm_start(WasmModule *m, int func_idx) {
	m->has_start = 1;
	m->start_idx = func_idx;
}

void wm_data(WasmModule *m, uint32_t offset, const void *bytes, size_t n) {
	wb_byte(&m->data, 0x00); // active, memory 0
	wb_byte(&m->data, 0x41);
	wb_i(&m->data, (int32_t)offset);
	wb_byte(&m->data, 0x0b); // offset expr
	wb_u(&m->data, n);
	wb_bytes(&m->data, bytes, n);
	m->data_count++;
}

// section = [id][size][ leb(count) + content ]
static void emit_vec_section(WasmBuf *out, uint8_t id, int count, const WasmBuf *content) {
	if (count == 0)
		return;
	WasmBuf s;
	wb_init(&s);
	wb_u(&s, count);
	wb_bytes(&s, content->data, content->len);
	wb_byte(out, id);
	wb_u(out, s.len);
	wb_bytes(out, s.data, s.len);
	wb_free(&s);
}

void wm_finish(WasmModule *m, WasmBuf *out) {
	wb_init(out);
	static const uint8_t hdr[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
	wb_bytes(out, hdr, 8);
	emit_vec_section(out, 1, m->type_count, &m->types);
	emit_vec_section(out, 2, m->import_count, &m->imports);
	emit_vec_section(out, 3, m->func_count, &m->funcs);
	if (m->has_memory) { // memory section: vec of 1 limits
		WasmBuf s;
		wb_init(&s);
		wb_u(&s, 1);
		wb_byte(&s, 0x00);
		wb_u(&s, m->mem_min);
		wb_byte(out, 5);
		wb_u(out, s.len);
		wb_bytes(out, s.data, s.len);
		wb_free(&s);
	}
	emit_vec_section(out, 6, m->global_count, &m->globals);
	emit_vec_section(out, 7, m->export_count, &m->exports);
	if (m->has_start) {
		WasmBuf s;
		wb_init(&s);
		wb_u(&s, m->start_idx);
		wb_byte(out, 8);
		wb_u(out, s.len);
		wb_bytes(out, s.data, s.len);
		wb_free(&s);
	}
	emit_vec_section(out, 10, m->code_count, &m->code);
	emit_vec_section(out, 11, m->data_count, &m->data);
}
